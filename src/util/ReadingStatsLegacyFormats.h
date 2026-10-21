#pragma once

#include <cstddef>
#include <cstdint>

namespace readingstats::legacy {

// CrossInk BookReadingStats and GlobalReadingStats use little-endian packed
// byte streams, not native C++ struct layouts. Never reinterpret_cast them.
struct Counters {
  uint32_t sessions = 0;
  uint64_t totalSeconds = 0;
  uint32_t turns = 0;
};

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// CrossInk /.crosspoint/global_stats.bin: v1=13, v2=17, v3=159 bytes.
inline bool crossInkGlobal(const uint8_t* data, size_t size, Counters& out) {
  if (!data || !size) return false;
  if ((data[0] == 1 && size != 13) || (data[0] == 2 && size != 17) || (data[0] == 3 && size != 159) || data[0] < 1 ||
      data[0] > 3)
    return false;
  out = {};
  out.sessions = le32(data + 1);
  out.totalSeconds = le32(data + 5);
  out.turns = le32(data + 9);
  return true;
}

// CrossInk book cache stats: v1=11, v2=12, v3=16, v4=69,
// v5=73 bytes. Session count is number of opens, not necessarily >=60s.
inline bool crossInkBook(const uint8_t* data, size_t size, Counters& out) {
  if (!data || !size) return false;
  const size_t expected[] = {0, 11, 12, 16, 69, 73};
  if (data[0] < 1 || data[0] > 5 || size != expected[data[0]]) return false;
  out = {};
  out.sessions = le16(data + 1);
  out.totalSeconds = le32(data + 3);
  out.turns = le32(data + 7);
  return true;
}

// vCodex TimeUtils::daysFromCivil() is zero-based at 1970-01-01.
// Razor-Crest Snapshot day ordinals are one-based. The import must
// not shift historical dates into an adjacent day.
inline bool vCodexDay(uint32_t legacyDay, uint32_t& out) {
  if (legacyDay == UINT32_MAX) return false;
  out = legacyDay + 1;
  return true;
}
}  // namespace readingstats::legacy
