#include <cassert>
#include <climits>
#include <cstdint>

#include "../../src/util/ReadingStatsData.h"

using readingstats::kHistoryDays;
using readingstats::Snapshot;

int main() {
  Snapshot stats{};
  assert(stats.totalSeconds() == 0);
  assert(!stats.valid());
  stats.addSession(65, 3, 20000);
  stats.seal();
  assert(stats.valid());
  assert(stats.sessions == 1);
  assert(stats.totalSeconds() == 65);
  assert(stats.pageTurns == 3);
  assert(stats.secondsOnDay(20000) == 65);

  stats.addSession(9, 1, 20000);
  assert(stats.sessions == 1);  // Short reading contributes, not a new session.
  assert(stats.secondsOnDay(20000) == 74);
  stats.addSession(30, 2, 0);  // Unknown clock: all-time metrics still work.
  assert(stats.totalSeconds() == 104);
  assert(stats.secondsOnDay(20000) == 74);

  stats.addSession(90, 4, 20002);
  assert(stats.secondsOnDay(20001) == 0);
  assert(stats.secondsOnDay(20002) == 90);
  stats.addSession(60, 1, 20001);  // Earlier local day within retained window.
  assert(stats.secondsOnDay(20001) == 60);

  stats.addSession(120, 1, 20000 + kHistoryDays + 1);
  assert(stats.secondsOnDay(20000) == 0);
  assert(stats.secondsOnDay(20000 + kHistoryDays + 1) == 120);

  stats.seal();
  assert(stats.valid());
  stats.daySeconds[(20000 + kHistoryDays + 1) % kHistoryDays]++;
  assert(!stats.valid());  // Corrupt or incomplete SD record is rejected.

  Snapshot saturated{};
  saturated.totalSecondsHi = UINT32_MAX;
  saturated.totalSecondsLo = UINT32_MAX - 3;
  saturated.pageTurns = UINT32_MAX - 2;
  saturated.addSession(60, 5, 0);
  assert(saturated.totalSeconds() == UINT64_MAX);
  assert(saturated.pageTurns == UINT32_MAX);

  readingstats::Pending txn{};
  txn.sequence = 7;
  txn.seconds = 90;
  txn.turns = 4;
  txn.day = 20004;
  txn.setBookHash(0x0102030405060708ULL);
  txn.seal();
  assert(txn.valid());
  assert(txn.bookHash() == 0x0102030405060708ULL);
  Snapshot current{};
  assert(readingstats::applyOnce(current, txn));
  assert(current.lastSequence == 7);
  assert(current.totalSeconds() == 90);
  assert(!readingstats::applyOnce(current, txn));
  assert(current.totalSeconds() == 90);  // Restart/replay cannot double-count.
  txn.turns++;
  assert(!txn.valid());
}
