#include <cassert>
#include <cstdint>

#include "../../src/activities/plugins/DashboardCalendar.h"

int main() {
  using namespace dashboardcalendar;
  using readingstats::snapshot::civilDay;

  // Native ordinals start at one: 1970-01-01 is Thursday (Monday index 3).
  assert(mondayWeekday(0) == 0);
  assert(mondayWeekday(civilDay(1970, 1, 1)) == 3);
  assert(mondayWeekday(civilDay(1970, 1, 4)) == 6);
  assert(mondayWeekday(civilDay(1970, 1, 5)) == 0);

  readingstats::Snapshot stats{};
  const uint32_t november = civilDay(2026, 11, 1);
  stats.addSession(600, 1, november);        // Sun 1, light
  stats.addSession(1800, 1, november + 9);   // Tue 10, medium
  stats.addSession(3600, 1, november + 16);  // Tue 17, dark
  stats.addSession(7200, 1, november + 18);  // Thu 19, future from today 18

  auto month = summarizeMonth(stats, 2026, 11, 18);
  assert(month.valid() && month.today == civilDay(2026, 11, 18));
  assert(month.days == 30 && month.offset == 6 && month.rows == 6);
  assert(month.elapsedDays == 18 && month.activeDays == 3);
  assert(month.readingSeconds == 6000 && month.bestSeconds == 3600);
  assert(month.bestDay == 17 && month.averageSeconds() == 2000);
  assert(month.consistencyPercent() == 17);  // Rounded 3/18.

  assert(intensity(0) == 0);
  assert(intensity(1) == 1 && intensity(899) == 1);
  assert(intensity(900) == 2 && intensity(2699) == 2);
  assert(intensity(2700) == 3);

  // February month length, leap day and variable four/five/six-row grid.
  month = summarizeMonth(stats, 2027, 2, 28);
  assert(month.valid() && month.days == 28 && month.offset == 0 && month.rows == 4);
  assert(month.activeDays == 0 && month.bestDay == 0);
  assert(month.averageSeconds() == 0 && month.consistencyPercent() == 0);
  month = summarizeMonth(stats, 2028, 2, 29);
  assert(month.valid() && month.days == 29 && month.offset == 1 && month.rows == 5);
  month = summarizeMonth(stats, 2026, 12, 31);
  assert(month.valid() && month.days == 31 && month.offset == 1 && month.rows == 5);
  assert(civilDay(2027, 1, 1) - month.firstDay == 31);

  // Every month must occupy 4-6 complete Monday-first rows without
  // displacing its first or last numbered date.
  for (int year = 2020; year <= 2030; ++year) {
    for (unsigned m = 1; m <= 12; ++m) {
      const auto observed = summarizeMonth(stats, year, m, 1);
      assert(observed.valid());
      assert(observed.rows >= 4 && observed.rows <= 6);
      assert(observed.offset == mondayWeekday(observed.firstDay));
      assert(observed.rows == (observed.offset + observed.days + 6) / 7);
      assert(mondayWeekday(observed.firstDay + observed.days - 1) == (observed.offset + observed.days - 1) % 7);
    }
  }

  assert(!summarizeMonth(stats, 2026, 2, 29).valid());
  assert(!summarizeMonth(stats, 2026, 11, 31).valid());
  assert(!summarizeMonth(stats, 2026, 0, 1).valid());
  assert(!summarizeMonth(stats, 2019, 13, 1).valid());
}
