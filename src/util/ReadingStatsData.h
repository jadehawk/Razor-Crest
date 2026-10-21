#pragma once

#include <cstddef>
#include <cstdint>

namespace readingstats {

// Fixed-scalar, allocation-free persisted records. No std::string or vector
// appears in the on-disk layout, so all devices share the same format.
constexpr uint32_t kMagic = 0x32545352;  // "RST2"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kHistoryDays = 366;

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
    if (seconds >= 60 && sessions != UINT32_MAX) ++sessions;
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
        for (auto& day : daySeconds) day = 0;
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
  stats.addSession(pending.seconds, pending.turns, pending.day);
  stats.lastSequence = pending.sequence;
  return true;
}

static_assert(sizeof(Snapshot) == (9 + kHistoryDays) * sizeof(uint32_t), "Unexpected stats layout");
static_assert(sizeof(Pending) == 8 * sizeof(uint32_t), "Unexpected pending layout");

}  // namespace readingstats
