#if CROSSPOINT_DESSEN_APPS
#include "DessenReadingHistory.h"

#include <ArduinoJson.h>
#include <Memory.h>
#include <Utf8.h>
#include <freeink/DessenServices.h>
#include <sdk/dessen_abi.h>

#include <algorithm>
#include <cstring>

#include "sync/ReadingSessionLog.h"

namespace {
void put32(uint8_t* out, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(n >> (8 * i));
}
void put64(uint8_t* out, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(n >> (8 * i));
}
bool inWindow(uint32_t date, uint32_t anchor, uint32_t days) { return date && date <= anchor && anchor - date < days; }
}  // namespace

bool DessenReadingHistory::decode(std::string_view text, Id& id) {
  if (text.size() != 32) return false;
  for (unsigned i = 0; i < 32; ++i) {
    const char c = text[i];
    const int n = c >= '0' && c <= '9'   ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
    if (n < 0) return false;
    if (i % 2)
      id[i / 2] |= n;
    else
      id[i / 2] = n << 4;
  }
  return true;
}

int DessenReadingHistory::begin(std::string_view selector, uint32_t anchor) {
  if (!selector.empty()) {
    hasId_ = decode(selector, selectedId_);
    if (!hasId_) {
      if (selector.front() != '/' || selector.size() >= sizeof selectedPath_ ||
          selector.find('\0') != std::string_view::npos)
        return DSN_ERR_INVALID;
      memcpy(selectedPath_, selector.data(), selector.size());
    }
  }
  fixedAnchor_ = anchor != 0;
  anchor_ = anchor;
  if (!Storage.exists(readingSessionLog::LOG_PATH)) return DSN_STATS_NO_HISTORY;
  if (!Storage.openFileForRead("DSN", readingSessionLog::LOG_PATH, file_) || file_.isDirectory() ||
      file_.fileSize64() > UINT32_MAX)
    return DSN_ERR_IO;
  bytes_ = static_cast<uint32_t>(file_.fileSize64());
  return dessen::kPending;
}

int DessenReadingHistory::processLine() {
  if (overflow_ || !length_) {
    ++rejected_;
    return 0;
  }
  JsonDocument doc;
  const auto error = deserializeJson(doc, line_, length_, DeserializationOption::NestingLimit(4));
  if (error == DeserializationError::NoMemory) return DSN_STATS_NO_MEMORY;
  Id id{};
  const auto seconds = doc["secs"];
  if (error || !decode(doc["doc"] | "", id) || !seconds.is<uint32_t>() || seconds.as<uint32_t>() < 10 ||
      seconds.as<uint32_t>() > 7 * 86400) {
    ++rejected_;
    return 0;
  }
  const uint32_t duration = seconds.as<uint32_t>();
  const bool selected = hasId_ ? id == selectedId_ : selectedPath_[0] && strcmp(selectedPath_, doc["path"] | "") == 0;
  const int64_t start = doc["start"] | int64_t{0};
  const int tz = doc["tz"] | 0;
  uint32_t date = 0;
  unsigned period = 0;
  if (doc["start"].is<int64_t>() && start >= 946684800 && start <= 4102444799LL && doc["tz"].is<int>() && tz >= -720 &&
      tz <= 840) {
    const int64_t local = start + static_cast<int64_t>(tz) * 60;
    date = static_cast<uint32_t>(local / 86400);
    const auto hour = local % 86400 / 3600;
    period = hour < 6 || hour >= 21 ? 3 : hour < 12 ? 0 : hour < 17 ? 1 : 2;
    latest_ = std::max(latest_, date);
    if (!fixedAnchor_) anchor_ = latest_;
  }
  auto add = [&](Totals& t) {
    t.seconds += duration;
    ++t.sessions;
    if (date) {
      ++t.dated;
      t.periods[period] += duration;
    }
  };
  add(overall_);
  if (selected) add(selected_);
  if (inWindow(date, anchor_, Days)) {
    const auto slot = date % Days;
    auto& block = days_[slot / DayBlock];
    if (!block) block = makeUniqueNoThrow<Day[]>(DayBlock);
    if (!block) return DSN_STATS_NO_MEMORY;
    auto& d = block[slot % DayBlock];
    if (d.date != date) d = Day{0, 0, date};
    d.total += duration;
    if (selected) d.selected += duration;
  }
  if (inWindow(date, anchor_, 7)) {
    auto& week = week_[date % 7];
    if (week.date != date) week = Week{{}, date};
    week.periods[period] += duration;
  }
  unsigned lo = 0, hi = count_;
  while (lo < hi) {
    const unsigned mid = lo + (hi - lo) / 2;
    if (book(mid).id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < count_ && book(lo).id == id) {
    book(lo) = Book{id, lineOffset_, static_cast<uint16_t>(length_)};
  } else if (count_ < MaxBooks) {
    auto& block = books_[count_ / BookBlock];
    if (!block) block = makeUniqueNoThrow<Book[]>(BookBlock);
    if (!block) return DSN_STATS_NO_MEMORY;
    for (unsigned i = count_; i > lo; --i) book(i) = book(i - 1);
    book(lo) = Book{id, lineOffset_, static_cast<uint16_t>(length_)};
    ++count_;
  } else {
    limited_ = true;
  }
  return 0;
}

int DessenReadingHistory::step() {
  if (ready_) return 0;
  if (consumed_ < bytes_) {
    const auto wanted = std::min<uint32_t>(sizeof chunk_, bytes_ - consumed_);
    const int got = file_.read(chunk_, wanted);
    if (got <= 0 || static_cast<uint32_t>(got) > wanted) return DSN_ERR_IO;
    for (int i = 0; i < got; ++i) {
      const char c = chunk_[i];
      ++consumed_;
      if (c == '\n') {
        const int result = processLine();
        if (result < 0) return result;
        length_ = 0;
        overflow_ = false;
        lineOffset_ = consumed_;
      } else if (length_ < sizeof line_ - 1) {
        line_[length_++] = c;
      } else {
        overflow_ = true;
      }
    }
    if (consumed_ < bytes_) return dessen::kPending;
  }
  ready_ = true;
  return 0;
}

const DessenReadingHistory::Day* DessenReadingHistory::day(uint32_t date) const {
  const unsigned slot = date % Days;
  const auto& block = days_[slot / DayBlock];
  if (!block || block[slot % DayBlock].date != date) return nullptr;
  return &block[slot % DayBlock];
}

int DessenReadingHistory::bookInfo(uint32_t first, uint8_t* out, uint32_t cap) {
  if (first >= count_) return 0;
  const unsigned n = std::min<uint32_t>(cap / DSN_STATS_BOOK_INFO_BYTES, count_ - first);
  auto staged = makeUniqueNoThrow<uint8_t[]>(n * DSN_STATS_BOOK_INFO_BYTES);
  if (!staged) return DSN_STATS_NO_MEMORY;
  for (unsigned i = 0; i < n; ++i) {
    const auto& entry = book(first + i);
    if (!file_.seek(entry.offset) || file_.read(line_, entry.length) != entry.length) return DSN_ERR_IO;
    JsonDocument doc;
    const auto error = deserializeJson(doc, line_, entry.length, DeserializationOption::NestingLimit(4));
    if (error == DeserializationError::NoMemory) return DSN_STATS_NO_MEMORY;
    Id id{};
    if (error || !decode(doc["doc"] | "", id) || id != entry.id) return DSN_ERR_IO;
    auto* record = staged.get() + i * DSN_STATS_BOOK_INFO_BYTES;
    memset(record, 0, DSN_STATS_BOOK_INFO_BYTES);
    memcpy(record, id.data(), id.size());
    const uint16_t progress =
        doc["to"].is<uint16_t>() && doc["to"].as<uint16_t>() <= 10000 ? doc["to"].as<uint16_t>() : UINT16_MAX;
    record[16] = progress & 255;
    record[17] = progress >> 8;
    const char* title = doc["title"] | "";
    const auto len = utf8SafeTruncateBuffer(title, std::min<size_t>(strlen(title), 80));
    record[18] = len;
    memcpy(record + 20, title, len);
  }
  memcpy(out, staged.get(), n * DSN_STATS_BOOK_INFO_BYTES);
  return n * DSN_STATS_BOOK_INFO_BYTES;
}

int DessenReadingHistory::read(uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) {
  if (!ready_ || !out) return DSN_ERR_INVALID;
  if (kind == DSN_STATS_SUMMARY) {
    if (first) return DSN_ERR_INVALID;
    if (cap < DSN_STATS_SUMMARY_BYTES) return DSN_ERR_TOO_BIG;
    uint32_t active = 0, selected = 0;
    for (unsigned i = 0; i < Days && i < anchor_; ++i) {
      const auto* d = day(anchor_ - i);
      if (d) {
        active += d->total != 0;
        selected += d->selected != 0;
      }
    }
    memset(out, 0, DSN_STATS_SUMMARY_BYTES);
    put32(out, DSN_STATS_VERSION);
    put32(out + 4, (limited_ ? DSN_STATS_BOOKS_LIMITED : 0) | (length_ || overflow_ ? DSN_STATS_INCOMPLETE_TAIL : 0));
    put32(out + 8, anchor_);
    put32(out + 12, latest_);
    put32(out + 16, active);
    put32(out + 20, selected);
    put32(out + 24, count_);
    put64(out + 32, bytes_);
    put64(out + 40, rejected_);
    auto totals = [](uint8_t* p, const Totals& t) {
      put64(p, t.seconds);
      put64(p + 8, t.sessions);
      put64(p + 16, t.dated);
      for (unsigned i = 0; i < 4; ++i) put64(p + 24 + 8 * i, t.periods[i]);
    };
    totals(out + 48, overall_);
    totals(out + 104, selected_);
    uint64_t periods[4]{};
    for (const auto& w : week_)
      if (inWindow(w.date, anchor_, 7))
        for (unsigned i = 0; i < 4; ++i) periods[i] += w.periods[i];
    for (unsigned i = 0; i < 4; ++i) put64(out + 160 + 8 * i, periods[i]);
    return DSN_STATS_SUMMARY_BYTES;
  }
  if (kind == DSN_STATS_BOOK_INFO) {
    if (!cap || cap % DSN_STATS_BOOK_INFO_BYTES || cap / DSN_STATS_BOOK_INFO_BYTES > DSN_STATS_BOOK_INFO_MAX)
      return DSN_ERR_INVALID;
    return bookInfo(first, out, cap);
  }
  if (!cap || cap % 16 || cap / 16 > DSN_STATS_PAGE_MAX) return DSN_ERR_INVALID;
  if (kind == DSN_STATS_DAYS) {
    if (!inWindow(first, anchor_, Days)) return DSN_ERR_INVALID;
    const unsigned n = std::min(cap / 16, anchor_ - first + 1);
    for (unsigned i = 0; i < n; ++i) {
      const auto* d = day(first + i);
      put64(out + 16 * i, d ? d->total : 0);
      put64(out + 16 * i + 8, d ? d->selected : 0);
    }
    return 16 * n;
  }
  if (kind == DSN_STATS_BOOKS) {
    if (first >= count_) return 0;
    const unsigned n = std::min<uint32_t>(cap / 16, count_ - first);
    for (unsigned i = 0; i < n; ++i) memcpy(out + 16 * i, book(first + i).id.data(), 16);
    return 16 * n;
  }
  return DSN_ERR_INVALID;
}
#endif
