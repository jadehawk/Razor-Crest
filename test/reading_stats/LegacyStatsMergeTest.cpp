#include <cassert>
#include <cstdio>

#include "../../src/util/ReadingStatsLegacyMerge.h"

int main() {
  using namespace readingstats;
  Snapshot native{};
  native.addSession(125, 4, 20500);
  native.addSession(75, 2, 20499);
  Snapshot prior{};
  prior.addSession(300, 0, 20499);
  prior.sessions = 10;             // vCodex supplied its historical open count, not turns
  prior.addSession(40, 0, 20000);  // older than the daily display window
  prior.seal();
  assert(legacy::mergeSnapshot(native, prior));
  assert(native.totalSeconds() == 540);
  assert(native.sessions == 12);
  assert(native.pageTurns == 6);
  assert(native.latestDay == 20500);
  assert(native.secondsOnDay(20500) == 125);
  assert(native.secondsOnDay(20499) == 375);
  assert(native.secondsOnDay(20000) == 0);
  assert(native.valid());

  Snapshot allTimeOnly{};
  allTimeOnly.addSession(60, 0, 0);
  allTimeOnly.seal();
  assert(legacy::mergeSnapshot(native, allTimeOnly));
  assert(native.totalSeconds() == 600);
  assert(native.latestDay == 20500);
  assert(native.secondsOnDay(20500) == 125);

  Snapshot invalid{};
  invalid.version = 99;
  const uint64_t total = native.totalSeconds();
  assert(!legacy::mergeSnapshot(native, invalid));
  assert(native.totalSeconds() == total);

  std::puts("Legacy merge: counted data, daily buckets, and BIN checksum PASS");
  return 0;
}
