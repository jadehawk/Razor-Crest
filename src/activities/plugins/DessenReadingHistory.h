#pragma once

#if CROSSPOINT_DESSEN_APPS

#include <HalStorage.h>
#include <sdk/dessen_abi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

// One app-owned, read-only snapshot of the existing append-only session log.
// Only the occupied day and book blocks are allocated. No history, upload state,
// or other SD files are written. The host destroys the snapshot before networking.
class DessenReadingHistory {
 public:
  int begin(std::string_view selector, uint32_t anchor);
  int step();  // kPending until the bounded scan completes
  int read(uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap);

 private:
  using Id = std::array<uint8_t, 16>;
  static constexpr unsigned Days = 371, DayBlock = 16, BookBlock = 16, MaxBooks = 1024;
  struct Day {
    uint64_t total = 0, selected = 0;
    uint32_t date = 0;
  };
  struct Week {
    uint64_t periods[4]{};
    uint32_t date = 0;
  };
  struct Book {
    Id id{};
    uint32_t offset = 0;
    uint16_t length = 0;
  };
  struct Totals {
    uint64_t seconds = 0, sessions = 0, dated = 0, periods[4]{};
  };
  std::array<std::unique_ptr<Day[]>, (Days + DayBlock - 1) / DayBlock> days_;
  std::array<std::unique_ptr<Book[]>, MaxBooks / BookBlock> books_;
  std::array<Week, 7> week_{};
  HalFile file_;
  Totals overall_{}, selected_{};
  Id selectedId_{};
  char selectedPath_[DSN_BOOK_PATH_MAX + 1]{};
  char line_[512]{};
  char chunk_[1024]{};
  uint32_t bytes_ = 0, consumed_ = 0, lineOffset_ = 0, latest_ = 0, anchor_ = 0;
  uint64_t rejected_ = 0;
  unsigned count_ = 0, length_ = 0;
  bool hasId_ = false, fixedAnchor_ = false, overflow_ = false, limited_ = false, ready_ = false;
  static bool decode(std::string_view text, Id& id);
  Book& book(unsigned i) { return books_[i / BookBlock][i % BookBlock]; }
  const Day* day(uint32_t date) const;
  int processLine();
  int bookInfo(uint32_t first, uint8_t* out, uint32_t cap);
};
#endif
