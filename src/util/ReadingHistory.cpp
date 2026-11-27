#include "ReadingHistory.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace readingHistory {
namespace {
// ArduinoJson stops at the first complete object; track input consumption to
// reject a second object or trailing garbage on the same JSONL line.
struct LineReader {
  const char* data;
  size_t size;
  size_t position = 0;

  int read() { return position == size ? -1 : static_cast<unsigned char>(data[position++]); }
  size_t readBytes(char* out, size_t count) {
    const size_t amount = std::min(count, size - position);
    std::memcpy(out, data + position, amount);
    position += amount;
    return amount;
  }
};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool decodeDocumentId(std::string_view text, DocumentId& id) {
  if (text.size() != id.size() * 2) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    const int hi = hexDigit(text[i * 2]);
    const int lo = hexDigit(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    id[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

namespace {
bool inWindow(uint32_t day, uint32_t anchor) { return day && day <= anchor && anchor - day < HISTORY_DAYS; }

void add(Totals& totals, uint32_t tracked, uint32_t seconds, bool dated, unsigned period, bool haveTurns,
         uint32_t turns) {
  ++totals.sessions;
  totals.seconds += tracked;
  totals.turns += turns;
  totals.turnsComplete = totals.turnsComplete && haveTurns;
  if (dated) {
    ++totals.datedSessions;
    totals.periods[period] += seconds;
  }
}
}  // namespace

Status Query::begin(std::string_view document, uint32_t anchorDay, bool details) {
  stop(Status::Idle);
  detailedBooks = details;
  result = {};
  lineOffset = 0;
  baselineOpen = baselineGlobal = false;
  baselineRemaining = 0;
  baselineHash = 2166136261u;
  week = {};
  selected = {};
  hasSelected = !document.empty();
  if (hasSelected && !decodeDocumentId(document, selected)) {
    state = Status::InvalidQuery;
    return state;
  }
  result.anchorDay = anchorDay;
  for (size_t i = 0; i < days.size(); ++i) {
    days[i] = makeUniqueNoThrow<Day[]>(dayBlockSize(i));
    if (!days[i]) {
      LOG_ERR("RHIST", "Cannot allocate day block %u (%u bytes)", static_cast<unsigned>(i),
              static_cast<unsigned>(dayBlockSize(i) * sizeof(Day)));
      stop(Status::NoMemory);
      return state;
    }
  }
  line = makeUniqueNoThrow<char[]>(MAX_LINE + 1);
  filter["doc"] = true;
  filter["secs"] = true;
  filter["start"] = true;
  filter["tz"] = true;
  filter["type"] = true;
  filter["v"] = true;
  filter["tracked_secs"] = true;
  filter["turns"] = true;
  filter["device"] = true;
  filter["history_id"] = true;
  if (!line || filter.overflowed()) {
    LOG_ERR("RHIST", "Cannot allocate %u-byte line or JSON filter", static_cast<unsigned>(MAX_LINE + 1));
    stop(Status::NoMemory);
    return state;
  }
  state = Status::Scanning;
  return state;
}

void Query::releaseParser() {
  line.reset();
  filter.clear();
  parsed.clear();
  length = 0;
  overflow = false;
}

void Query::stop(Status reason) {
  releaseParser();
  for (auto& block : ids) block.reset();
  for (auto& block : bookSeconds) block.reset();
  for (auto& block : days) block.reset();
  state = reason == Status::Ready || reason == Status::Scanning ? Status::Idle : reason;
}

void Query::countBook(const DocumentId& id, uint64_t seconds) {
  size_t lo = 0, hi = result.books;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (entryAt(mid).id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  const uint32_t offset = lineOffset < UINT32_MAX ? static_cast<uint32_t>(lineOffset) : UINT32_MAX;
  if (lo < result.books && entryAt(lo).id == id) {
    entryAt(lo).offset = offset;
    if (detailedBooks) secondsAt(lo) += seconds;
    return;
  }
  if (result.books == MAX_BOOKS) {
    result.booksLimited = true;
    if (lo == MAX_BOOKS) return;
  } else {
    // Allocate only as many small blocks as this history needs.
    auto& block = ids[result.books / BOOK_BLOCK];
    if (!block) {
      block = makeUniqueNoThrow<BookEntry[]>(BOOK_BLOCK);
      auto& totals = bookSeconds[result.books / BOOK_BLOCK];
      if (detailedBooks) totals = makeUniqueNoThrow<uint64_t[]>(BOOK_BLOCK);
      if (!block || (detailedBooks && !totals)) {
        LOG_ERR("RHIST", "Cannot allocate %u-byte document block",
                static_cast<unsigned>(BOOK_BLOCK * sizeof(BookEntry)));
        stop(Status::NoMemory);
        return;
      }
    }
    ++result.books;
  }
  for (size_t i = result.books - 1; i > lo; --i) {
    entryAt(i) = entryAt(i - 1);
    if (detailedBooks) secondsAt(i) = secondsAt(i - 1);
  }
  entryAt(lo) = {id, offset};
  if (detailedBooks) secondsAt(lo) = seconds;
}

void Query::rejectLine() {
  if (baselineOpen) {
    LOG_ERR("RHIST", "Invalid historical baseline; refusing partial results");
    stop(Status::IoError);
  } else {
    ++result.rejected;
  }
}

void Query::processBaseline() {
  // Parse migration records separately; do not grow the stock-session JSON filter.
  LineReader reader{line.get(), length};
  const auto error = deserializeJson(parsed, reader, DeserializationOption::NestingLimit(4));
  auto invalid = [&]() {
    LOG_ERR("RHIST", "Invalid historical baseline at byte %llu", static_cast<unsigned long long>(lineOffset));
    stop(Status::IoError);
  };
  if (error == DeserializationError::NoMemory) {
    stop(Status::NoMemory);
    return;
  }
  if (error || reader.position != length || !parsed.is<JsonObject>() || !parsed["v"].is<uint32_t>() ||
      parsed["v"].as<uint32_t>() != 1) {
    invalid();
    return;
  }
  const std::string_view kind = parsed["type"] | "";
  if (kind == "rc-baseline-begin") {
    const auto count = parsed["records"];
    // At most 2048 source books, each with 366 day records, a total,
    // plus the global and seven weekly rows. Never accept a second cutover.
    if (baselineOpen || result.baselineApplied || !count.is<uint32_t>() || !count.as<uint32_t>() ||
        count.as<uint32_t>() > 800000) {
      invalid();
      return;
    }
    baselineOpen = true;
    baselineGlobal = false;
    baselineRemaining = count.as<uint32_t>();
    baselineHash = 2166136261u;
    // The legacy snapshots are absolute local totals as of this boundary,
    // NOT a delta. Keep original JSONL bytes for upstream upload, but replace
    // their local aggregate contribution rather than counting them twice.
    const auto bytes = result.bytes, rejected = result.rejected;
    const auto anchor = result.anchorDay;
    const auto sourceId = result.sourceId;
    const auto sourceMac = result.sourceMac;
    const bool conflict = result.sourceConflict || sourceId != DocumentId{};
    result = {};
    result.sourceId = sourceId;
    result.sourceMac = sourceMac;
    result.sourceConflict = conflict;
    result.bytes = bytes;
    result.rejected = rejected;
    result.anchorDay = anchor;
    for (size_t i = 0; i < days.size(); ++i) std::fill_n(days[i].get(), dayBlockSize(i), Day{});
    week = {};
    for (auto& block : ids) block.reset();
    for (auto& block : bookSeconds) block.reset();
    return;
  }
  if (!baselineOpen) {
    invalid();
    return;
  }
  if (kind == "rc-baseline-end") {
    if (baselineRemaining || !baselineGlobal || !parsed["hash"].is<uint32_t>() ||
        parsed["hash"].as<uint32_t>() != baselineHash) {
      invalid();
      return;
    }
    baselineOpen = false;
    result.baselineApplied = true;
    return;
  }
  if (!baselineRemaining) {
    invalid();
    return;
  }
  --baselineRemaining;
  for (size_t i = 0; i < length; ++i) baselineHash = (baselineHash ^ static_cast<uint8_t>(line[i])) * 16777619u;
  baselineHash = (baselineHash ^ uint8_t('\n')) * 16777619u;
  const auto doc = parsed["doc"];
  const bool global = doc.isNull();
  DocumentId id{};
  if (!global) {
    const auto value = doc.as<JsonString>();
    if (value.isNull() || !decodeDocumentId({value.c_str(), value.size()}, id)) {
      invalid();
      return;
    }
  }
  const bool isBook = !global && hasSelected && selected == id;
  auto dayValue = [&](const char* key, uint32_t& day, bool allowZero) {
    if (!parsed[key].is<uint32_t>()) return false;
    day = parsed[key].as<uint32_t>();
    return (allowZero && !day) || (day >= 10957 && day <= 47482);
  };
  auto periods = [&](uint64_t (&out)[4]) {
    const auto values = parsed["periods"].as<JsonArrayConst>();
    if (values.size() != 4) return false;
    unsigned i = 0;
    for (const auto value : values) {
      if (!value.is<uint32_t>()) return false;
      out[i++] = value.as<uint32_t>();
    }
    return true;
  };
  if (kind == "rc-baseline-total") {
    uint32_t day;
    Totals total;
    if ((!baselineGlobal && !global) || (baselineGlobal && global) || !parsed["seconds"].is<uint64_t>() ||
        !parsed["sessions"].is<uint32_t>() || !parsed["turns"].is<uint32_t>() || !dayValue("latest", day, true) ||
        !periods(total.periods)) {
      invalid();
      return;
    }
    total.seconds = parsed["seconds"].as<uint64_t>();
    total.sessions = parsed["sessions"].as<uint32_t>();
    total.turns = parsed["turns"].as<uint32_t>();
    // Legacy aggregate snapshots do not contain the number of dated sessions.
    // Do not invent it. Page turns remain in the JSONL for an extended API.
    result.latestDay = std::max(result.latestDay, day);
    if (global) {
      baselineGlobal = true;
      result.overall = total;
    } else {
      // Migration emits one total per distinct document. Reject duplicates
      // that would otherwise silently replace the selected book totals.
      for (size_t i = 0; i < result.books; ++i) {
        if (entryAt(i).id == id) {
          invalid();
          return;
        }
      }
      if (isBook) result.book = total;
      countBook(id, total.seconds);
    }
    return;
  }
  if (!baselineGlobal) {
    invalid();
    return;
  }
  uint32_t day;
  if (!dayValue("day", day, false)) {
    invalid();
    return;
  }
  // Keep raw dates within the ring. The final anchor is determined after
  // the entire checkpoint, not from the order of book rows.
  if (kind == "rc-baseline-day") {
    if (!parsed["seconds"].is<uint32_t>()) {
      invalid();
      return;
    }
    if (!global && !isBook) return;
    const auto seconds = parsed["seconds"].as<uint32_t>();
    const auto anchor = result.anchorDay ? result.anchorDay : std::max(day, result.latestDay);
    if (!inWindow(day, anchor)) return;
    auto& d = daySlot(day % HISTORY_DAYS);
    if (d.date != day) {
      if (d.date > day) return;
      d = {0, 0, day};
    }
    (global ? d.seconds : d.bookSeconds) = seconds;
  } else if (kind == "rc-baseline-week") {
    uint64_t values[4]{};
    if (!periods(values)) {
      invalid();
      return;
    }
    if (!global && !isBook) return;
    const auto anchor = result.anchorDay ? result.anchorDay : result.latestDay;
    if (day > anchor || anchor - day >= week.size()) return;
    auto& d = week[day % week.size()];
    if (d.date > day) return;
    d.date = day;
    std::copy_n(values, 4, global ? d.periods : d.bookPeriods);
  } else {
    invalid();
  }
}

void Query::processLine() {
  if (length && line[length - 1] == '\r') --length;
  if (overflow || !length || line[0] != '{' || line[length - 1] != '}') {
    rejectLine();
    return;
  }

  LineReader reader{line.get(), length};
  const auto error =
      deserializeJson(parsed, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8));
  if (error == DeserializationError::NoMemory) {
    LOG_ERR("RHIST", "Cannot allocate JSON parser for %u-byte line", static_cast<unsigned>(length));
    stop(Status::NoMemory);
    return;
  }
  if (error || reader.position != length || !parsed.is<JsonObject>()) {
    rejectLine();
    return;
  }
  const auto type = parsed["type"];
  const std::string_view kind = type.is<const char*>() ? type.as<const char*>() : "";
  if (kind.starts_with("rc-baseline-")) {
    processBaseline();
    return;
  }
  if (baselineOpen) {
    rejectLine();
    return;
  }
  const bool owned = result.sourceId != DocumentId{};
  if (kind == "rc-history-owner" || type.isNull() || kind == "session") {
    const auto mac = parsed["device"].as<JsonString>();
    if (!mac.isNull() && mac.size() == 12 &&
        std::string_view(mac.c_str(), mac.size()).find_first_not_of("0123456789abcdef") == std::string_view::npos &&
        std::string_view(mac.c_str(), mac.size()) != "000000000000") {
      if (result.sourceMac[0] && memcmp(result.sourceMac.data(), mac.c_str(), 12)) result.sourceConflict = true;
      memcpy(result.sourceMac.data(), mac.c_str(), 12);
    } else if (!parsed["device"].isNull() || owned || kind == "rc-history-owner") {
      result.sourceConflict = true;
    }
    if (kind == "rc-history-owner") {
      const auto id = parsed["history_id"].as<JsonString>();
      DocumentId value{};
      if (owned || parsed["v"] != 1 || id.isNull() || !decodeDocumentId({id.c_str(), id.size()}, value) ||
          value == DocumentId{})
        result.sourceConflict = true;
      else
        result.sourceId = value;
      return;
    }
    if (!parsed["v"].isNull() && parsed["v"] != 1 && parsed["v"] != 2) result.sourceConflict = true;
  } else {
    result.sourceConflict = true;
    return;
  }
  const JsonString doc = parsed["doc"].as<JsonString>();
  DocumentId id{};
  const auto secs = parsed["secs"];
  if (doc.isNull() || !decodeDocumentId(std::string_view(doc.c_str(), doc.size()), id) || !secs.is<uint32_t>() ||
      secs.as<uint32_t>() < 10 || secs.as<uint32_t>() > 604800) {
    ++result.rejected;
    return;
  }
  const uint32_t seconds = secs.as<uint32_t>();
  const bool extended = parsed["v"].is<uint32_t>() && parsed["v"].as<uint32_t>() == 2;
  const auto trackedField = parsed["tracked_secs"];
  const auto turnsField = parsed["turns"];
  if (extended && (!trackedField.is<uint32_t>() || trackedField.as<uint32_t>() < seconds ||
                   trackedField.as<uint32_t>() > 604800 || !turnsField.is<uint32_t>())) {
    ++result.rejected;
    return;
  }
  const uint32_t tracked = extended ? trackedField.as<uint32_t>() : seconds;
  const uint32_t turns = extended ? turnsField.as<uint32_t>() : 0;
  const auto start = parsed["start"];
  const auto tz = parsed["tz"];
  const int64_t epoch = start.is<int64_t>() ? start.as<int64_t>() : 0;
  const int32_t offset = tz.is<int32_t>() ? tz.as<int32_t>() : 0;
  const bool dated = epoch >= 946684800 && epoch <= 4102444799LL && tz.is<int32_t>() && offset >= -720 && offset <= 840;
  const bool isBook = hasSelected && selected == id;
  uint32_t day = 0;
  unsigned period = 0;
  if (dated) {
    const int64_t local = epoch + static_cast<int64_t>(offset) * 60;
    day = static_cast<uint32_t>(local / 86400);
    const auto hour = local % 86400 / 3600;
    period = hour < 6 || hour >= 21 ? 3 : hour < 12 ? 0 : hour < 17 ? 1 : 2;
    result.latestDay = std::max(result.latestDay, day);
  }
  add(result.overall, tracked, seconds, dated, period, extended, turns);
  if (isBook) add(result.book, tracked, seconds, dated, period, extended, turns);
  countBook(id, tracked);
  if (state != Status::Scanning) return;
  const uint32_t anchor = result.anchorDay ? result.anchorDay : result.latestDay;
  if (inWindow(day, anchor)) {
    auto& bucket = daySlot(day % HISTORY_DAYS);
    if (bucket.date != day) bucket = {0, 0, day};
    bucket.seconds += seconds;
    if (isBook) bucket.bookSeconds += seconds;
    if (anchor - day < week.size()) {
      auto& weekDay = week[day % week.size()];
      if (weekDay.date != day) {
        weekDay = {};
        weekDay.date = day;
      }
      weekDay.periods[period] += seconds;
      if (isBook) weekDay.bookPeriods[period] += seconds;
    }
  }
}

Status Query::feed(const char* data, size_t size) {
  if (state != Status::Scanning) return state;
  if (!data && size) {
    stop(Status::IoError);
    return state;
  }
  const uint64_t chunkOffset = result.bytes;
  result.bytes += size;
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    if (c == '\n') {
      processLine();
      if (state != Status::Scanning) return state;
      length = 0;
      overflow = false;
      lineOffset = chunkOffset + i + 1;
    } else if (!overflow) {
      // Allow one CR before LF without increasing the JSON payload limit.
      if (length == MAX_LINE + 1)
        overflow = true;
      else
        line[length++] = c;
      if (length == MAX_LINE + 1 && c != '\r') overflow = true;
    }
  }
  return state;
}

Status Query::finish() {
  if (state != Status::Scanning) return state;
  if (baselineOpen) {
    LOG_ERR("RHIST", "Incomplete historical baseline; refusing partial results");
    stop(Status::IoError);
    return state;
  }
  result.incompleteTail = length != 0 || overflow;
  if (!result.anchorDay) result.anchorDay = result.latestDay;
  for (size_t i = 0; i < HISTORY_DAYS; ++i) {
    const auto& day = daySlot(i);
    if (!inWindow(day.date, result.anchorDay)) continue;
    if (day.seconds) ++result.activeDays;
    if (day.bookSeconds) ++result.bookActiveDays;
  }
  for (const auto& day : week) {
    if (!day.date || day.date > result.anchorDay || result.anchorDay - day.date >= week.size()) continue;
    for (unsigned i = 0; i < 4; ++i) result.weekPeriods[i] += day.periods[i];
  }
  releaseParser();
  state = Status::Ready;
  return state;
}

const DocumentId* Query::bookAt(size_t index) const {
  return state == Status::Ready && index < result.books ? &entryAt(index).id : nullptr;
}

uint32_t Query::bookOffsetAt(size_t index) const { return bookAt(index) ? entryAt(index).offset : UINT32_MAX; }

Day Query::dayAt(uint32_t date) const {
  if (state != Status::Ready || !inWindow(date, result.anchorDay)) return {};
  const auto& day = daySlot(date % HISTORY_DAYS);
  return day.date == date ? day : Day{};
}
}  // namespace readingHistory
