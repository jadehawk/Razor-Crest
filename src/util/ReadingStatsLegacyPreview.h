#pragma once

#include <cstdint>
#include <string>

#include "ReadingStatsData.h"
#include "ReadingStatsLegacyFormats.h"

namespace readingstats::legacy {

enum class Source : uint8_t { CrossInk, VCodex };

struct Preview {
  bool present = false;
  bool supported = false;
  bool truncated = false;
  uint32_t books = 0;
  Counters totals{};
};

// Bounded, read-only discovery. Never modifies legacy or native statistics.
// vCodex is streamed a book at a time rather than deserializing all books
// into RAM (important for no-PSRAM X3).
Preview inspect(Source source);

// One record at a time. The callback must not retain references past return.
// Before invoking a mutating visitor, scanVCodexBooks validates the complete
// source once; a changed source can still fail midway and is safe to retry.
struct BookRecord {
  std::string key;
  std::string path;
  std::string title;
  std::string author;
  Snapshot delta{};
};
using BookVisitor = bool (*)(const BookRecord&, void*);
bool scanVCodexBooks(BookVisitor visitor, void* user, uint32_t& seen);
}  // namespace readingstats::legacy
