#include "components/home/HomeWidgetThemeAdapters.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using home_grid::Definition;
using home_grid::Result;
using home_grid::RowMode;
using home_grid::Screen;
using home_grid::Status;
using home_widgets::BackgroundMode;
using home_widgets::Kind;
using home_widgets::PaintStatus;
using home_widgets::Panel;
using home_widgets::Plan;
using home_widgets::firmware::MetricField;

namespace {

struct Data {
  int clockReads = 0;
  int metricReads = 0;
  uint16_t lastMetric = 0;
  bool clockAvailable = true;
  bool metricAvailable = true;
  bool longMetric = false;
};

bool readClock(void* owner, char* output, size_t capacity) {
  auto& state = *static_cast<Data*>(owner);
  ++state.clockReads;
  if (!state.clockAvailable) return false;
  std::snprintf(output, capacity, "12:34");
  return true;
}

bool readMetric(void* owner, uint16_t field, char* label, size_t labelSize,
                char* value, size_t valueSize) {
  auto& state = *static_cast<Data*>(owner);
  ++state.metricReads;
  state.lastMetric = field;
  if (!state.metricAvailable) return false;
  std::snprintf(label, labelSize, "%s",
                state.longMetric ? "READING STATISTICS WITH A VERY LONG NAME" : "ACTIVE DAYS");
  std::snprintf(value, valueSize, "%s",
                state.longMetric ? "123456789012345678901234567890" : "18");
  return true;
}

Definition definition(int rows) {
  Definition d;
  d.rowCount = static_cast<uint8_t>(rows);
  d.columnCount = static_cast<uint8_t>(rows);
  for (int i = 0; i < rows; ++i) {
    d.rows[i].firstColumn = static_cast<uint8_t>(i);
    d.rows[i].columnCount = 1;
    d.rows[i].mode = RowMode::Fixed;
  }
  d.rows[0].minHeight = d.rows[0].maxHeight = 4;
  if (rows == 3) {
    d.rows[1].minHeight = d.rows[1].maxHeight = 10;
    d.rows[2].minHeight = d.rows[2].maxHeight = 10;
  } else {
    d.rows[1].minHeight = d.rows[1].maxHeight = 20;
  }
  return d;
}

void testClockMetricAndPanelInk() {
  GfxRenderer screen;
  UITheme theme;
  Data data{};
  home_widgets::firmware::View view{theme, screen};
  view.readouts = {&data, readClock, readMetric};

  const auto def = definition(3);
  Result result;
  assert(home_grid::resolve(def, Screen{32, 24}, result) == Status::Ok);

  Plan plan;
  plan.widgetCount = 3;
  plan.background.mode = BackgroundMode::White;
  plan.widgets[0].kind = Kind::Header;
  plan.widgets[0].panel = Panel::White;
  plan.widgets[1].kind = Kind::Clock;
  plan.widgets[1].panel = Panel::Black;
  plan.widgets[2].kind = Kind::Metric;
  plan.widgets[2].panel = Panel::White;
  plan.widgets[2].resource = static_cast<uint16_t>(MetricField::ActiveDays);
  auto surface = home_widgets::firmware::makeSurface(view);
  auto hooks = home_widgets::native::hooks(surface);

  auto report = home_widgets::paint(plan, def, result, hooks);
  assert(report.status == PaintStatus::Ok && report.failedWidgets == 0);
  assert(data.clockReads == 1 && data.metricReads == 1);
  assert(data.lastMetric == static_cast<uint16_t>(MetricField::ActiveDays));
  assert(std::strcmp(screen.lastText, "18") == 0);
  assert(screen.lastInkBlack);

  report = home_widgets::paint(plan, def, result, hooks, home_grid::Rect{0, 4, 32, 10});
  assert(report.status == PaintStatus::Ok && report.failedWidgets == 0);
  assert(std::strcmp(screen.lastText, "12:34") == 0);
  assert(!screen.lastInkBlack); // white ink on black panel

  data.clockAvailable = false;
  report = home_widgets::paint(plan, def, result, hooks, home_grid::Rect{0, 4, 32, 10});
  assert(report.failedWidgets == 0);
  assert(std::strcmp(screen.lastText, "--:--") == 0);

  data.metricAvailable = false;
  report = home_widgets::paint(plan, def, result, hooks, home_grid::Rect{0, 14, 32, 10});
  assert(report.failedWidgets == 0);
  assert(std::strcmp(screen.lastText, "--") == 0);

  plan.widgets[2].resource = 999; // No untrusted statistic ID lookups.
  report = home_widgets::paint(plan, def, result, hooks);
  assert(report.failedWidgets == 1);
  assert(screen.getClipRect() == (std::array<int,4>{0,0,32,24}));
}

void testUtf8TextIsTruncatedAtCodepointBoundary() {
  GfxRenderer screen;
  char label[40] = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
  home_widgets::firmware::drawFitted(
      screen, UI_12_FONT_ID, home_grid::Rect{0, 0, 14, 12},
      0, label, sizeof(label), true);
  // Simulated font width counts bytes; the clip budget fits only one
  // two-byte character, never a dangling continuation byte.
  assert(std::strlen(screen.lastText) == 2);
  assert(std::strcmp(screen.lastText, "\xc3\xa9") == 0);
}

void testLongLabelAndMissingProvider() {
  GfxRenderer screen;
  UITheme theme;
  Data data{};
  data.longMetric = true;
  home_widgets::firmware::View view{theme, screen};
  view.readouts = {&data, nullptr, readMetric};

  const auto def = definition(2);
  Result result;
  assert(home_grid::resolve(def, Screen{32, 24}, result) == Status::Ok);
  Plan plan;
  plan.widgetCount = 2;
  plan.background.mode = BackgroundMode::Black;
  plan.widgets[0].kind = Kind::Header;
  plan.widgets[0].panel = Panel::White;
  plan.widgets[1].kind = Kind::Metric;
  plan.widgets[1].panel = Panel::Black;
  plan.widgets[1].resource = static_cast<uint16_t>(MetricField::Sessions);

  auto surface = home_widgets::firmware::makeSurface(view);
  const auto hooks = home_widgets::native::hooks(surface);
  auto report = home_widgets::paint(plan, def, result, hooks);
  assert(report.failedWidgets == 0);
  assert(screen.textDrawCount == 2); // Label and value
  assert(screen.getTextWidth(UI_12_FONT_ID, screen.lastText) <= 24);
  assert(!screen.lastInkBlack);
  assert(data.lastMetric == static_cast<uint16_t>(MetricField::Sessions));

  view.readouts.metric = nullptr;
  report = home_widgets::paint(plan, def, result, hooks);
  assert(report.failedWidgets == 0); // Explicit missing-data placeholder
  assert(std::strcmp(screen.lastText, "--") == 0);

  plan.widgets[1].kind = Kind::Clock;
  report = home_widgets::paint(plan, def, result, hooks);
  assert(report.failedWidgets == 0);
  assert(std::strcmp(screen.lastText, "--:--") == 0);

  // Too-small widget fails closed instead of truncating to unusable dimensions.
  assert(!home_widgets::firmware::paintReadout(
      screen, Kind::Metric, static_cast<uint16_t>(MetricField::Sessions),
      home_grid::Rect{0, 0, 12, 8}, Panel::White, view.readouts, UI_12_FONT_ID));

  screen.fontHeight = 0; // Font not installed / temporarily unavailable
  report = home_widgets::paint(plan, def, result, hooks);
  assert(report.status == PaintStatus::Ok && report.failedWidgets == 1);
  assert(screen.getClipRect() == (std::array<int,4>{0,0,32,24}));
}

} // namespace

// Host fake for the existing firmware CPB bridge. This test doesn't load an
// image, but the unlinked symbol is still referenced by the adapter header.
namespace themebitmap {
bool draw(const GfxRenderer&, const std::string&, const Options&) { return true; }
}

int main() {
  testClockMetricAndPanelInk();
  testUtf8TextIsTruncatedAtCodepointBoundary();
  testLongLabelAndMissingProvider();
}
