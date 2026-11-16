#pragma once

// Stage 4 preflight only: fail closed before activating a theme Home grid.
// No SD I/O, renderer calls, input state changes or framebuffer allocations.
// HomeActivity must retain its existing path unless Status::Ready is returned.
#include <cstdint>

#include "components/home/HomeWidgetEngine.h"

namespace home_grid::activation {
enum class Status : uint8_t {
  Ready,
  NoLayout,
  UnsupportedCapabilities,
  UnsupportedBackground,
  MissingShortcuts,
  InvalidGeometry,
  InvalidWidgets,
};

struct Policy {
  bool hasLayout = false;
  bool supportsLayout = false;
  bool supportsWidgets = false;
  bool supportsBackground = false;
};

struct Active {
  home_grid::Result rectangles{};
  bool enabled = false;
};
static_assert(sizeof(Active) <= 264, "Home activation state exceeds bounded V1 storage");

inline Status prepare(const home_grid::Definition& definition,
                      const home_widgets::Plan& plan,
                      const home_grid::Screen& screen, Policy policy,
                      Active& active) {
  active = Active{}; // Clear even after failed reactivation.
  if (!policy.hasLayout) return Status::NoLayout;
  if (!policy.supportsLayout || !policy.supportsWidgets)
    return Status::UnsupportedCapabilities;
  if (plan.background.mode == home_widgets::BackgroundMode::Bitmap &&
      (!policy.supportsBackground || !plan.background.bitmap))
    return Status::UnsupportedBackground;
  if (definition.rowCount == 0 || definition.rows[0].minHeight < 40)
    return Status::InvalidGeometry;

  // Exactly one native scrollable shortcuts widget maintains Settings access.
  // Its actual menu item count/scrolling still belongs to HomeActivity.
  int shortcuts = 0;
  if (plan.widgetCount == 0 || plan.widgetCount > home_grid::kMaxWidgets)
    return Status::InvalidWidgets;
  for (int index = 0; index < plan.widgetCount; ++index) {
    const auto& widget = plan.widgets[index];
    if (widget.kind == home_widgets::Kind::Shortcuts) ++shortcuts;
    if (widget.kind == home_widgets::Kind::Scene && !widget.resource)
      return Status::InvalidWidgets;
    if (widget.kind == home_widgets::Kind::Plugin &&
        widget.fallback == home_widgets::Kind::Scene && !widget.fallbackResource)
      return Status::InvalidWidgets;
  }
  if (shortcuts != 1) return Status::MissingShortcuts;

  home_grid::Result resolved;
  if (home_grid::resolve(definition, screen, resolved) != home_grid::Status::Ok)
    return Status::InvalidGeometry;
  if (!home_widgets::validPlan(plan, definition, resolved))
    return Status::InvalidWidgets;

  active.rectangles = resolved;
  active.enabled = true;
  return Status::Ready;
}
}  // namespace home_grid::activation
