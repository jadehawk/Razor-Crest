#include <cassert>
#include <cstdint>

#include "AutoOrientationDetector.h"

static void expectStableRotation(AutoOrientationDetector& detector, float ax, float ay, float az,
                                 uint8_t from, int8_t to, uint32_t start) {
  detector.reset();
  assert(detector.update(ax, ay, az, from, start) == -1);
  assert(detector.update(ax, ay, az, from, start + AutoOrientationDetector::STABLE_MS - 1) == -1);
  assert(detector.update(ax, ay, az, from, start + AutoOrientationDetector::STABLE_MS) == to);
  assert(detector.update(ax, ay, az, static_cast<uint8_t>(to), start + 1000) == -1);
}

int main() {
  AutoOrientationDetector detector;

  // X3 native panel is landscape. In upright portrait, its +X axis
  // points downward; the accelerometer reports -X (acceleration upward).
  expectStableRotation(detector, -1, 0, 0, 3, 0, 100);   // Portrait
  expectStableRotation(detector, 0, 1, 0, 0, 1, 1200);   // Portrait -> CW landscape
  expectStableRotation(detector, 0, -1, 0, 0, 3, 2300);  // Portrait -> CCW landscape
  expectStableRotation(detector, 1, 0, 0, 1, 2, 3400);   // Inverted portrait
  expectStableRotation(detector, -1, 0, 0, 2, 0, 4500);  // Inverted -> portrait

  // The held orientation, lying flat, and ambiguous angles cannot rotate.
  assert(detector.update(-1, 0, 0, 0, 5600) == -1);
  assert(detector.update(0, 0, 1, 0, 6501) == -1);
  assert(detector.update(0.7f, 0.7f, 0, 0, 6600) == -1);
  assert(detector.update(2.0f, 0, 0, 0, 6700) == -1);
  assert(detector.update(0.3f, 0, 0, 0, 6800) == -1);

  // A temporary flat position cancels an incomplete rotation.
  detector.reset();
  assert(detector.update(0, 1, 0, 0, 7000) == -1);
  assert(detector.update(0, 0, 1, 0, 7600) == -1);
  assert(detector.update(0, 1, 0, 0, 7800) == -1);
  assert(detector.update(0, 1, 0, 0, 8500) == -1);
  assert(detector.update(0, 1, 0, 0, 8700) == 1);

  // A changed intended direction restarts stabilization, not the old timer.
  detector.reset();
  assert(detector.update(0, 1, 0, 0, 9000) == -1);
  assert(detector.update(0, -1, 0, 0, 9400) == -1);
  assert(detector.update(0, -1, 0, 0, 10300) == 3);

  // unsigned millis() wraparound retains the debounce duration.
  detector.reset();
  assert(detector.update(0, 1, 0, 0, UINT32_MAX - 400) == -1);
  assert(detector.update(0, 1, 0, 0, 600) == 1);

  // Reading in bed: a 60-69 degree tilt must not switch to landscape, even
  // after the entire stabilization period. An intentional 71-degree turn does.
  detector.reset();
  assert(detector.update(-0.5f, 0.8660254f, 0, 0, 11000) == -1);       // 60 degrees CW
  assert(detector.update(-0.5f, 0.8660254f, 0, 0, 12000) == -1);
  assert(detector.update(-0.358368f, 0.933580f, 0, 0, 13000) == -1);  // 69 degrees CW
  assert(detector.update(-0.358368f, 0.933580f, 0, 0, 14000) == -1);
  assert(detector.update(-0.325568f, 0.945519f, 0, 0, 15000) == -1);  // 71 degrees CW
  assert(detector.update(-0.325568f, 0.945519f, 0, 0, 15899) == -1);
  assert(detector.update(-0.325568f, 0.945519f, 0, 0, 15900) == 1);

  // The opposite turn applies the same threshold for CCW landscape.
  expectStableRotation(detector, -0.325568f, -0.945519f, 0, 0, 3, 17000);

  // Hysteresis: once in landscape, a small change back does not flip it.
  detector.reset();
  assert(detector.update(-0.5f, 0.8660254f, 0, 1, 19000) == -1);
  assert(detector.update(-0.5f, 0.8660254f, 0, 1, 20100) == -1);
  expectStableRotation(detector, -0.945519f, 0.325568f, 0, 1, 0, 21000);

  // A mostly face-up device is ignored, even with the right in-plane angle.
  detector.reset();
  assert(detector.update(-0.195f, 0.568f, 0.8f, 0, 24000) == -1);
  assert(detector.update(-0.195f, 0.568f, 0.8f, 0, 25100) == -1);
}
