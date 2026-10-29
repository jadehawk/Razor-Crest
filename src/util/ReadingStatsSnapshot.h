#pragma once

#include <cstdint>

#include "ReadingStatsData.h"

namespace readingstats::snapshot {

// The stats store uses local civil-day ordinals: Unix day number + 1.
// Keep calendar arithmetic independent of the timezone and the firmware clock
// so the snapshot can be verified by small host-side tests.
inline uint32_t civilDay(const int year, const unsigned month, const unsigned day) {
  if (year < 1970 || month < 1 || month > 12 || day < 1) return 0;
  constexpr uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (day > monthDays[month - 1] + (month == 2 && leap ? 1u : 0u)) return 0;
  int y = year - (month <= 2 ? 1 : 0);
  const int era = y / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned shifted = month > 2 ? month - 3 : month + 9;
  const unsigned doy = (153 * shifted + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t ordinal = static_cast<int64_t>(era) * 146097 + doe - 719468 + 1;
  return ordinal > 0 && ordinal <= UINT32_MAX ? static_cast<uint32_t>(ordinal) : 0;
}

struct Summary {
  uint64_t monthSeconds = 0;
  uint32_t activeDays = 0;
  uint32_t streak = 0;
  bool dateKnown = false;
};

inline Summary summarize(const Snapshot& stats, const uint32_t today, const uint32_t monthStart) {
  Summary result;
  if (today == 0 || monthStart == 0 || monthStart > today) return result;
  result.dateKnown = true;
  for (uint32_t day = monthStart; day <= today; ++day) {
    const uint32_t seconds = stats.secondsOnDay(day);
    if (seconds) {
      result.monthSeconds += seconds;
      ++result.activeDays;
    }
  }

  // A streak survives until the end of the day after the last reading day.
  // Never count an unrecorded day as reading activity.
  uint32_t day = today;
  if (stats.secondsOnDay(day) == 0) {
    if (day <= 1) return result;
    --day;
  }
  while (day > 0 && result.streak < kHistoryDays && stats.secondsOnDay(day) != 0) {
    ++result.streak;
    --day;
  }
  return result;
}

}  // namespace readingstats::snapshot
