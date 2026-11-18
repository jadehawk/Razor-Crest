#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "../../src/util/ReadingStatsStore.h"
#include "fake_hal/HalStorage.h"

FakeHalStorage Storage;

static uint64_t pathHash(const char* path) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    hash = (hash ^ *p) * 1099511628211ULL;
  return hash;
}

static int64_t localEpoch(int year, int month, int day, int hour) {
  std::tm date{};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = day;
  date.tm_hour = hour;
  date.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&date));
}

int main() {
  using namespace readingstats;
  constexpr const char* first = "/Books/one.epub";
  constexpr const char* second = "/Books/two.epub";
  constexpr const char* globalTime = "/.crosspoint/reading-stats/time_global_v1.bin";
  constexpr const char* pending = "/.crosspoint/reading-stats/pending_v1.bin";

  assert(record(first, 120, 3, localEpoch(2026, 11, 18, 8), "One", "A"));
  assert(record(first, 180, 4, localEpoch(2026, 11, 18, 23), "One", "A"));
  assert(record(second, 60, 1, localEpoch(2026, 11, 18, 14), "Two", "B"));
  TimeBuckets global{}, book{}, other{};
  assert(loadTimeGlobal(global) && global.valid());
  assert(global.total[0] == 120 && global.total[1] == 60 && global.total[3] == 180);
  assert(loadTimeBookHash(pathHash(first), book) && book.total[0] == 120 && book.total[3] == 180);
  assert(loadTimeBook(second, other) && other.total[1] == 60);
  assert(record(first, 40, 2, 0, "One", "A"));  // No trustworthy clock.
  assert(loadTimeGlobal(global) && global.total[0] == 120 && global.total[3] == 180);
  Snapshot lifetime{};
  assert(loadGlobal(lifetime) && lifetime.totalSeconds() == 400);

  // Fail the global time write after both native snapshots and the book time
  // file have been saved. The retained journal must replay exactly once.
  Storage.failRenameTo = globalTime;
  assert(!record(first, 90, 2, localEpoch(2026, 11, 19, 8), "One", "A"));
  assert(Storage.exists(pending));
  Storage.failRenameTo.clear();
  assert(loadGlobal(lifetime) && lifetime.totalSeconds() == 490);
  assert(lifetime.secondsOnDay(lifetime.latestDay) == 90);
  assert(lifetime.secondsOnDay(lifetime.latestDay - 1) == 360);
  assert(!Storage.exists(pending));
  assert(loadTimeGlobal(global) && global.total[0] == 210);
  assert(loadTimeBook(first, book) && book.total[0] == 210);
  assert(loadGlobal(lifetime) && lifetime.totalSeconds() == 490);
  assert(loadTimeGlobal(global) && global.total[0] == 210);
  assert(loadTimeBook(first, book) && book.total[0] == 210);

  // Removing a book clears its per-book time profile without altering global.
  assert(removeBookRecord(pathHash(first)));
  assert(!loadTimeBook(first, book));
  assert(loadTimeGlobal(global) && global.total[0] == 210);
  assert(loadGlobal(lifetime) && lifetime.totalSeconds() == 490);

  std::puts("Time of day: book/global scopes, unknown clock, crash replay, deletion PASS");
  return 0;
}
