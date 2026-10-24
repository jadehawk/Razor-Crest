#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ReadingStatsData.h"

namespace readingstats {

// Compact on-SD catalog of books opened or recorded. Fixed-size entries
// allow O(1) positional paging without loading every title on a no-PSRAM X3.
// Hash identifies the original complete path even if the display path is cut.
struct BookEntry {
  uint32_t magic = 0x314B4252;  // RBK1
  uint32_t version = 1;
  uint32_t hashLo = 0;
  uint32_t hashHi = 0;
  char path[176] = {};
  char title[72] = {};
  char author[48] = {};
  uint32_t checksum = 0;

  uint64_t hash() const { return (static_cast<uint64_t>(hashHi) << 32) | hashLo; }
  void setHash(uint64_t v) {
    hashLo = static_cast<uint32_t>(v);
    hashHi = static_cast<uint32_t>(v >> 32);
  }
  void seal() { checksum = checksumBytes(this, offsetof(BookEntry, checksum)); }
  bool valid() const {
    return magic == 0x314B4252 && version == 1 && checksum == checksumBytes(this, offsetof(BookEntry, checksum)) &&
           memchr(path, '\0', sizeof(path)) && memchr(title, '\0', sizeof(title)) &&
           memchr(author, '\0', sizeof(author));
  }
};

static_assert(sizeof(BookEntry) == 316, "Unexpected book-index format");
}  // namespace readingstats
