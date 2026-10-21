#include <cassert>
#include <cstdint>
#include <cstdio>

#include "../../src/util/ReadingStatsLegacyFormats.h"

using namespace readingstats::legacy;

int main() {
  uint8_t crossInk[159] = {};
  crossInk[0] = 3;
  crossInk[1] = 9;  // opens
  crossInk[5] = 0x10;
  crossInk[6] = 0x0e;  // 3600 seconds
  crossInk[9] = 20;    // forward turns
  Counters out{};
  assert(crossInkGlobal(crossInk, sizeof(crossInk), out));
  assert(out.sessions == 9 && out.totalSeconds == 3600 && out.turns == 20);
  assert(!crossInkGlobal(crossInk, 158, out));
  assert(!crossInkGlobal(crossInk, 17, out));
  crossInk[0] = 1;
  assert(crossInkGlobal(crossInk, 13, out));
  crossInk[0] = 2;
  assert(crossInkGlobal(crossInk, 17, out));
  crossInk[0] = 4;
  assert(!crossInkGlobal(crossInk, sizeof(crossInk), out));

  uint8_t book[73] = {};
  book[0] = 5;
  book[1] = 12;
  book[3] = 0x20;
  book[4] = 0x1c;  // 7200 seconds
  book[7] = 91;
  assert(crossInkBook(book, 73, out));
  assert(out.sessions == 12 && out.totalSeconds == 7200 && out.turns == 91);
  for (uint8_t version = 1; version <= 4; ++version) {
    book[0] = version;
    const size_t sizes[] = {0, 11, 12, 16, 69, 73};
    assert(crossInkBook(book, sizes[version], out));
    assert(!crossInkBook(book, sizes[version] + 1, out));
  }
  book[0] = 6;
  assert(!crossInkBook(book, 73, out));

  uint32_t oneBased = 0;
  assert(vCodexDay(0, oneBased) && oneBased == 1);
  assert(vCodexDay(20500, oneBased) && oneBased == 20501);
  assert(!vCodexDay(UINT32_MAX, oneBased));
  std::puts("CrossInk versioned formats and vCodex ordinal conversion PASS");
  return 0;
}
