#include <cassert>

#include "../../src/util/ReadingStatsSnapshot.h"

using readingstats::Snapshot;
using readingstats::snapshot::civilDay;
using readingstats::snapshot::summarize;

int main() {
  assert(civilDay(1970, 1, 1) == 1);
  assert(civilDay(2024, 2, 29) + 1 == civilDay(2024, 3, 1));
  assert(civilDay(2025, 2, 29) == 0);
  assert(civilDay(2026, 0, 1) == 0);
  assert(civilDay(2026, 13, 1) == 0);
  assert(civilDay(2026, 4, 31) == 0);

  const uint32_t first = civilDay(2026, 10, 1);
  const uint32_t today = civilDay(2026, 10, 29);
  Snapshot stats{};
  assert(!summarize(stats, 0, first).dateKnown);
  assert(!summarize(stats, today, 0).dateKnown);
  assert(!summarize(stats, first - 1, first).dateKnown);
  auto empty = summarize(stats, today, first);
  assert(empty.dateKnown && empty.monthSeconds == 0 && empty.activeDays == 0 && empty.streak == 0);

  stats.addSession(3600, 3, first - 1);  // Prior month excluded.
  stats.addSession(60, 1, first);
  stats.addSession(600, 2, today - 3);
  stats.addSession(900, 3, today - 2);
  stats.addSession(1200, 4, today - 1);
  auto yesterday = summarize(stats, today, first);
  assert(yesterday.monthSeconds == 2760);
  assert(yesterday.activeDays == 4);
  assert(yesterday.streak == 3);  // Still current before today's first session.

  stats.addSession(300, 1, today);
  auto current = summarize(stats, today, first);
  assert(current.monthSeconds == 3060 && current.activeDays == 5 && current.streak == 4);

  auto nextDay = summarize(stats, today + 1, first);
  assert(nextDay.streak == 4);  // Today's last read becomes yesterday's.

  auto missed = summarize(stats, today + 2, first);
  assert(missed.streak == 0);

  const uint32_t november = civilDay(2026, 11, 1);
  auto nextMonth = summarize(stats, november, november);
  assert(nextMonth.monthSeconds == 0 && nextMonth.activeDays == 0 && nextMonth.streak == 0);

  // A streak can span the month boundary, while monthly totals cannot.
  Snapshot boundary{};
  boundary.addSession(90, 1, november - 1);
  boundary.addSession(90, 1, november);
  auto across = summarize(boundary, november, november);
  assert(across.monthSeconds == 90 && across.activeDays == 1 && across.streak == 2);

  // Unknown local dates contribute to lifetime totals but never to a month.
  Snapshot undated{};
  undated.addSession(500, 1, 0);
  auto notCounted = summarize(undated, today, first);
  assert(notCounted.monthSeconds == 0 && notCounted.activeDays == 0 && notCounted.streak == 0);
}
