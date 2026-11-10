#pragma once

#include <strings.h>

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

// True when a plugin "download" destination feeds the custom sleep source:
// /sleep.bmp (drawn first) or the /.sleep and /sleep wallpaper pools. Templates
// after the literal prefix still resolve under that directory. Transparent
// overlays (/.sleep-overlay) are not part of sleep-entry delivery.
inline bool isSleepImageDestination(const char* dest) {
  if (!dest || dest[0] != '/') return false;
  return strcasecmp(dest, "/sleep.bmp") == 0 || strncasecmp(dest, "/.sleep/", 8) == 0 ||
         strncasecmp(dest, "/sleep/", 7) == 0;
}

}  // namespace sleepscreen
