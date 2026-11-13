#pragma once

#include <algorithm>

// The menu viewport is bounded by the book card, button hints and any
// external-theme footer. All enabled entries remain in the logical list.
namespace home_menu {
inline int safeBottom(int screenHeight, int hintsHeight, int footerHeight) {
  return screenHeight - std::max(0, hintsHeight) - std::max(0, footerHeight) - 8;
}

inline int capacityFor(int screenHeight, int menuTop, int rowHeight, int rowSpacing, int hintsHeight,
                       int footerHeight) {
  const int pitch = std::max(1, rowHeight + rowSpacing);
  return std::clamp((safeBottom(screenHeight, hintsHeight, footerHeight) - menuTop) / pitch, 1, 12);
}

inline int visibleRows(int total, int top, int capacity) {
  return std::min(std::max(0, total - top), std::max(1, capacity));
}

inline int scrollTopForSelection(int top, int selected, int total, int capacity) {
  const int rows = std::max(1, capacity);
  const int maxTop = std::max(0, total - rows);
  top = std::clamp(top, 0, maxTop);
  if (selected < 0 || selected >= total) return top;
  if (selected < top) return selected;
  if (selected >= top + rows) return std::min(maxTop, selected - rows + 1);
  return top;
}

inline int scrollTopBy(int top, int delta, int total, int capacity) {
  return std::clamp(top + delta, 0, std::max(0, total - std::max(1, capacity)));
}
}  // namespace home_menu
