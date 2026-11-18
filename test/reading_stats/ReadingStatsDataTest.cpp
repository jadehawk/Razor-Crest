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

  // Old journal files do not encode an hour and must keep their day ordinal.
  readingstats::Pending oldTxn{};
  oldTxn.day = 20005;
  assert(oldTxn.localDay() == 20005);
  assert(oldTxn.dayPeriod() == -1);
  assert(readingstats::periodForHour(0) == 3);
  assert(readingstats::periodForHour(5) == 3);
  assert(readingstats::periodForHour(6) == 0);
  assert(readingstats::periodForHour(11) == 0);
  assert(readingstats::periodForHour(12) == 1);
  assert(readingstats::periodForHour(16) == 1);
  assert(readingstats::periodForHour(17) == 2);
  assert(readingstats::periodForHour(20) == 2);
  assert(readingstats::periodForHour(21) == 3);
  oldTxn.day = readingstats::packDayPeriod(20005, 2);
  assert(oldTxn.localDay() == 20005);
  assert(oldTxn.dayPeriod() == 2);
  assert(readingstats::packDayPeriod(0, 1) == 0);
  assert(readingstats::packDayPeriod(20005, 4) == 20005);

  readingstats::TimeBuckets hours{};
  assert(!hours.valid());
  hours.addSession(60, 20000, 0);  // Morning, old day.
  hours.addSession(90, 20000, 3);  // Night, same day.
  hours.addSession(30, 20001, 1);
  assert(hours.total[0] == 60);
  assert(hours.total[1] == 30);
  assert(hours.total[3] == 90);
  assert(hours.secondsInLastDays(20001, 0) == 60);
  assert(hours.secondsInLastDays(20001, 3) == 90);
  assert(hours.secondsInLastDays(20001, 1) == 30);
  assert(hours.secondsInLastDays(20001, 2) == 0);
  assert(hours.secondsInLastDays(20002, 0, 1) == 0);
  hours.addSession(120, 20007, 2);  // Expire day 20000 on rollover.
  assert(hours.secondsInLastDays(20007, 0) == 0);
  assert(hours.secondsInLastDays(20007, 2) == 120);
  assert(hours.total[0] == 60);    // Lifetime totals survive the rollover.
  hours.addSession(50, 20000, 1);  // Expired day does not re-enter the week.
  assert(hours.total[1] == 80);
  assert(hours.secondsInLastDays(20007, 1) == 30);
  hours.addSession(100, 0, 0);  // Unknown clock must not invent a period.
  assert(hours.total[0] == 60);
  hours.total[3] = UINT32_MAX - 10;
  hours.addSession(20, 20008, 3);
  assert(hours.total[3] == UINT32_MAX);
  hours.seal();
  assert(hours.valid());
  hours.recent[20008 % readingstats::kTimeHistoryDays][3]++;
  assert(!hours.valid());
}
