#include "components/home/HomeWidgetThemeAdapters.h"

#include <cassert>
#include <functional>
#include <string>
#include <vector>

using namespace home_grid;
using namespace home_widgets;

namespace {
int cpbDraws = 0;
bool cpbShouldFail = false;

Definition layout() {
  Definition d;
  d.rowCount = 4;
  d.columnCount = 4;
  for (int i = 0; i < 4; ++i) {
    d.rows[i].firstColumn = static_cast<uint8_t>(i);
    d.rows[i].columnCount = 1;
    d.rows[i].mode = RowMode::Fixed;
  }
  d.rows[0].minHeight = d.rows[0].maxHeight = 4;
  d.rows[1].minHeight = d.rows[1].maxHeight = 8;
  d.rows[2].minHeight = d.rows[2].maxHeight = 6;
  d.rows[3].minHeight = d.rows[3].maxHeight = 6;
  return d;
}

Plan widgetPlan() {
  Plan p;
  p.widgetCount = 4;
  p.widgets[0].kind = Kind::Header;
  p.widgets[0].panel = Panel::White;
  p.widgets[1].kind = Kind::Empty;
  p.widgets[2].kind = Kind::Book;
  p.widgets[3].kind = Kind::Shortcuts;
  p.widgets[3].panel = Panel::White;
  p.underlayCount = 1;
  p.underlays[0] = Underlay{1, 2, firmware::kLegacyHomeScene};
  p.background.mode = BackgroundMode::Bitmap;
  p.background.bitmap = firmware::kWallpaperBitmap;
  return p;
}

void testNativeHeaderBookMenuAndUnderlay() {
  GfxRenderer screen;
  UITheme theme;
  std::vector<RecentBook> books{{"Book A"}, {"Book B"}};
  bool rendered = false, stored = false, restored = false;
  int snapshots = 0;
  std::function<bool()> save = [&snapshots]() { ++snapshots; return true; };
  std::function<std::string(int)> label = [](int i) {
    return i == 0 ? std::string("Library") : std::string("Settings");
  };
  std::function<UIIcon(int)> icon = [](int) { return Library; };
  std::string wallpaper = "wallpaper.cpb";
  firmware::View view{theme, screen};
  view.recentBooks = &books;
  view.wallpaperPath = &wallpaper;
  view.headerTitle = "Home";
  view.selectorIndex = 0;
  view.currentBook = 1;
  view.externalBookScene = home_grid::Rect{0, 4, 32, 14};
  view.useExternalBookCard = true;
  view.coverRendered = &rendered;
  view.coverBufferStored = &stored;
  view.bufferRestored = &restored;
  view.storeCoverBuffer = &save;
  view.visibleMenuRows = 2;
  view.selectedMenuRow = 1;
  view.menuLabel = &label;
  view.menuIcon = &icon;

  const auto d = layout();
  Result out;
  assert(resolve(d, Screen{32, 24}, out) == Status::Ok);
  auto p = widgetPlan();
  auto surface = firmware::makeSurface(view);
  auto callbacks = home_widgets::native::hooks(surface);
  const auto result = paint(p, d, out, callbacks);
  assert(result.status == PaintStatus::Ok && !result.backgroundFallback);
  assert(result.failedWidgets == 0 && result.failedUnderlays == 0);
  assert(cpbDraws == 1 && theme.backgroundCalls == 1 && theme.foregroundCalls == 1);
  assert(theme.lastArea.y == 4 && theme.lastArea.height == 14);
  assert(theme.lastBookIndex == 1);
  assert(theme.theme.headerCalls == 1 && theme.theme.bookCalls == 0);
  assert(theme.theme.menuCalls == 1 && theme.theme.menuCount == 2);
  assert(theme.theme.menuSelected == 1 && theme.theme.lastMenuLabel == "Library");
  assert(theme.theme.lastMenuIcon == Library);
  assert(screen.pixel(3, 14) == 1); // Foreground anchored to Hero+Book union
  assert(screen.pixel(0, 20) == 0); // Menu's opaque panel
  assert(screen.getClipRect() == (std::array<int, 4>{0, 0, 32, 24}));
  assert(hitTest(p, out, 2, 7, callbacks) == -1); // Underlay never interactive
  assert(hitTest(p, out, 3, 14, callbacks) == 2);
}

void testNativeFallbackBookAndSnapshot() {
  GfxRenderer screen;
  UITheme theme;
  std::vector<RecentBook> books{{"Book A"}};
  bool rendered = false, stored = false, restored = true;
  int saves = 0;
  std::function<bool()> save = [&saves]() { ++saves; return true; };
  std::function<std::string(int)> label = [](int) { return "Settings"; };
  std::function<UIIcon(int)> icon = [](int) { return Folder; };

  firmware::View view{theme, screen};
  view.recentBooks = &books;
  view.coverRendered = &rendered;
  view.coverBufferStored = &stored;
  view.bufferRestored = &restored;
  view.storeCoverBuffer = &save;
  view.visibleMenuRows = 1;
  view.selectedMenuRow = 0;
  view.menuLabel = &label;
  view.menuIcon = &icon;

  const auto d = layout();
  Result out;
  assert(resolve(d, Screen{32, 24}, out) == Status::Ok);
  auto p = widgetPlan();
  p.underlayCount = 0;
  p.background.mode = BackgroundMode::White;
  auto surface = firmware::makeSurface(view);
  const auto result = paint(p, d, out, home_widgets::native::hooks(surface));
  assert(result.status == PaintStatus::Ok && result.failedWidgets == 0);
  assert(theme.backgroundCalls == 0 && theme.foregroundCalls == 0);
  assert(theme.theme.bookCalls == 1 && saves == 1 && rendered && stored && !restored);
  assert(screen.pixel(3, 13) == 1);
}

void testMenuOverflowIsClippedToNativeRowHeight() {
  GfxRenderer screen;
  UITheme theme;
  std::function<std::string(int)> label = [](int) { return "Settings"; };
  std::function<UIIcon(int)> icon = [](int) { return Folder; };
  firmware::View view{theme, screen};
  view.visibleMenuRows = 99;  // Corrupt/stale viewport request must not overdraw.
  view.selectedMenuRow = 98;
  view.menuLabel = &label;
  view.menuIcon = &icon;
  const auto d = layout();
  Result out;
  assert(resolve(d, Screen{32, 24}, out) == Status::Ok);
  auto p = widgetPlan();
  p.underlayCount = 0;
  p.background.mode = BackgroundMode::White;
  p.widgets[2].kind = Kind::Empty;
  auto surface = firmware::makeSurface(view);
  auto hooks = home_widgets::native::hooks(surface);
  const auto painted = paint(p, d, out, hooks);
  assert(painted.status == PaintStatus::Ok && painted.failedWidgets == 0);
  assert(theme.theme.menuCount == 3); // 6-pixel menu / 2-pixel native rows
  assert(theme.theme.menuSelected == -1);

  view.visibleMenuRows = 2;
  view.selectedMenuRow = 1;
  assert(paint(p, d, out, hooks).failedWidgets == 0);
  assert(theme.theme.menuCount == 2 && theme.theme.menuSelected == 1);

  // A menu region too short for one native row fails without painting over
  // adjacent widgets; Stage 4 must choose valid dimensions.
  auto tight = d;
  tight.rows[3].minHeight = tight.rows[3].maxHeight = 1;
  assert(resolve(tight, Screen{32, 24}, out) == Status::Ok);
  assert(paint(p, tight, out, hooks).failedWidgets == 1);
}

void testFailureModesAreSafeAndBoundsAreRetained() {
  GfxRenderer screen;
  UITheme theme;
  std::vector<RecentBook> books{{"B"}};
  firmware::View view{theme, screen};
  view.recentBooks = &books;
  view.useExternalBookCard = true;
  view.externalBookScene = home_grid::Rect{0, 4, 32, 14};
  view.visibleMenuRows = 2;

  const auto d = layout();
  Result out;
  assert(resolve(d, Screen{32, 24}, out) == Status::Ok);
  auto p = widgetPlan();
  p.background.mode = BackgroundMode::White;
  auto surface = firmware::makeSurface(view);
  auto callbacks = home_widgets::native::hooks(surface);
  auto r = paint(p, d, out, callbacks);
  assert(r.status == PaintStatus::Ok && r.failedWidgets == 1); // no menu callbacks
  assert(r.failedUnderlays == 0);
  assert(screen.getClipRect() == (std::array<int, 4>{0, 0, 32, 24}));
  assert(firmware::drawScene(&view, 99, home_grid::Rect{0, 4, 32, 8}) == false);
  assert(firmware::getBitmap(&view, 99) == nullptr);
  assert(firmware::getBitmap(&view, firmware::kWallpaperBitmap) == nullptr);

  theme.sceneAvailable = false;
  r = paint(p, d, out, callbacks);
  assert(r.failedUnderlays == 1 && r.failedWidgets >= 1);
  // SD art errors never let the compositor paint outside the geometry.
  assert(screen.getClipRect() == (std::array<int, 4>{0, 0, 32, 24}));
}

}  // namespace

namespace themebitmap {
bool draw(const GfxRenderer& renderer, const std::string& path, const Options& options) {
  ++cpbDraws;
  if (cpbShouldFail || path != "wallpaper.cpb") return false;
  renderer.fillRect(options.x, options.y, options.width, options.height, true);
  return true;
}
}  // namespace themebitmap

int main() {
  testNativeHeaderBookMenuAndUnderlay();
  testNativeFallbackBookAndSnapshot();
  testMenuOverflowIsClippedToNativeRowHeight();
  testFailureModesAreSafeAndBoundsAreRetained();
}
