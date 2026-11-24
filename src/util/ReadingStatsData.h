#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace readingstats {

// Fixed-scalar, allocation-free persisted records. No std::string or vector
// appears in the on-disk layout, so all devices share the same format.
constexpr uint32_t kMagic = 0x32545352;  // "RST2"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kHistoryDays = 366;
constexpr uint32_t kTimeHistoryDays = 7;
constexpr uint32_t kDayOrdinalMask = 0x1FFFFFFFu;

// Day ordinals fit in 29 bits. The high bits of the existing pending journal
// encode 1..4 for the local session-start period (0 means unknown/old journal).
inline uint32_t packDayPeriod(uint32_t ordinal, uint8_t period) {
  return ordinal && period < 4 ? (ordinal & kDayOrdinalMask) | (static_cast<uint32_t>(period + 1) << 29) : ordinal;
}

inline uint8_t periodForHour(uint8_t hour) {
  if (hour >= 6 && hour < 12) return 0;   // Morning
  if (hour >= 12 && hour < 17) return 1;  // Afternoon
  if (hour >= 17 && hour < 21) return 2;  // Evening
  return 3;                               // Night (21:00-05:59)
}

inline uint32_t checksumBytes(const void* bytes, size_t len) {
  uint32_t hash = 2166136261u;
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < len; ++i) hash = (hash ^ data[i]) * 16777619u;
  return hash;
}

struct Snapshot {
  uint32_t magic = kMagic;
  uint32_t version = kVersion;
  uint32_t lastSequence = 0;
  uint32_t sessions = 0;
  uint32_t totalSecondsLo = 0;
  uint32_t totalSecondsHi = 0;
  uint32_t pageTurns = 0;
  uint32_t latestDay = 0;
  uint32_t daySeconds[kHistoryDays] = {};
  uint32_t checksum = 0;

  uint64_t totalSeconds() const { return (static_cast<uint64_t>(totalSecondsHi) << 32) | totalSecondsLo; }
  uint32_t calculateChecksum() const { return checksumBytes(this, offsetof(Snapshot, checksum)); }
  bool valid() const { return magic == kMagic && version == kVersion && checksum == calculateChecksum(); }
  void seal() { checksum = calculateChecksum(); }

  void addSession(uint32_t seconds, uint32_t qualifiedTurns, uint32_t localDay) {
    if (seconds == 0) return;
    if (seconds >= 10 && sessions != UINT32_MAX) ++sessions;
    const uint64_t old = totalSeconds();
    const uint64_t updated = old > UINT64_MAX - seconds ? UINT64_MAX : old + seconds;
    totalSecondsLo = static_cast<uint32_t>(updated);
    totalSecondsHi = static_cast<uint32_t>(updated >> 32);
    pageTurns = qualifiedTurns > UINT32_MAX - pageTurns ? UINT32_MAX : pageTurns + qualifiedTurns;

    // Unknown dates count in totalSeconds but never fake today's activity.
    if (localDay == 0) return;
    if (latestDay == 0) {
      latestDay = localDay;
    } else if (localDay > latestDay) {
      const uint32_t advance = localDay - latestDay;
      if (advance >= kHistoryDays) {
        std::fill_n(daySeconds, kHistoryDays, 0u);
      } else {
        for (uint32_t i = 1; i <= advance; ++i) daySeconds[(latestDay + i) % kHistoryDays] = 0;
      }
      latestDay = localDay;
    }
    if (latestDay - localDay >= kHistoryDays) return;
    uint32_t& day = daySeconds[localDay % kHistoryDays];
    day = seconds > UINT32_MAX - day ? UINT32_MAX : day + seconds;
  }

  uint32_t secondsOnDay(uint32_t day) const {
    if (latestDay == 0 || day > latestDay || latestDay - day >= kHistoryDays) return 0;
    return daySeconds[day % kHistoryDays];
  }
};

// A small separate record preserves the existing native statistics format and
// contains only sessions with a trustworthy local start hour. Imports and older
// reading sessions have no time-of-day data and are not fabricated.
struct TimeBuckets {
  uint32_t magic = 0x31505452;  // RTP1
  uint32_t version = 1;
  uint32_t lastSequence = 0;
  uint32_t latestDay = 0;
  uint32_t total[4] = {};
  uint32_t recent[kTimeHistoryDays][4] = {};
  uint32_t checksum = 0;

  uint32_t calculateChecksum() const { return checksumBytes(this, offsetof(TimeBuckets, checksum)); }
  bool valid() const { return magic == 0x31505452 && version == 1 && checksum == calculateChecksum(); }
  void seal() { checksum = calculateChecksum(); }

  void addSession(uint32_t seconds, uint32_t day, uint8_t period) {
    if (!seconds || !day || period >= 4) return;
    uint32_t& bucket = total[period];
    bucket = seconds > UINT32_MAX - bucket ? UINT32_MAX : bucket + seconds;
    if (!latestDay) latestDay = day;
    if (day > latestDay) {
      const uint32_t advance = day - latestDay;
      if (advance >= kTimeHistoryDays) {
        for (auto& row : recent) std::fill_n(row, 4, 0u);
      } else {
        for (uint32_t i = 1; i <= advance; ++i) std::fill_n(recent[(latestDay + i) % kTimeHistoryDays], 4, 0u);
      }
      latestDay = day;
    }
    if (day > latestDay || latestDay - day >= kTimeHistoryDays) return;
    uint32_t& daily = recent[day % kTimeHistoryDays][period];
    daily = seconds > UINT32_MAX - daily ? UINT32_MAX : daily + seconds;
  }

  uint64_t secondsInLastDays(uint32_t endDay, uint8_t period, uint8_t days = kTimeHistoryDays) const {
    if (!endDay || period >= 4) return 0;
    uint64_t seconds = 0;
    for (uint32_t i = 0; i < days && i < kTimeHistoryDays && i < endDay; ++i) {
      const uint32_t day = endDay - i;
      if (day <= latestDay && latestDay - day < kTimeHistoryDays) seconds += recent[day % kTimeHistoryDays][period];
    }
    return seconds;
  }
};

// The single transaction marker makes a book snapshot + the global snapshot
// recoverable if power is lost between the two file updates.
struct Pending {
  uint32_t magic = 0x314E5854;  // "TXN1"
  uint32_t sequence = 0;
  uint32_t seconds = 0;
  uint32_t turns = 0;
  uint32_t day = 0;
  uint32_t bookHashLo = 0;
  uint32_t bookHashHi = 0;
  uint32_t checksum = 0;

  uint64_t bookHash() const { return (static_cast<uint64_t>(bookHashHi) << 32) | bookHashLo; }
  uint32_t localDay() const { return day & kDayOrdinalMask; }
  int8_t dayPeriod() const {
    const uint32_t encoded = day >> 29;
    return encoded >= 1 && encoded <= 4 && localDay() ? static_cast<int8_t>(encoded - 1) : -1;
  }
  void setBookHash(uint64_t hash) {
    bookHashLo = static_cast<uint32_t>(hash);
    bookHashHi = static_cast<uint32_t>(hash >> 32);
  }
  void seal() { checksum = checksumBytes(this, offsetof(Pending, checksum)); }
  bool valid() const {
    return magic == 0x314E5854 && sequence > 0 && seconds >= 10 &&
           checksum == checksumBytes(this, offsetof(Pending, checksum));
  }
};

inline bool applyOnce(Snapshot& stats, const Pending& pending) {
  if (stats.lastSequence >= pending.sequence) return false;
  stats.addSession(pending.seconds, pending.turns, pending.localDay());
  stats.lastSequence = pending.sequence;
  return true;
}

static_assert(sizeof(Snapshot) == (9 + kHistoryDays) * sizeof(uint32_t), "Unexpected stats layout");
static_assert(sizeof(Pending) == 8 * sizeof(uint32_t), "Unexpected pending layout");
static_assert(sizeof(TimeBuckets) == (4 + 4 + kTimeHistoryDays * 4 + 1) * sizeof(uint32_t),
              "Unexpected time buckets layout");

}  // namespace readingstats
