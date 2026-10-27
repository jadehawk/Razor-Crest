#pragma once

#include <algorithm>

// One layout equation for Home and Home Menu Buttons. It reserves the header,
// featured-book card, real row pitch, physical hints, artwork footer and an
// additional 8px separation. Only complete rows count; Settings is mandatory.
namespace home_menu {
inline int capacityFor(int screenHeight, int menuTop, int rowHeight, int rowSpacing, int hintsHeight,
                       int footerHeight) {
  const int safeEnd = screenHeight - hintsHeight - footerHeight - 8;
  const int pitch = std::max(1, rowHeight + rowSpacing);
  return std::clamp((safeEnd - menuTop + rowSpacing) / pitch, 1, 12);
}
}  // namespace home_menu
