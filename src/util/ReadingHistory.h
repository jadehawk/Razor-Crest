#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace readingHistory {
constexpr size_t MAX_LINE = 511;  // Stock writer reserves one byte for the terminator.
constexpr size_t HISTORY_DAYS = 371;
constexpr size_t MAX_BOOKS = 1024;
using DocumentId = std::array<uint8_t, 16>;
bool decodeDocumentId(std::string_view text, DocumentId& id);

enum class Status : uint8_t { Idle, Scanning, Ready, Missing, InvalidQuery, NoMemory, IoError, Cancelled };

struct Totals {
  uint64_t seconds = 0;
  uint64_t sessions = 0;
  uint64_t datedSessions = 0;
  uint64_t periods[4]{};
  uint64_t turns = 0;
  bool turnsComplete = true;
};

struct Day {
  uint64_t seconds = 0;
  uint64_t bookSeconds = 0;
  uint32_t date = 0;  // Unix local day number, zero means empty.
};

struct Summary {
  Totals overall;
  Totals book;
  uint64_t weekPeriods[4]{};
  uint64_t bytes = 0;
  uint64_t rejected = 0;
  uint32_t latestDay = 0;
  uint32_t anchorDay = 0;
  uint32_t activeDays = 0;
  uint32_t bookActiveDays = 0;
  size_t books = 0;
  bool booksLimited = false;
  bool incompleteTail = false;
  bool baselineApplied = false;  // Razor-Crest historical checkpoint, not individual sessions.
};

// One owner feeds bounded chunks. Results are available only after finish().
// No file access, clock lookup, or persistent state is owned by the parser.
class Query {
 public:
  Status begin(std::string_view document = {}, uint32_t anchorDay = 0, bool detailedBooks = false);
  Status feed(const char* data, size_t size);
  Status finish();
  void stop(Status reason);
  Status status() const { return state; }
  uint64_t processedSessions() const { return result.overall.sessions; }
  uint64_t rejectedLines() const { return result.rejected; }
  const Summary* summary() const { return state == Status::Ready ? &result : nullptr; }
  const DocumentId* bookAt(size_t index) const;
  uint32_t bookOffsetAt(size_t index) const;
  Day dayAt(uint32_t date) const;

 private:
  friend class Provider;  // Versioned derived checkpoint; no I/O in Query.
  static constexpr size_t BOOK_BLOCK = 64;
  static constexpr size_t BOOK_BLOCKS = MAX_BOOKS / BOOK_BLOCK;
  struct BookEntry {
    DocumentId id{};
    uint32_t offset = UINT32_MAX;
  };
  BookEntry& entryAt(size_t index) { return ids[index / BOOK_BLOCK][index % BOOK_BLOCK]; }
  const BookEntry& entryAt(size_t index) const { return ids[index / BOOK_BLOCK][index % BOOK_BLOCK]; }
  void countBook(const DocumentId& id, uint64_t seconds = 0);
  uint64_t& secondsAt(size_t index) { return bookSeconds[index / BOOK_BLOCK][index % BOOK_BLOCK]; }
  const uint64_t& secondsAt(size_t index) const { return bookSeconds[index / BOOK_BLOCK][index % BOOK_BLOCK]; }
  bool detailedBooks = false;
  std::array<std::unique_ptr<uint64_t[]>, BOOK_BLOCKS> bookSeconds;
  void processLine();
  void processBaseline();
  void rejectLine();
  void releaseParser();

  Status state = Status::Idle;
  Summary result;
  DocumentId selected{};
  bool hasSelected = false;
  std::array<std::unique_ptr<BookEntry[]>, BOOK_BLOCKS> ids;
  std::unique_ptr<Day[]> days;
  struct WeekDay {
    uint64_t periods[4]{};
    uint64_t bookPeriods[4]{};
    uint32_t date = 0;
  };
  std::array<WeekDay, 7> week{};
  std::unique_ptr<char[]> line;
  JsonDocument filter;
  JsonDocument parsed;
  uint64_t lineOffset = 0;
  size_t length = 0;
  bool overflow = false;
  bool baselineOpen = false;
  bool baselineGlobal = false;
  uint32_t baselineRemaining = 0;
  uint32_t baselineHash = 2166136261u;
};
}  // namespace readingHistory
