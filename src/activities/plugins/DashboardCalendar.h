#pragma once

#include <cstdint>

#include "../../util/ReadingStatsSnapshot.h"

namespace dashboardcalendar {

// Stored day ordinals are Unix days + 1. Monday is weekday column zero.
inline uint8_t mondayWeekday(uint32_t ordinal) { return ordinal ? static_cast<uint8_t>((ordinal + 2u) % 7u) : 0; }

struct Month {
  uint32_t firstDay = 0;
  uint32_t today = 0;
  uint64_t readingSeconds = 0;
  uint32_t bestSeconds = 0;
  uint8_t days = 0;
  uint8_t offset = 0;
  uint8_t rows = 0;
  uint8_t elapsedDays = 0;
  uint8_t activeDays = 0;
  uint8_t bestDay = 0;

  bool valid() const { return firstDay != 0; }
  uint32_t averageSeconds() const { return activeDays ? static_cast<uint32_t>((readingSeconds / activeDays)) : 0; }
  uint32_t consistencyPercent() const {
    return elapsedDays ? (static_cast<uint32_t>(activeDays) * 100u + elapsedDays / 2u) / elapsedDays : 0;
  }
};

inline Month summarizeMonth(const readingstats::Snapshot& stats, int year, unsigned month, unsigned day) {
  Month result;
  const uint32_t first = readingstats::snapshot::civilDay(year, month, 1);
  const uint32_t next = month == 12 ? readingstats::snapshot::civilDay(year + 1, 1, 1)
                                    : readingstats::snapshot::civilDay(year, month + 1, 1);
  const uint32_t today = readingstats::snapshot::civilDay(year, month, day);
  if (!first || !next || next <= first || !today || today < first || today >= next) return result;

  result.firstDay = first;
  result.today = today;
  result.days = static_cast<uint8_t>(next - first);
  result.offset = mondayWeekday(first);
  result.rows = static_cast<uint8_t>((result.offset + result.days + 6) / 7);
  result.elapsedDays = static_cast<uint8_t>(day);

  for (unsigned date = 1; date <= day; ++date) {
    const uint32_t seconds = stats.secondsOnDay(first + date - 1);
    result.readingSeconds += seconds;
    if (seconds > 0) ++result.activeDays;
    if (seconds > result.bestSeconds) {
      result.bestSeconds = seconds;
      result.bestDay = static_cast<uint8_t>(date);
    }
  }
  return result;
}

// The same shading thresholds are used by calendar cells and the legend.
// 0 is no reading, 1 is <15 minutes, 2 is 15-44 minutes, 3 is 45+ minutes.
inline uint8_t intensity(uint32_t seconds) {
  if (!seconds) return 0;
  if (seconds < 900) return 1;
  if (seconds < 2700) return 2;
  return 3;
}

}  // namespace dashboardcalendar
