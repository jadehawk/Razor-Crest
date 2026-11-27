#pragma once

#include <Logging.h>
#include <Memory.h>
#include <sdk/dessen_abi.h>

#include <algorithm>
#include <cstring>

#include "util/ReadingHistoryProvider.h"

namespace dessenReadingStats {
inline int resultCode(readingHistory::Status status) {
  using readingHistory::Status;
  switch (status) {
    case Status::Ready:
      return 0;
    case Status::Missing:
      return DSN_STATS_NO_HISTORY;
    case Status::NoMemory:
      return DSN_STATS_NO_MEMORY;
    case Status::IoError:
      return DSN_ERR_IO;
    default:
      return DSN_ERR_INVALID;
  }
}

inline void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void put64(uint8_t* out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void putTotals(uint8_t* out, const readingHistory::Totals& totals) {
  put64(out, totals.seconds);
  put64(out + 8, totals.sessions);
  put64(out + 16, totals.datedSessions);
  for (unsigned i = 0; i < 4; ++i) put64(out + 24 + i * 8, totals.periods[i]);
}

// Both the parser and the disk-backed provider expose the same result records.
inline bool readDay(const readingHistory::Query& q, uint32_t date, readingHistory::Day& day) {
  day = q.dayAt(date);
  return true;
}
inline bool readDay(const readingHistory::Provider& p, uint32_t date, readingHistory::Day& day) {
  return p.readDay(date, day);
}
inline bool readId(const readingHistory::Query& q, size_t index, readingHistory::DocumentId& id) {
  const auto* value = q.bookAt(index);
  if (!value) return false;
  id = *value;
  return true;
}
inline bool readId(const readingHistory::Provider& p, size_t index, readingHistory::DocumentId& id) {
  uint32_t offset;
  return p.readBook(index, id, offset);
}

// No raw struct copies: the wire representation does not depend on host alignment.
template <class Source>
inline int readImpl(const Source& query, uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) {
  const auto* summary = query.summary();
  if (!summary) return DSN_ERR_INVALID;
  if (kind == DSN_STATS_SUMMARY) {
    if (first) return DSN_ERR_INVALID;
    if (!out || cap < DSN_STATS_SUMMARY_BYTES) return DSN_ERR_TOO_BIG;
    put32(out, DSN_STATS_VERSION);
    put32(out + 4, (summary->booksLimited ? DSN_STATS_BOOKS_LIMITED : 0) |
                       (summary->incompleteTail ? DSN_STATS_INCOMPLETE_TAIL : 0));
    put32(out + 8, summary->anchorDay);
    put32(out + 12, summary->latestDay);
    put32(out + 16, summary->activeDays);
    put32(out + 20, summary->bookActiveDays);
    put32(out + 24, static_cast<uint32_t>(summary->books));
    put32(out + 28, 0);
    put64(out + 32, summary->bytes);
    put64(out + 40, summary->rejected);
    putTotals(out + 48, summary->overall);
    putTotals(out + 104, summary->book);
    for (unsigned i = 0; i < 4; ++i) put64(out + 160 + i * 8, summary->weekPeriods[i]);
    return DSN_STATS_SUMMARY_BYTES;
  }
  // Optional extension, preserving the version-1 summary and syscall IDs.
  if (kind == 6) {
    if (first || !out || cap < 24) return DSN_ERR_INVALID;
    put32(out, 1);
    put32(out + 4, (summary->overall.turnsComplete ? 1 : 0) | (summary->book.turnsComplete ? 2 : 0));
    put64(out + 8, summary->overall.turns);
    put64(out + 16, summary->book.turns);
    return 24;
  }
  if (kind != DSN_STATS_DAYS && kind != DSN_STATS_BOOKS) return DSN_ERR_INVALID;
  if (!out || cap < 16 || cap % 16 || cap / 16 > DSN_STATS_PAGE_MAX) return DSN_ERR_INVALID;
  const uint32_t count = cap / 16;
  uint32_t n = 0;
  if (kind == DSN_STATS_DAYS) {
    if (!first || first > summary->anchorDay || summary->anchorDay - first >= readingHistory::HISTORY_DAYS)
      return DSN_ERR_INVALID;
    n = std::min(count, summary->anchorDay - first + 1);
    for (uint32_t i = 0; i < n; ++i) {
      readingHistory::Day day;
      if (!readDay(query, first + i, day)) return DSN_ERR_IO;
      put64(out + i * 16, day.seconds);
      put64(out + i * 16 + 8, day.bookSeconds);
    }
  } else if (first < summary->books) {
    n = std::min(count, static_cast<uint32_t>(summary->books - first));
    for (uint32_t i = 0; i < n; ++i) {
      readingHistory::DocumentId id;
      if (!readId(query, first + i, id)) return DSN_ERR_IO;
      std::memcpy(out + i * 16, id.data(), id.size());
    }
  }
  return static_cast<int>(n * 16);
}
inline int read(const readingHistory::Query& query, uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) {
  return readImpl(query, kind, first, out, cap);
}

inline int read(const readingHistory::Provider& provider, uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) {
  if (kind == 8) {
    if (!out || cap != 20) return DSN_ERR_INVALID;
    readingHistory::DocumentId id;
    uint32_t offset;
    if (!provider.readBook(first, id, offset)) return DSN_ERR_INVALID;
    memcpy(out, id.data(), 16);
    put32(out + 16, offset);
    return 20;
  }
  // Disk reads can fail partway through a page. Do not partially modify the
  // guest buffer on failure; stage at most 64 fixed-size records.
  if ((kind != DSN_STATS_DAYS && kind != DSN_STATS_BOOKS) || !out || cap < 16 || cap % 16 ||
      cap / 16 > DSN_STATS_PAGE_MAX)
    return readImpl(provider, kind, first, out, cap);
  auto page = makeUniqueNoThrow<uint8_t[]>(cap);
  if (!page) {
    LOG_ERR("RHIST", "Cannot allocate %u-byte result page", static_cast<unsigned>(cap));
    return DSN_STATS_NO_MEMORY;
  }
  const int result = readImpl(provider, kind, first, page.get(), cap);
  if (result > 0) std::memcpy(out, page.get(), static_cast<size_t>(result));
  return result;
}

inline int readBookInfo(const readingHistory::Provider& provider, uint32_t first, uint8_t* out, uint32_t cap) {
  const auto* summary = provider.summary();
  if (!summary || !out || !cap || cap % DSN_STATS_BOOK_INFO_BYTES ||
      cap / DSN_STATS_BOOK_INFO_BYTES > DSN_STATS_BOOK_INFO_MAX)
    return DSN_ERR_INVALID;
  if (first >= summary->books) return 0;
  const size_t n = std::min<size_t>(cap / DSN_STATS_BOOK_INFO_BYTES, summary->books - first);
  auto page = makeUniqueNoThrow<uint8_t[]>(n * DSN_STATS_BOOK_INFO_BYTES);
  if (!page) return DSN_STATS_NO_MEMORY;
  readingHistory::BookInfo book{};
  for (size_t i = 0; i < n; ++i) {
    const auto state = provider.readBooks(first + i, &book, 1);
    if (state != readingHistory::Status::Ready) return resultCode(state);
    auto* record = page.get() + i * DSN_STATS_BOOK_INFO_BYTES;
    std::memcpy(record, book.id.data(), book.id.size());
    record[16] = static_cast<uint8_t>(book.progress);
    record[17] = static_cast<uint8_t>(book.progress >> 8);
    record[18] = book.titleLength;
    record[19] = 0;
    std::memcpy(record + 20, book.title, sizeof(book.title));
  }
  std::memcpy(out, page.get(), n * DSN_STATS_BOOK_INFO_BYTES);
  return static_cast<int>(n * DSN_STATS_BOOK_INFO_BYTES);
}
inline constexpr uint32_t DETAILED_BOOK_BYTES = 364;
// One bounded row, with cached metadata and a full-width duration.
inline int readDetailedBook(const readingHistory::Provider& provider, uint32_t first, uint8_t* out, uint32_t cap) {
  if (!provider.summary() || !out || cap != DETAILED_BOOK_BYTES) return DSN_ERR_INVALID;
  if (first >= provider.summary()->books) return 0;
  readingHistory::BookInfo book{};
  const auto state = provider.readBooks(first, &book, 1);
  if (state != readingHistory::Status::Ready) return resultCode(state);
  memset(out, 0, cap);
  put64(out, book.seconds);
  out[8] = static_cast<uint8_t>(book.progress);
  out[9] = static_cast<uint8_t>(book.progress >> 8);
  out[10] = book.titleLength;
  memcpy(out + 12, book.title, sizeof(book.title));
  memcpy(out + 92, book.path, sizeof(book.path));
  memcpy(out + 268, book.series, sizeof(book.series));
  return DETAILED_BOOK_BYTES;
}
}  // namespace dessenReadingStats
