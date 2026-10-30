#pragma once

#include <cmath>
#include <cstdint>

// Uses gravity to select one of the existing four reader orientations.
// Never allocates or wakes hardware; callers provide accelerometer samples.
class AutoOrientationDetector {
 public:
  static constexpr uint32_t STABLE_MS = 900;
  // Require the new screen axis to dominate the old axis by about tan(70°).
  // This keeps the reader in its current orientation when held diagonally.
  static constexpr float MIN_AXIS_DOMINANCE = 2.75f;

  void reset() {
    candidate = 0xFF;
    candidateSinceMs = 0;
  }

  // -1 means no change. Sensor axes are in the board frame supplied by the SDK.
  int8_t update(float ax, float ay, float az, uint8_t current, uint32_t now) {
    const float x = std::fabs(ax);
    const float y = std::fabs(ay);
    const float z = std::fabs(az);
    const float magnitudeSquared = ax * ax + ay * ay + az * az;
    const float dominant = x > y ? x : y;
    const float minor = x > y ? y : x;
    if (magnitudeSquared < 0.64f || magnitudeSquared > 1.69f || z > 0.75f ||
        dominant < 0.6f || dominant < MIN_AXIS_DOMINANCE * minor) {
      reset();
      return -1;
    }

    // SDK X3 axes follow the native landscape panel: +X points toward the
    // portrait page bottom, while +Y points toward the portrait page left.
    // Acceleration points upward, opposite gravity, when the device is still.
    const uint8_t direction = x > y ? (ax < 0 ? 0 : 2) : (ay > 0 ? 1 : 3);
    if (direction == current) {
      reset();
      return -1;
    }
    if (candidate != direction) {
      candidate = direction;
      candidateSinceMs = now;
      return -1;
    }
    if (now - candidateSinceMs < STABLE_MS) return -1;
    reset();
    return static_cast<int8_t>(direction);
  }

 private:
  uint8_t candidate = 0xFF;
  uint32_t candidateSinceMs = 0;
};
