#pragma once

#include <algorithm>

// Device-agnostic spare-space geometry for optional SD theme decoration.
// No allocation, hardware dependencies, or free-heap-based behavior.
namespace screendecor {
struct Band {
  int top = 0;
  int bottom = 0;
  bool valid() const { return bottom - top >= 72; }
};

inline Band afterContent(int screenHeight, int buttonHintHeight, int contentBottom) {
  if (screenHeight <= 0) return {};
  const int bottom = std::max(0, screenHeight - std::max(0, buttonHintHeight) - 8);
  return Band{std::clamp(contentBottom + 12, 0, screenHeight), bottom};
}

inline Band forList(int screenHeight, int buttonHintHeight, int headerBottom, int extraBand, int count, int rowHeight,
                    int rowGap) {
  if (count <= 0 || count > 8) return {};  // avoid status and scroll-heavy screens
  const int safeCount = std::clamp(count, 0, 8);
  const int perRow = std::clamp(rowHeight, 0, 200) + std::clamp(rowGap, 0, 60);
  return afterContent(screenHeight, buttonHintHeight, headerBottom + std::max(0, extraBand) + safeCount * perRow);
}
}  // namespace screendecor
