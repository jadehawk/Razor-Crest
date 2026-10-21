#pragma once

#include <algorithm>
#include <cstdint>

#include "ReadingStatsData.h"

namespace readingstats::legacy {

// In-place, allocation-free arithmetic. No second 1.5 KB Snapshot on the
// small X3 task stack. This is only called after a durable import marker
// exists, and the caller seals and commits the result transactionally.
inline bool mergeSnapshot(Snapshot& destination, const Snapshot& delta) {
  if (!delta.valid() || destination.magic != kMagic || destination.version != kVersion ||
      (destination.checksum != 0 && !destination.valid()))
    return false;
  const uint64_t left = destination.totalSeconds();
  const uint64_t right = delta.totalSeconds();
  const uint64_t total = left > UINT64_MAX - right ? UINT64_MAX : left + right;
  destination.totalSecondsLo = static_cast<uint32_t>(total);
  destination.totalSecondsHi = static_cast<uint32_t>(total >> 32);
  destination.sessions =
      delta.sessions > UINT32_MAX - destination.sessions ? UINT32_MAX : destination.sessions + delta.sessions;
  destination.pageTurns =
      delta.pageTurns > UINT32_MAX - destination.pageTurns ? UINT32_MAX : destination.pageTurns + delta.pageTurns;

  // First expire the slots that have fallen out of the 366-day window.
  // This preserves all still-valid destination slots without copying them.
  const uint32_t oldDay = destination.latestDay;
  const uint32_t latest = std::max(oldDay, delta.latestDay);
  if (latest > oldDay) {
    const uint32_t advance = latest - oldDay;
    if (advance >= kHistoryDays) {
      for (auto& value : destination.daySeconds) value = 0;
    } else {
      for (uint32_t i = 1; i <= advance; ++i) destination.daySeconds[(oldDay + i) % kHistoryDays] = 0;
    }
  }
  destination.latestDay = latest;
  for (uint32_t i = 0; i < kHistoryDays && delta.latestDay > i; ++i) {
    const uint32_t day = delta.latestDay - i;
    if (latest - day >= kHistoryDays) break;
    const uint32_t increment = delta.secondsOnDay(day);
    uint32_t& target = destination.daySeconds[day % kHistoryDays];
    target = increment > UINT32_MAX - target ? UINT32_MAX : target + increment;
  }
  destination.seal();
  return true;
}
}  // namespace readingstats::legacy
