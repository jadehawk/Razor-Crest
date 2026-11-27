#include "ReadingHistoryProvider.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

namespace readingHistory {
namespace {
constexpr char HISTORY_PATH[] = "/.crosspoint/reading_sessions.jsonl";
// Derived and disposable; not in the legacy reading-stats directory.
constexpr char CACHE_PATH[] = "/.crosspoint/reading_sessions-history.cache";
constexpr char TEMP_PATH[] = "/.crosspoint/reading_sessions-history.cache.tmp";
constexpr char MAGIC[] = "RHISTC07";
constexpr uint64_t HASH_INITIAL = 14695981039346656037ULL;
constexpr size_t HEADER = 288, DAY_BYTES = 20, WEEK_BYTES = 68, BOOK_BYTES = 432, BOOK_INDEX_BYTES = 28;
constexpr size_t WEEK_START = HEADER + HISTORY_DAYS * DAY_BYTES;
constexpr size_t BOOK_START = WEEK_START + 7 * WEEK_BYTES;

uint64_t hashBytes(uint64_t hash, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 1099511628211ULL;
  return hash;
}

void put(char* out, uint64_t value, unsigned n) {
  for (unsigned i = 0; i < n; ++i) out[i] = static_cast<char>(value >> (8 * i));
}

uint64_t get(const char* in, unsigned n) {
  uint64_t value = 0;
  for (unsigned i = 0; i < n; ++i) value |= uint64_t(static_cast<uint8_t>(in[i])) << (8 * i);
  return value;
}

void encodeTotals(char* out, const Totals& t) {
  put(out, t.seconds, 8);
  put(out + 8, t.sessions, 8);
  put(out + 16, t.datedSessions, 8);
  for (unsigned i = 0; i < 4; ++i) put(out + 24 + 8 * i, t.periods[i], 8);
}

void decodeTotals(const char* in, Totals& t) {
  t.seconds = get(in, 8);
  t.sessions = get(in + 8, 8);
  t.datedSessions = get(in + 16, 8);
  for (unsigned i = 0; i < 4; ++i) t.periods[i] = get(in + 24 + 8 * i, 8);
}

void encodeSummary(char* p, const Summary& s) {
  put(p, 1, 4);
  put(p + 4, (s.booksLimited ? 1 : 0) | (s.incompleteTail ? 2 : 0), 4);
  put(p + 8, s.anchorDay, 4);
  put(p + 12, s.latestDay, 4);
  put(p + 16, s.activeDays, 4);
  put(p + 20, s.bookActiveDays, 4);
  put(p + 24, s.books, 4);
  put(p + 28, s.baselineApplied ? 1 : 0, 4);
  put(p + 32, s.bytes, 8);
  put(p + 40, s.rejected, 8);
  encodeTotals(p + 48, s.overall);
  encodeTotals(p + 104, s.book);
  for (unsigned i = 0; i < 4; ++i) put(p + 160 + 8 * i, s.weekPeriods[i], 8);
  put(p + 192, s.overall.turns, 8);
  put(p + 200, s.book.turns, 8);
  put(p + 208, (s.overall.turnsComplete ? 1 : 0) | (s.book.turnsComplete ? 2 : 0), 4);
  put(p + 212, 0, 4);
  memcpy(p + 216, s.sourceId.data(), 16);
  memcpy(p + 232, s.sourceMac.data(), 13);
  put(p + 245, s.sourceConflict ? 1 : 0, 1);
}

bool decodeSummary(const char* p, Summary& s) {
  const auto flags = get(p + 4, 4);
  if (get(p, 4) != 1 || flags > 3 || get(p + 28, 4) > 1) return false;
  s.baselineApplied = get(p + 28, 4) != 0;
  s.booksLimited = flags & 1;
  s.incompleteTail = flags & 2;
  s.anchorDay = get(p + 8, 4);
  s.latestDay = get(p + 12, 4);
  s.activeDays = get(p + 16, 4);
  s.bookActiveDays = get(p + 20, 4);
  s.books = get(p + 24, 4);
  s.bytes = get(p + 32, 8);
  s.rejected = get(p + 40, 8);
  decodeTotals(p + 48, s.overall);
  decodeTotals(p + 104, s.book);
  for (unsigned i = 0; i < 4; ++i) s.weekPeriods[i] = get(p + 160 + 8 * i, 8);
  s.overall.turns = get(p + 192, 8);
  s.book.turns = get(p + 200, 8);
  const auto coverage = get(p + 208, 4);
  if (coverage > 3 || get(p + 212, 4)) return false;
  s.overall.turnsComplete = coverage & 1;
  s.book.turnsComplete = coverage & 2;
  memcpy(s.sourceId.data(), p + 216, 16);
  memcpy(s.sourceMac.data(), p + 232, 13);
  if (s.sourceMac[12] || get(p + 245, 1) > 1) return false;
  s.sourceConflict = get(p + 245, 1) != 0;
  return s.books <= MAX_BOOKS && (!s.booksLimited || s.books == MAX_BOOKS) && s.anchorDay <= 47482 &&
         s.latestDay <= 47482 && s.activeDays <= HISTORY_DAYS && s.bookActiveDays <= HISTORY_DAYS &&
         (s.baselineApplied || (s.bookActiveDays <= s.activeDays && s.book.sessions <= s.overall.sessions &&
                                s.book.seconds <= s.overall.seconds)) &&
         s.overall.datedSessions <= s.overall.sessions && s.book.datedSessions <= s.book.sessions;
}

bool readExact(HalFile& f, void* out, size_t n) { return f.read(out, n) == static_cast<int>(n); }

bool writeHashed(HalFile& f, uint64_t& hash, const void* p, size_t n) {
  if (f.write(p, n) != n) return false;
  hash = hashBytes(hash, p, n);
  return true;
}
Status readBookMetadata(HalFile& history, char* buffer, uint64_t expected, uint32_t offset, BookInfo& out,
                        JsonDocument& filter, JsonDocument& parsed) {
    if (offset == UINT32_MAX) return Status::Ready;
    if (offset >= expected || !history.seek64(offset)) return Status::IoError;
    const auto wanted = static_cast<size_t>(std::min<uint64_t>(MAX_LINE + 2, expected - offset));
    const int got = history.read(buffer, wanted);
    if (got <= 0 || static_cast<size_t>(got) > wanted) return Status::IoError;
    const auto* end = static_cast<const char*>(memchr(buffer, '\n', got));
    if (!end) return Status::IoError;
    const auto error = deserializeJson(parsed, buffer, static_cast<size_t>(end - buffer),
                                       DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(8));
    if (error == DeserializationError::NoMemory) return Status::NoMemory;
    const JsonString doc = parsed["doc"].as<JsonString>();
    DocumentId id{};
    if (error || doc.isNull() || !decodeDocumentId({doc.c_str(), doc.size()}, id) || id != out.id)
      return Status::IoError;
    const auto progress = parsed["to"];
    if (progress.is<uint16_t>() && progress.as<uint16_t>() <= 10000) out.progress = progress.as<uint16_t>();
    {
      const auto path = parsed["path"].as<JsonString>();
      if (!path.isNull() && path.size() < sizeof(out.path) && path.size() && path.c_str()[0] == '/' &&
          !memchr(path.c_str(), 0, path.size()))
        std::memcpy(out.path, path.c_str(), path.size());
      const auto author = parsed["author"].as<JsonString>();
      if (!author.isNull()) {
        const auto n = utf8SafeTruncateBuffer(author.c_str(),
                                              static_cast<int>(std::min(author.size(), sizeof(out.author) - 1)));
        std::memcpy(out.author, author.c_str(), n);
      }
    }
    const auto series = parsed["series"].as<JsonString>();
    const auto index = parsed["series_index"].as<JsonString>();
    if (!series.isNull() && series.size()) {
      size_t prefix = 0;
      if (!index.isNull() && index.size() && index.size() <= 12 &&
          strspn(index.c_str(), "0123456789.") == index.size()) {
        prefix = snprintf(out.series, sizeof(out.series), "#%s - ", index.c_str());
      }
      const auto n = utf8SafeTruncateBuffer(series.c_str(),
          static_cast<int>(std::min(series.size(), sizeof(out.series) - prefix - 1)));
      memcpy(out.series + prefix, series.c_str(), n);
    }
    const JsonString title = parsed["title"].as<JsonString>();
    if (!title.isNull()) {
      const size_t n = std::min(title.size(), sizeof(out.title));
      out.titleLength = static_cast<uint8_t>(utf8SafeTruncateBuffer(title.c_str(), static_cast<int>(n)));
      for (size_t j = 0; j < out.titleLength; ++j) {
        const auto c = static_cast<unsigned char>(title.c_str()[j]);
        out.title[j] = c < 32 || c == 127 ? ' ' : static_cast<char>(c);
      }
    }
  return Status::Ready;
}
}  // namespace

void Provider::close() {
  if (file) file.close();
  if (checkpoint) checkpoint.close();
  chunk.reset();
}

Status Provider::fail(Status reason) {
  close();
  scan.stop(reason);
  verifying = false;
  state = reason;
  return state;
}

size_t Provider::aggregationBuffersBytes() const {
  size_t bytes = (chunk ? READ_BYTES : 0) + (scan.line ? MAX_LINE + 1 : 0);
  for (size_t i = 0; i < scan.days.size(); ++i)
    if (scan.days[i]) bytes += Query::dayBlockSize(i) * sizeof(Day);
  for (const auto& block : scan.ids)
    if (block) bytes += Query::BOOK_BLOCK * sizeof(Query::BookEntry);
  for (const auto& block : scan.bookSeconds)
    if (block) bytes += Query::BOOK_BLOCK * sizeof(uint64_t);
  return bytes;
}

bool Provider::readRecord(uint64_t position, void* out, size_t size) const {
  return checkpoint && checkpoint.seek64(position) && readExact(checkpoint, out, size);
}

bool Provider::loadCheckpoint() {
  if (!Storage.exists(CACHE_PATH) || !Storage.openFileForRead("RHIST", CACHE_PATH, checkpoint) ||
      checkpoint.isDirectory())
    return false;
  char* p = chunk.get();
  if (!readRecord(0, p, HEADER) || std::memcmp(p, MAGIC, 8) || get(p + 8, 4) != requestedAnchor ||
      get(p + 12, 4) != ((documentKey[0] ? 1u : 0u) | (detailedBooks ? 2u : 0u)))
    return false;
  DocumentId id{};
  if (documentKey[0] && !decodeDocumentId(documentKey, id)) return false;
  if (std::memcmp(p + 16, id.data(), id.size()) || !decodeSummary(p + 40, cached) || cached.bytes > expected ||
      (requestedAnchor && cached.anchorDay != requestedAnchor) ||
      (!requestedAnchor && cached.anchorDay != cached.latestDay) || (cached.incompleteTail && cached.bytes != expected))
    return false;
  checkpointHash = get(p + 32, 8);
  const uint64_t end = BOOK_START + cached.books * BOOK_BYTES;
  if (checkpoint.fileSize64() != end + 8) return false;
  uint64_t hash = hashBytes(HASH_INITIAL, p, HEADER);
  // Validate the entire small checkpoint, not just the header.
  for (uint64_t at = HEADER; at < end;) {
    const auto n = static_cast<size_t>(std::min<uint64_t>(READ_BYTES, end - at));
    if (!readExact(checkpoint, p, n)) return false;
    hash = hashBytes(hash, p, n);
    at += n;
  }
  return readExact(checkpoint, p, 8) && get(p, 8) == hash;
}

Status Provider::fresh() {
  if (checkpoint) checkpoint.close();
  verifying = false;
  processed = parsed = 0;
  sourceHash = HASH_INITIAL;
  if (!file.seek64(0)) return fail(Status::IoError);
  const auto s = scan.begin(documentKey, requestedAnchor, detailedBooks);
  if (s != Status::Scanning) return fail(s);
  return expected ? state : finish();
}

Status Provider::begin(std::string_view document, uint32_t anchorDay, bool details, uint64_t prefixBytes) {
  detailedBooks = details;
  close();
  scan.stop(Status::Idle);
  scan.result = {};
  cached = {};
  expected = processed = parsed = 0;
  verifying = false;
  sourceHash = HASH_INITIAL;
  DocumentId id{};
  if ((!document.empty() && !decodeDocumentId(document, id)) || anchorDay > 47482) return fail(Status::InvalidQuery);
  std::memset(documentKey, 0, sizeof(documentKey));
  if (!document.empty()) std::memcpy(documentKey, document.data(), document.size());
  requestedAnchor = anchorDay;
  if (!Storage.exists(HISTORY_PATH)) return fail(Status::Missing);
  if (!Storage.openFileForRead("RHIST", HISTORY_PATH, file) || file.isDirectory()) return fail(Status::IoError);
  expected = file.fileSize64();
  if (prefixBytes != UINT64_MAX) {
    if (prefixBytes > expected) return fail(Status::IoError);
    expected = prefixBytes;
  }
  chunk = makeUniqueNoThrow<char[]>(READ_BYTES);
  if (!chunk) {
    LOG_ERR("RHIST", "Cannot allocate %u-byte read buffer", static_cast<unsigned>(READ_BYTES));
    return fail(Status::NoMemory);
  }
  state = Status::Scanning;
  if (loadCheckpoint()) {
    verifying = true;
    return cached.bytes ? state : resumeCheckpoint();
  }
  return fresh();
}

Status Provider::resumeCheckpoint() {
  verifying = false;
  if (sourceHash != checkpointHash) {
    LOG_INF("RHIST", "cache source changed; rebuilding");
    return fresh();
  }
  if (processed == expected) {
    LOG_INF("RHIST", "cache hit bytes=%llu", static_cast<unsigned long long>(expected));
    file.close();
    chunk.reset();
    state = Status::Ready;
    return state;
  }
  // Only complete newline boundaries can resume; a changed fixed anchor or
  // selected document has already invalidated the checkpoint in loadCheckpoint.
  const auto s = scan.begin(documentKey, requestedAnchor, detailedBooks);
  if (s != Status::Scanning) return fail(s);
  char record[WEEK_BYTES];
  for (size_t i = 0; i < HISTORY_DAYS; ++i) {
    if (!readRecord(HEADER + i * DAY_BYTES, record, DAY_BYTES)) return fresh();
    const Day day{get(record, 8), get(record + 8, 8), static_cast<uint32_t>(get(record + 16, 4))};
    if (day.date || day.seconds || day.bookSeconds) {
      auto* slot = scan.writableDaySlot(i);
      if (!slot) return fail(Status::NoMemory);
      *slot = day;
    }
  }
  for (size_t i = 0; i < scan.week.size(); ++i) {
    if (!readRecord(WEEK_START + i * WEEK_BYTES, record, WEEK_BYTES)) return fresh();
    auto& d = scan.week[i];
    for (unsigned j = 0; j < 4; ++j) d.periods[j] = get(record + 8 * j, 8);
    for (unsigned j = 0; j < 4; ++j) d.bookPeriods[j] = get(record + 32 + 8 * j, 8);
    d.date = get(record + 64, 4);
  }
  DocumentId previous{};
  for (size_t i = 0; i < cached.books; ++i) {
    if (!readRecord(BOOK_START + i * BOOK_BYTES, record, BOOK_INDEX_BYTES)) return fresh();
    DocumentId id{};
    std::memcpy(id.data(), record, id.size());
    if (i && !(previous < id)) return fresh();
    scan.lineOffset = get(record + 16, 4);
    if (scan.lineOffset != UINT32_MAX && scan.lineOffset >= cached.bytes) return fresh();
    scan.countBook(id, get(record + 20, 8));
    if (scan.status() != Status::Scanning) return fail(scan.status());
    previous = id;
  }
  scan.result = cached;
  scan.result.anchorDay = requestedAnchor;
  scan.result.activeDays = scan.result.bookActiveDays = 0;
  for (auto& value : scan.result.weekPeriods) value = 0;
  scan.lineOffset = processed;
  checkpoint.close();
  LOG_INF("RHIST", "cache resume bytes=%llu append=%llu", static_cast<unsigned long long>(processed),
          static_cast<unsigned long long>(expected - processed));
  return state;
}

bool Provider::saveCheckpoint() {
  if (checkpoint) checkpoint.close();
  // An interrupted write is never published. The prior complete cache may be
  // lost on a failed replacement, but the authoritative JSONL is untouched.
  HalFile temp;
  Storage.remove(TEMP_PATH);
  if (!Storage.openFileForWrite("RHIST", TEMP_PATH, temp)) return false;
  char* p = chunk.get();
  std::memset(p, 0, HEADER);
  std::memcpy(p, MAGIC, 8);
  put(p + 8, requestedAnchor, 4);
  put(p + 12, (scan.hasSelected ? 1 : 0) | (detailedBooks ? 2 : 0), 4);
  std::memcpy(p + 16, scan.selected.data(), scan.selected.size());
  put(p + 32, sourceHash, 8);
  encodeSummary(p + 40, *scan.summary());
  uint64_t hash = HASH_INITIAL;
  bool ok = writeHashed(temp, hash, p, HEADER);
  for (size_t i = 0; ok && i < HISTORY_DAYS; ++i) {
    const auto& d = scan.daySlot(i);
    put(p, d.seconds, 8);
    put(p + 8, d.bookSeconds, 8);
    put(p + 16, d.date, 4);
    ok = writeHashed(temp, hash, p, DAY_BYTES);
  }
  for (const auto& d : scan.week) {
    if (!ok) break;
    for (unsigned i = 0; i < 4; ++i) put(p + i * 8, d.periods[i], 8);
    for (unsigned i = 0; i < 4; ++i) put(p + 32 + i * 8, d.bookPeriods[i], 8);
    put(p + 64, d.date, 4);
    ok = writeHashed(temp, hash, p, WEEK_BYTES);
  }
  // The calendar has been serialized. Release its blocks before metadata parsing.
  for (auto& block : scan.days) block.reset();
  JsonDocument filter;
  JsonDocument parsed;
  if (scan.result.books) {
    filter["doc"] = true;
    filter["title"] = true;
    filter["to"] = true;
    filter["path"] = true;
    filter["author"] = true;
    filter["series"] = true;
    filter["series_index"] = true;
    ok = ok && !filter.overflowed();
  }
  for (size_t i = 0; ok && i < scan.result.books; ++i) {
    const auto& b = scan.entryAt(i);
    BookInfo info{};
    info.id = b.id;
    if (readBookMetadata(file, p, expected, b.offset, info, filter, parsed) != Status::Ready) {
      ok = false;
      break;
    }
    memset(p, 0, BOOK_BYTES);
    memcpy(p, b.id.data(), b.id.size());
    put(p + 16, b.offset, 4);
    put(p + 20, detailedBooks ? scan.secondsAt(i) : 0, 8);
    put(p + 28, info.progress, 2);
    p[30] = static_cast<char>(info.titleLength);
    memcpy(p + 31, info.title, sizeof(info.title));
    memcpy(p + 111, info.path, sizeof(info.path));
    memcpy(p + 287, info.author, sizeof(info.author));
    memcpy(p + 336, info.series, sizeof(info.series));
    ok = writeHashed(temp, hash, p, BOOK_BYTES);
  }
  put(p, hash, 8);
  if (ok) ok = temp.write(p, 8) == 8;
  if (!temp.close()) ok = false;
  if (ok) ok = Storage.replaceFile(TEMP_PATH, CACHE_PATH);
  if (!ok) Storage.remove(TEMP_PATH);
  return ok;
}

Status Provider::finish() {
  const auto s = scan.finish();
  if (s != Status::Ready) return fail(s);
  const bool saved = saveCheckpoint();
  if (file) file.close();
  // Free the day ring, book index and parser BEFORE the guest draws its UI.
  scan.stop(Status::Idle);
  if (!saved || !loadCheckpoint() || cached.bytes != expected || checkpointHash != sourceHash) {
    LOG_ERR("RHIST", "Cannot publish reading history checkpoint");
    return fail(Status::IoError);
  }
  chunk.reset();
  state = Status::Ready;
  LOG_INF("RHIST", "cache published; aggregation memory released");
  return state;
}

Status Provider::step() {
  if (state != Status::Scanning) return state;
  const uint64_t boundary = verifying ? cached.bytes : expected;
  const size_t wanted = static_cast<size_t>(std::min<uint64_t>(READ_BYTES, boundary - processed));
  const int got = file.read(chunk.get(), wanted);
  if (got <= 0 || static_cast<size_t>(got) > wanted) return fail(Status::IoError);
  sourceHash = hashBytes(sourceHash, chunk.get(), got);
  processed += static_cast<unsigned>(got);
  if (verifying) return processed == boundary ? resumeCheckpoint() : state;
  parsed += static_cast<unsigned>(got);
  const auto s = scan.feed(chunk.get(), static_cast<size_t>(got));
  if (s != Status::Scanning) return fail(s);
  return processed == expected ? finish() : state;
}

bool Provider::readDay(uint32_t date, Day& out) const {
  out = {};
  if (!summary() || !date || date > cached.anchorDay || cached.anchorDay - date >= HISTORY_DAYS) return false;
  char p[DAY_BYTES];
  if (!readRecord(HEADER + (date % HISTORY_DAYS) * DAY_BYTES, p, sizeof(p))) return false;
  if (get(p + 16, 4) == date) out = {get(p, 8), get(p + 8, 8), date};
  return true;
}

bool Provider::readWeekDay(uint32_t date, uint64_t (&out)[4], bool selected) const {
  std::fill_n(out, 4, 0);
  if (!summary() || !date || date > cached.anchorDay || cached.anchorDay - date >= 7) return false;
  char p[WEEK_BYTES];
  if (!readRecord(WEEK_START + (date % 7) * WEEK_BYTES, p, sizeof(p))) return false;
  if (get(p + 64, 4) == date) {
    for (unsigned i = 0; i < 4; ++i) out[i] = get(p + (selected ? 32 : 0) + i * 8, 8);
  }
  return true;
}

bool Provider::readBook(size_t index, DocumentId& id, uint32_t& offset) const {
  if (!summary() || index >= cached.books) return false;
  char p[20];
  if (!readRecord(BOOK_START + index * BOOK_BYTES, p, sizeof(p))) return false;
  std::memcpy(id.data(), p, id.size());
  offset = static_cast<uint32_t>(get(p + 16, 4));
  return offset == UINT32_MAX || offset < expected;
}

Status Provider::readBooks(size_t first, BookInfo* out, size_t count) const {
  const auto* summary = this->summary();
  if (!summary || !out || count > 8 || first > summary->books || count > summary->books - first)
    return Status::InvalidQuery;
  char record[BOOK_BYTES];
  for (size_t i = 0; i < count; ++i) {
    if (!readRecord(BOOK_START + (first + i) * BOOK_BYTES, record, sizeof(record))) return Status::IoError;
    auto& book = out[i];
    book = {};
    memcpy(book.id.data(), record, book.id.size());
    book.seconds = get(record + 20, 8);
    book.progress = static_cast<uint16_t>(get(record + 28, 2));
    book.titleLength = static_cast<uint8_t>(record[30]);
    if (book.titleLength > sizeof(book.title) ||
        (book.progress != UINT16_MAX && book.progress > 10000) ||
        record[286] || record[334] || record[335] || record[431]) return Status::IoError;
    memcpy(book.title, record + 31, sizeof(book.title));
    memcpy(book.path, record + 111, sizeof(book.path));
    memcpy(book.author, record + 287, sizeof(book.author));
    memcpy(book.series, record + 336, sizeof(book.series));
  }
  return Status::Ready;
}

void Provider::cancel() { fail(Status::Cancelled); }
}  // namespace readingHistory
