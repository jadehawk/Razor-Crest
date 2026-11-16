#pragma once

// Home Layout V1: bounded compositor and widget dispatch, independent of the
// device renderer. HomeActivity supplies existing native draw functions in
// Stage 4; SD parsing and provider discovery arrive in later stages.
#include <cstdint>

#include "util/HomeGridGeometry.h"

namespace home_widgets {
using home_grid::Rect;
constexpr int kMaxUnderlays = 2;

enum class Kind : uint8_t { Empty, Header, Scene, Book, Shortcuts, Clock, Metric, Plugin };
enum class Panel : uint8_t { Transparent, White, Black };
enum class BackgroundMode : uint8_t { None, White, Black, Bitmap };
enum class ImageFit : uint8_t { Cover, Contain, Native };
enum class PaintStatus : uint8_t { Ok, InvalidPlan, InvalidHooks };

struct Widget {
  Kind kind = Kind::Empty;
  Panel panel = Panel::Transparent;
  Kind fallback = Kind::Empty;  // Plugin: Empty or Scene only
  uint8_t reserved = 0;
  uint16_t resource = 0;       // Validated scene/metric/provider handle
  uint16_t fallbackResource = 0;
  uint16_t minWidth = 1;
  uint16_t minHeight = 1;
};

struct Background {
  BackgroundMode mode = BackgroundMode::None;
  ImageFit fit = ImageFit::Cover;
  bool inkOnly = false;
  uint8_t focusX = 50;  // Cropping/resampling is precomputed in Theme Studio.
  uint8_t focusY = 50;
  uint16_t bitmap = 0;  // Validated CPB asset handle
};

struct Underlay {
  uint8_t fromRow = 0;
  uint8_t throughRow = 0;
  uint16_t scene = 0;
};

struct Plan {
  Widget widgets[home_grid::kMaxWidgets]{};
  Background background{};
  Underlay underlays[kMaxUnderlays]{};
  uint8_t widgetCount = 0;
  uint8_t underlayCount = 0;
};

struct PaintHooks {
  void* user = nullptr;
  // push/pop must preserve the caller's prior clip, including on SD errors.
  void (*pushClip)(void*, Rect) = nullptr;
  void (*popClip)(void*) = nullptr;
  void (*fill)(void*, Rect, bool black) = nullptr;
  // Draw at the original canvas/scene coordinates within the current clip.
  bool (*bitmap)(void*, const Background& image, Rect canvas) = nullptr;
  bool (*scene)(void*, uint16_t scene, Rect area) = nullptr;
  // Header, book, shortcuts, clock and metric are native renderer adapters.
  bool (*native)(void*, Kind kind, uint16_t resource, Rect area) = nullptr;
  bool (*providerAvailable)(void*, uint16_t provider) = nullptr;
  // Native chrome (status, focus, hints) is *always* drawn last by HomeActivity.
  // HomeActivity owns it separately; it cannot be supplied by SD themes.
};

struct PaintReport {
  PaintStatus status = PaintStatus::InvalidPlan;
  uint8_t missingProviders = 0;
  uint8_t failedWidgets = 0;
  uint8_t failedUnderlays = 0;
  bool backgroundFallback = false;
};

static_assert(sizeof(Widget) <= 16, "Home widget descriptor has grown unexpectedly");
static_assert(sizeof(Plan) <= 320, "Home widget plan exceeds low-memory envelope");

inline bool contains(Rect parent, Rect child) {
  return child.width > 0 && child.height > 0 && child.x >= parent.x && child.y >= parent.y &&
         static_cast<int>(child.x) + child.width <= static_cast<int>(parent.x) + parent.width &&
         static_cast<int>(child.y) + child.height <= static_cast<int>(parent.y) + parent.height;
}

inline Rect intersect(Rect a, Rect b) {
  const int x = a.x > b.x ? a.x : b.x;
  const int y = a.y > b.y ? a.y : b.y;
  const int right = static_cast<int>(a.x) + a.width < static_cast<int>(b.x) + b.width
                        ? static_cast<int>(a.x) + a.width : static_cast<int>(b.x) + b.width;
  const int bottom = static_cast<int>(a.y) + a.height < static_cast<int>(b.y) + b.height
                         ? static_cast<int>(a.y) + a.height : static_cast<int>(b.y) + b.height;
  return Rect{static_cast<int16_t>(x), static_cast<int16_t>(y),
              static_cast<int16_t>(right > x ? right - x : 0),
              static_cast<int16_t>(bottom > y ? bottom - y : 0)};
}

inline bool validKind(Kind kind) {
  return kind >= Kind::Empty && kind <= Kind::Plugin;
}

inline bool validPanel(Panel panel) {
  return panel >= Panel::Transparent && panel <= Panel::Black;
}

inline bool focusable(Kind kind) {
  return kind == Kind::Book || kind == Kind::Shortcuts || kind == Kind::Plugin;
}

inline bool validPlan(const Plan& plan, const home_grid::Definition& def, const home_grid::Result& result) {
  if (plan.widgetCount != result.widgetCount || def.columnCount != result.widgetCount ||
      result.rowCount != def.rowCount || result.rowCount == 0 ||
      result.rowCount > home_grid::kMaxRows || result.widgetCount > home_grid::kMaxWidgets ||
      plan.underlayCount > kMaxUnderlays || !contains(result.canvas, result.rows[0]) ||
      plan.widgets[0].kind != Kind::Header || def.rows[0].columnCount != 1 ||
      plan.background.mode > BackgroundMode::Bitmap || plan.background.fit > ImageFit::Native ||
      plan.background.focusX > 100 || plan.background.focusY > 100)
    return false;
  for (int i = 0; i < result.widgetCount; ++i) {
    const Widget& widget = plan.widgets[i];
    if (!validKind(widget.kind) || !validPanel(widget.panel) ||
        !contains(result.canvas, result.widgets[i]) ||
        result.widgets[i].width < widget.minWidth || result.widgets[i].height < widget.minHeight ||
        widget.minWidth == 0 || widget.minHeight == 0)
      return false;
    if (widget.kind == Kind::Header && i != 0) return false;
    if (widget.kind == Kind::Plugin && widget.fallback != Kind::Empty &&
        widget.fallback != Kind::Scene)
      return false;
  }
  uint16_t coveredRows = 0;
  for (int i = 0; i < plan.underlayCount; ++i) {
    const Underlay& layer = plan.underlays[i];
    Rect unionRect;
    if (!layer.scene || !home_grid::spanRows(result, layer.fromRow, layer.throughRow, unionRect))
      return false;
    for (int row = layer.fromRow; row <= layer.throughRow; ++row) {
      if (coveredRows & (1u << row) || def.rows[row].columnCount != 1 ||
          plan.widgets[def.rows[row].firstColumn].kind == Kind::Shortcuts)
        return false;
      coveredRows |= static_cast<uint16_t>(1u << row);
    }
  }
  return true;
}

namespace detail {
inline void begin(const PaintHooks& hooks, Rect clip) { hooks.pushClip(hooks.user, clip); }
inline void end(const PaintHooks& hooks) { hooks.popClip(hooks.user); }

inline bool paintBase(const Plan& plan, const home_grid::Result& result, const PaintHooks& hooks) {
  const bool black = plan.background.mode == BackgroundMode::Black;
  hooks.fill(hooks.user, result.canvas, black);
  if (plan.background.mode != BackgroundMode::Bitmap) return true;
  // Caller supplies a prevalidated declared CPB resource. The native adapter
  // uses a fixed 128-byte scanline, never a second frame-sized bitmap.
  if (plan.background.bitmap && hooks.bitmap &&
      hooks.bitmap(hooks.user, plan.background, result.canvas))
    return true;
  // Even a decoder failing halfway must not leave a partial wallpaper.
  hooks.fill(hooks.user, result.canvas, false);
  return false;
}

inline void paintUnderlays(const Plan& plan, const home_grid::Result& result,
                           const PaintHooks& hooks, Rect clippedTo, uint8_t& failedCount) {
  for (int j = 0; j < plan.underlayCount; ++j) {
    const Underlay& item = plan.underlays[j];
    Rect area;
    if (!home_grid::spanRows(result, item.fromRow, item.throughRow, area)) continue;
    const Rect clipped = intersect(area, clippedTo);
    if (clipped.width <= 0 || clipped.height <= 0) continue;
    begin(hooks, clipped);
    if (!hooks.scene || !hooks.scene(hooks.user, item.scene, area)) {
      // Damaged scene artwork may have painted part of the surface. Replay
      // the underlying wallpaper inside the current clip (not a white wipe),
      // preserving the backdrop and leaving native widgets interactive.
      (void)paintBase(plan, result, hooks);
      ++failedCount;
    }
    end(hooks);
  }
}

inline void redrawUnderlying(const Plan& plan, const home_grid::Result& result,
                             const PaintHooks& hooks, Rect area) {
  begin(hooks, area);
  (void)paintBase(plan, result, hooks);
  end(hooks);
  uint8_t ignoredUnderlayFailures = 0;
  paintUnderlays(plan, result, hooks, area, ignoredUnderlayFailures);
}

inline bool drawWidget(Kind kind, uint16_t resource, Rect area, const PaintHooks& hooks) {
  if (kind == Kind::Empty) return true;
  if (kind == Kind::Scene) return hooks.scene && hooks.scene(hooks.user, resource, area);
  if (kind == Kind::Header || kind == Kind::Book || kind == Kind::Shortcuts ||
      kind == Kind::Clock || kind == Kind::Metric)
    return hooks.native && hooks.native(hooks.user, kind, resource, area);
  return false;
}
}  // namespace detail

// A dirty rectangle may rerender one card without erasing its transparent
// background: replay wallpaper/underlays clipped to dirty, then paint widgets.
// Native status and physical hints remain HomeActivity's final pass (Stage 4).
inline PaintReport paint(const Plan& plan, const home_grid::Definition& def,
                        const home_grid::Result& result, const PaintHooks& hooks,
                        Rect dirty = {}) {
  PaintReport report;
  if (!validPlan(plan, def, result)) return report;
  if (!hooks.pushClip || !hooks.popClip || !hooks.fill) {
    report.status = PaintStatus::InvalidHooks;
    return report;
  }
  if (dirty.width == 0 && dirty.height == 0) dirty = result.canvas;
  const Rect clip = intersect(result.canvas, dirty);
  report.status = PaintStatus::Ok;
  if (clip.width <= 0 || clip.height <= 0) return report;

  detail::begin(hooks, clip);
  report.backgroundFallback = !detail::paintBase(plan, result, hooks);
  detail::end(hooks);
  detail::paintUnderlays(plan, result, hooks, clip, report.failedUnderlays);

  for (int i = 0; i < result.widgetCount; ++i) {
    const Widget& widget = plan.widgets[i];
    const Rect area = result.widgets[i];
    const Rect clipped = intersect(area, clip);
    if (clipped.width <= 0 || clipped.height <= 0) continue;

    Kind chosen = widget.kind;
    uint16_t resource = widget.resource;
    if (widget.kind == Kind::Plugin) {
      const bool ready = widget.resource && hooks.providerAvailable &&
                         hooks.providerAvailable(hooks.user, widget.resource);
      if (!ready) {
        ++report.missingProviders;
        chosen = widget.fallback;
        resource = widget.fallbackResource;
      }
    }
    detail::begin(hooks, clipped);
    if (widget.panel == Panel::White || widget.panel == Panel::Black)
      hooks.fill(hooks.user, area, widget.panel == Panel::Black);
    const bool drawn = chosen == Kind::Plugin
                           ? (hooks.native && hooks.native(hooks.user, Kind::Plugin, resource, area))
                           : detail::drawWidget(chosen, resource, area, hooks);
    detail::end(hooks);
    if (drawn) continue;
    ++report.failedWidgets;
    // Restore full visual beneath a partially painted, failed widget without
    // any temporary framebuffer or SD scripts. Then keep panel/fallback safe.
    detail::redrawUnderlying(plan, result, hooks, clipped);
    detail::begin(hooks, clipped);
    if (widget.panel == Panel::White || widget.panel == Panel::Black)
      hooks.fill(hooks.user, area, widget.panel == Panel::Black);
    detail::end(hooks);
  }
  return report;
}

inline int hitTest(const Plan& plan, const home_grid::Result& result,
                   int x, int y, const PaintHooks& hooks) {
  if (result.widgetCount != plan.widgetCount) return -1;
  for (int i = 0; i < result.widgetCount; ++i) {
    const Widget& widget = plan.widgets[i];
    if (!focusable(widget.kind)) continue;
    if (widget.kind == Kind::Plugin && (!hooks.providerAvailable || !widget.resource ||
        !hooks.providerAvailable(hooks.user, widget.resource))) continue;
    const Rect& area = result.widgets[i];
    if (x >= area.x && y >= area.y && x < area.x + area.width && y < area.y + area.height)
      return i;
  }
  return -1;
}

inline int nextFocusable(const Plan& plan, int current, bool forward, const PaintHooks& hooks) {
  if (plan.widgetCount == 0 || plan.widgetCount > home_grid::kMaxWidgets) return -1;
  for (int n = 0; n < plan.widgetCount; ++n) {
    const int index = (current < 0 || current >= plan.widgetCount)
                          ? (forward ? n : plan.widgetCount - n - 1)
                          : (current + (forward ? n + 1 : plan.widgetCount - n - 1)) % plan.widgetCount;
    const Widget& widget = plan.widgets[index];
    if (!focusable(widget.kind)) continue;
    if (widget.kind == Kind::Plugin && (!hooks.providerAvailable || !widget.resource ||
        !hooks.providerAvailable(hooks.user, widget.resource))) continue;
    return index;
  }
  return -1;
}
}  // namespace home_widgets
