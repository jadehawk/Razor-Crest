#include <cassert>
#include <initializer_list>

#include "../src/util/RecentBookLimits.h"

int main() {
  using namespace recentbooklimits;
  static_assert(NO_PSRAM == 10);
  static_assert(WITH_PSRAM == 50);
  static_assert(deviceMaximum(false) == 10);
  static_assert(deviceMaximum(true) == 50);
  static_assert(effective(AUTO, false) == 10);
  static_assert(effective(AUTO, true) == 50);
  static_assert(effective(5, false) == 5);
  static_assert(effective(5, true) == 5);
  static_assert(effective(10, false) == 10);
  static_assert(effective(50, true) == 50);
  static_assert(effective(50, false) == 10);
  static_assert(effective(255, false) == 10);
  static_assert(effective(255, true) == 50);
  // A configured smaller window does not lower the storage ceiling.
  for (int requested : {5, 10, 20, 30, 40, 50}) {
    assert(effective(static_cast<uint8_t>(requested), false) <= NO_PSRAM);
    assert(effective(static_cast<uint8_t>(requested), true) <= WITH_PSRAM);
  }
  // Synthetic 50-book history is representable without creating 50 covers.
  int stored = 0;
  for (int i = 0; i < 100; ++i)
    if (stored < deviceMaximum(true)) ++stored;
  assert(stored == 50);
  return 0;
}
