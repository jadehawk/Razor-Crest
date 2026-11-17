#pragma once

// Shared interaction regions for V1 Home grids. Logical book/menu actions
// stay in HomeActivity; this class never duplicates that state machine.
// A caller must use these SAME rectangles for painting and hit detection.
#include <algorithm>
#include <cstdint>

#include "components/home/HomeWidgetEngine.h"
#include "util/HomeMenuGeometry.h"

namespace home_grid::interaction {

struct Regions {
  home_grid::Rect menu{};
  home_grid::Rect book{};
  home_grid::Rect bookScene{};
  int menuWidget = -1;
  int bookWidget = -1;
};

// Grid input V1 has one logical book-card and one scrolling shortcut list.
// Noninteractive grid slots may be added freely, but duplicate focusable
// book panels would have ambiguous old Home carousel semantics.
inline bool locate(const home_widgets::Plan& plan,
                   const home_grid::Result& result, Regions& out) {
  out = {};
  if (!result.widgetCount || result.widgetCount != plan.widgetCount ||
      result.widgetCount > home_grid::kMaxWidgets) return false;
  for (int i = 0; i < result.widgetCount; ++i) {
    const auto kind = plan.widgets[i].kind;
    if (kind == home_widgets::Kind::Shortcuts) {
      if (out.menuWidget >= 0) return false;
      out.menuWidget = i;
      out.menu = result.widgets[i];
    } else if (kind == home_widgets::Kind::Book) {
      if (out.bookWidget >= 0) return false;
      out.bookWidget = i;
      out.book = result.widgets[i];
      out.bookScene = out.book;
    }
  }
  if (out.menuWidget < 0 || out.menu.width <= 0 || out.menu.height <= 0)
    return false;

  // Preserve legacy foreground BookCard's original Hero+Book anchor when
  // the decorative theme underlay spans its slot. Never move input bounds.
  for (int i = 0; i < plan.underlayCount; ++i) {
    const auto& layer = plan.underlays[i];
    if (out.bookWidget < 0) break;
    for (int row = layer.fromRow; row <= layer.throughRow && row < result.rowCount; ++row) {
      const home_grid::Rect rect = result.rows[row];
      if (!home_widgets::contains(rect, out.book)) continue;
      home_grid::Rect area;
      if (home_grid::spanRows(result, layer.fromRow, layer.throughRow, area))
        out.bookScene = area;
      break;
    }
  }
  return true;
}

inline bool inside(home_grid::Rect rect, int x, int y) {
  return rect.width > 0 && rect.height > 0 && x >= rect.x && y >= rect.y &&
         x < static_cast<int>(rect.x) + rect.width &&
         y < static_cast<int>(rect.y) + rect.height;
}

inline int visibleMenuCapacity(home_grid::Rect rect, int rowHeight, int rowSpacing,
                               bool footerHasArt = false) {
  if (rect.width < 1 || rowHeight <= 0 || rect.height < rowHeight) return 0;
  // Snoopy-like footer art retains the existing trailing row gap; otherwise
  // the final Home shortcut might be interactive but visually clipped.
  return home_menu::rowsFitting(rect.height, rowHeight, rowSpacing, footerHasArt);
}

// This is a *row in the visible viewport*, not an index into HomeMenuRegistry.
// Hitboxes include only the native row body, never the gap/scene underlay.
inline int menuHit(home_grid::Rect rect, int x, int y,
                   int rowHeight, int rowSpacing, int shownRows) {
  if (!inside(rect, x, y) || rowHeight <= 0 || shownRows <= 0) return -1;
  const int pitch = rowHeight + std::max(0, rowSpacing);
  if (pitch <= 0) return -1;
  const int relative = y - rect.y;
  const int row = relative / pitch;
  if (row < 0 || row >= shownRows || relative % pitch >= rowHeight)
    return -1;
  return row;
}
}  // namespace home_grid::interaction
