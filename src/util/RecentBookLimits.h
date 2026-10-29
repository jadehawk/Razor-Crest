#pragma once

#include <cstdint>

// Per-hardware safety ceilings for stored recent books and the Home carousel.
// No PSRAM: retain the existing 10-entry storage limit exactly.
// With PSRAM: allow up to 20 paths; never hold 20 decoded cover buffers.
namespace recentbooklimits {
constexpr uint8_t NO_PSRAM = 10;
constexpr uint8_t WITH_PSRAM = 20;
constexpr uint8_t AUTO = 0;

constexpr uint8_t deviceMaximum(bool hasPsram) { return hasPsram ? WITH_PSRAM : NO_PSRAM; }

// The persisted setting is a requested count, or zero for device-default.
// Clamp when used, so a preference copied from a PSRAM reader is safe on X3.
constexpr uint8_t effective(uint8_t requested, bool hasPsram) {
  const auto max = deviceMaximum(hasPsram);
  if (requested == AUTO) return max;
  return requested > max ? max : (requested < 1 ? 1 : requested);
}
}  // namespace recentbooklimits
