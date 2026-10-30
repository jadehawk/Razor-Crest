#pragma once

#include <cstdint>

// Test stub: only the sleep-screen mode values the util header compares against.
class CrossPointSettings {
 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    TRANSPARENT_CUSTOM = 7,
    COVER_TRANSPARENT = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
};
