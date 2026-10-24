#pragma once

#include <cstdint>

namespace dashboardcalendar {
// Native reading-statistics days are local Unix-day ordinals, with 0 unknown.
// 1970-01-01 was Thursday, so Monday is index zero at (ordinal + 3) % 7.
inline uint8_t mondayWeekday(uint32_t day) {
  return static_cast<uint8_t>(((day % 7u) + 3u) % 7u);
}

// Four visible calendar rows, each Monday through Sunday. The last row is
// the week containing the most recent reading day; future cells stay empty.
inline uint32_t fourWeekMonday(uint32_t latestDay) {
  if (latestDay == 0) return 0;
  const uint32_t back = 21u + mondayWeekday(latestDay);
  return latestDay >= back ? latestDay - back : 0;
}
}  // namespace dashboardcalendar
