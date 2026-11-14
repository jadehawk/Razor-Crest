#pragma once

#include <algorithm>

// The menu viewport is bounded by the book card, button hints and any
// external-theme footer. All enabled entries remain in the logical list.
namespace home_menu {
inline int safeBottom(int screenHeight, int hintsHeight, int footerHeight) {
  return screenHeight - std::max(0, hintsHeight) - std::max(0, footerHeight) - 8;
}

// Only gaps *between* rows consume space. A themed footer keeps the old
// trailing gap so decorative artwork remains protected.
inline int rowsFitting(int availableHeight, int rowHeight, int rowSpacing, bool trailingGap) {
  const int gap = std::max(0, rowSpacing);
  const int pitch = std::max(1, rowHeight + gap);
  return std::clamp((std::max(0, availableHeight) + (trailingGap ? 0 : gap)) / pitch, 1, 12);
}

// Touch-only, footer-free SD themes may trim a small amount of their hero
// panel if and only if it reveals one complete additional row.
inline int reclaimForNextRow(int availableHeight, int rowHeight, int rowSpacing, int maxReclaim) {
  const int nextRows = rowsFitting(availableHeight, rowHeight, rowSpacing, false) + 1;
  if (nextRows > 12) return 0;
  const int needed = nextRows * std::max(1, rowHeight) + (nextRows - 1) * std::max(0, rowSpacing) - availableHeight;
  return needed > 0 && needed <= maxReclaim ? needed : 0;
}

inline int capacityFor(int screenHeight, int menuTop, int rowHeight, int rowSpacing, int hintsHeight,
                       int footerHeight) {
  return rowsFitting(safeBottom(screenHeight, hintsHeight, footerHeight) - menuTop, rowHeight, rowSpacing,
                     footerHeight > 0);
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
