#pragma once

#include <cstdint>

#include "CrossPointSettings.h"

// Which sleep modes paint the custom source (live statistics or the selected
// wallpaper collection), and when that source is the one actually shown.
namespace sleepscreen {

constexpr bool usesCustomSource(const uint8_t mode) {
  return mode == CrossPointSettings::CUSTOM || mode == CrossPointSettings::COVER_CUSTOM;
}

constexpr bool showsCustomSource(const uint8_t mode, const bool lastSleepFromReader, const bool coverFallback) {
  return mode == CrossPointSettings::CUSTOM ||
         (mode == CrossPointSettings::COVER_CUSTOM && (!lastSleepFromReader || coverFallback));
}

}  // namespace sleepscreen
