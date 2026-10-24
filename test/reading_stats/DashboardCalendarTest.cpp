#include <cassert>
#include <cstdint>

#include "../../src/activities/plugins/DashboardCalendar.h"

int main() {
  using namespace dashboardcalendar;
  // Unix epoch Thursday, Jan 1, 1970; column 0 means Monday.
  assert(mondayWeekday(0) == 3);
  assert(mondayWeekday(1) == 4);
  assert(mondayWeekday(2) == 5);
  assert(mondayWeekday(3) == 6);
  assert(mondayWeekday(4) == 0);
  assert(mondayWeekday(10) == 6);
  assert(fourWeekMonday(0) == 0);
  // Last recorded day is Friday. Four aligned rows start on Monday.
  assert(fourWeekMonday(29) == 4);
  for (int i = 0; i < 28; ++i) {
    const uint32_t day = fourWeekMonday(29) + static_cast<uint32_t>(i);
    assert(mondayWeekday(day) == i % 7);
  }
  assert(fourWeekMonday(31) == 4);  // Sunday stays in that final row.
  assert(fourWeekMonday(32) == 11); // Next Monday advances the display week.
}
