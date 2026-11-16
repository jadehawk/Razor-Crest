#include "components/home/HomeGridActivation.h"

#include <cassert>

using namespace home_grid;
using namespace home_grid::activation;
using namespace home_widgets;

namespace {
Definition layout() {
  Definition d;
  d.rowCount = 3;
  d.columnCount = 3;
  for (int i = 0; i < 3; ++i) {
    d.rows[i].firstColumn = static_cast<uint8_t>(i);
    d.rows[i].columnCount = 1;
    d.rows[i].weight = 1;
  }
  d.rows[0].mode = RowMode::Fixed;
  d.rows[0].minHeight = d.rows[0].maxHeight = 56;
  d.rows[1].minHeight = 190;
  d.rows[2].minHeight = 190;
  return d;
}

Plan plan() {
  Plan p;
  p.widgetCount = 3;
  p.widgets[0].kind = Kind::Header;
  p.widgets[1].kind = Kind::Book;
  p.widgets[2].kind = Kind::Shortcuts;
  return p;
}

Policy enabled() {
  return Policy{true, true, true, false};
}

void testActivationAndDeviceSafeArea() {
  const Definition def = layout();
  const Plan widgets = plan();
  Active active;
  assert(prepare(def, widgets, Screen{528, 792, 14, 14, 0, 48},
                 enabled(), active) == home_grid::activation::Status::Ready);
  assert(active.enabled);
  assert(active.rectangles.widgetCount == 3);
  assert(active.rectangles.canvas.width == 500);
  assert(active.rectangles.canvas.height == 744);
  assert(active.rectangles.widgets[2].y + active.rectangles.widgets[2].height <= 744);

  // A touch device has a different protected bottom edge; never infer it.
  assert(prepare(def, widgets, Screen{480, 800, 14, 14, 0, 8},
                 enabled(), active) == home_grid::activation::Status::Ready);
  assert(active.rectangles.canvas.height == 792);
  assert(active.rectangles.canvas.width == 452);
}

void testFallbackWithoutThemeOrCapabilities() {
  const Definition def = layout();
  const Plan widgets = plan();
  Active active;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) == home_grid::activation::Status::Ready);
  assert(active.enabled);

  Policy unsupported = enabled();
  unsupported.hasLayout = false;
  assert(prepare(def, widgets, Screen{528, 792}, unsupported, active) == home_grid::activation::Status::NoLayout);
  assert(!active.enabled && active.rectangles.widgetCount == 0);

  unsupported = enabled();
  unsupported.supportsLayout = false;
  assert(prepare(def, widgets, Screen{528, 792}, unsupported, active) ==
         home_grid::activation::Status::UnsupportedCapabilities);
  assert(!active.enabled);
  unsupported = enabled();
  unsupported.supportsWidgets = false;
  assert(prepare(def, widgets, Screen{528, 792}, unsupported, active) ==
         home_grid::activation::Status::UnsupportedCapabilities);
  assert(!active.enabled);

  auto bitmap = widgets;
  bitmap.background.mode = BackgroundMode::Bitmap;
  bitmap.background.bitmap = 1;
  assert(prepare(def, bitmap, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::UnsupportedBackground);
  unsupported = enabled();
  unsupported.supportsBackground = true;
  assert(prepare(def, bitmap, Screen{528, 792}, unsupported, active) == home_grid::activation::Status::Ready);
  assert(active.enabled);
  bitmap.background.bitmap = 0;
  assert(prepare(def, bitmap, Screen{528, 792}, unsupported, active) ==
         home_grid::activation::Status::UnsupportedBackground);
  assert(!active.enabled);
}

void testInvalidWidgetAndGeometryFallBack() {
  auto def = layout();
  auto widgets = plan();
  Active active;

  def.rows[1].minHeight = 700;
  assert(prepare(def, widgets, Screen{528, 792, 14, 14, 0, 48},
                 enabled(), active) == home_grid::activation::Status::InvalidGeometry);
  assert(!active.enabled);
  def = layout();
  def.rows[0].minHeight = def.rows[0].maxHeight = 20;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::InvalidGeometry);
  assert(!active.enabled);
  def = layout();

  widgets.widgets[2].kind = Kind::Empty;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::MissingShortcuts);
  assert(!active.enabled);
  widgets = plan();
  widgets.widgets[1].kind = Kind::Shortcuts;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::MissingShortcuts);
  widgets = plan();
  widgets.widgets[0].kind = Kind::Empty;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::InvalidWidgets);

  widgets = plan();
  widgets.widgets[1].kind = Kind::Scene;
  widgets.widgets[1].resource = 0;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::InvalidWidgets);

  widgets = plan();
  widgets.widgets[1].kind = Kind::Plugin;
  widgets.widgets[1].resource = 5;
  widgets.widgets[1].fallback = Kind::Scene;
  widgets.widgets[1].fallbackResource = 0;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::InvalidWidgets);

  widgets = plan();
  widgets.widgets[1].minWidth = 1000;
  assert(prepare(def, widgets, Screen{528, 792}, enabled(), active) ==
         home_grid::activation::Status::InvalidWidgets);
  assert(!active.enabled);
}
}  // namespace

int main() {
  testActivationAndDeviceSafeArea();
  testFallbackWithoutThemeOrCapabilities();
  testInvalidWidgetAndGeometryFallBack();
}
