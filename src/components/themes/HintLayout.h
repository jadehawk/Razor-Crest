#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace hint_layout {
struct Box { int x; int width; };

// Allocate the entire band in physical key order. Short/empty labels donate
// space to longer ones; preserve the preferred width when space permits.
template <std::size_t N>
std::array<Box, N> allocate(int screen, int margin, int gap, const std::array<int, N>& needed,
                            int preferred, int minimum = 12) {
  std::array<Box, N> boxes{};
  const int room = std::max(0, screen - 2 * margin - gap * (static_cast<int>(N) - 1));
  std::array<int, N> widths{};
  int total = 0;
  for (std::size_t i = 0; i < N; ++i) {
    widths[i] = std::max(minimum, needed[i]);
    total += widths[i];
  }
  if (total > room) {
    // Impossible to fit all labels at this font. Bound every box; caller
    // can choose a smaller font or ellipsize without wrapping.
    int remaining = room;
    int demand = total;
    for (std::size_t i = 0; i < N; ++i) {
      const int requested = widths[i];
      widths[i] = demand ? remaining * requested / demand : 0;
      remaining -= widths[i];
      demand -= requested;
    }
  } else {
    int remaining = room - total;
    while (remaining > 0) {
      bool changed = false;
      for (std::size_t i = 0; i < N && remaining > 0; ++i) {
        if (widths[i] < preferred) {
          ++widths[i];
          --remaining;
          changed = true;
        }
      }
      if (!changed) break;
    }
    // Keep the whole row centered when its preferred widths fit.
  }
  int used = gap * (static_cast<int>(N) - 1);
  for (int w : widths) used += w;
  int x = (screen - used) / 2;
  for (std::size_t i = 0; i < N; ++i) {
    boxes[i] = {x, widths[i]};
    x += widths[i] + gap;
  }
  return boxes;
}
}  // namespace hint_layout
