#include "components/home/HomeWidgetNativePaint.h"

#include <array>
#include <cassert>
#include <string>

using namespace home_grid;
using namespace home_widgets;

namespace {
struct Context {
  GfxRenderer& renderer;
  std::string image = "valid";
  bool providerEnabled = false;
  bool failBook = false;
  bool failScene = false;
  int bitmapCount = 0;
  int sceneCount = 0;
  int headerCount = 0;
  int bookCount = 0;
  int menuCount = 0;
  int pluginCount = 0;
  themebitmap::Fit lastFit = themebitmap::Fit::Cover;
  bool lastInkOnly = false;
};
Context* current = nullptr;

const std::string* resolveBitmap(void* owner, uint16_t handle) {
  auto& c = *static_cast<Context*>(owner);
  return handle == 1 && c.image != "missing" ? &c.image : nullptr;
}

bool drawScene(void* owner, uint16_t resource, Rect area) {
  auto& c = *static_cast<Context*>(owner);
  ++c.sceneCount;
  if (!resource) return false;
  c.renderer.fillRect(area.x, area.y, area.width, area.height, true);
  return !c.failScene;
}

bool drawNative(void* owner, Kind kind, uint16_t, Rect area, Panel) {
  auto& c = *static_cast<Context*>(owner);
  if (kind == Kind::Header) {
    ++c.headerCount;
    c.renderer.drawPixel(area.x + 1, area.y + 1, true);
  } else if (kind == Kind::Book) {
    ++c.bookCount;
    c.renderer.fillRect(area.x, area.y, area.width, area.height, true);
    if (c.failBook) return false;
  } else if (kind == Kind::Shortcuts) {
    ++c.menuCount;
    c.renderer.drawPixel(area.x + 1, area.y + 1, true);
  } else if (kind == Kind::Plugin) {
    ++c.pluginCount;
    c.renderer.drawPixel(area.x + 1, area.y + 1, true);
  }
  return true;
}

bool providerAvailable(void* owner, uint16_t id) {
  const auto& c = *static_cast<Context*>(owner);
  return c.providerEnabled && id == 1;
}

Definition definition() {
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

Plan plan() {
  Plan p;
  p.widgetCount = 4;
  p.widgets[0].kind = Kind::Header;
  p.widgets[0].panel = Panel::White;
  p.widgets[1].kind = Kind::Empty;
  p.widgets[1].panel = Panel::Transparent;
  p.widgets[2].kind = Kind::Book;
  p.widgets[2].panel = Panel::Transparent;
  p.widgets[3].kind = Kind::Shortcuts;
  p.widgets[3].panel = Panel::White;
  p.background.mode = BackgroundMode::Bitmap;
  p.background.bitmap = 1;
  p.underlayCount = 1;
  p.underlays[0] = Underlay{1, 2, 1};
  return p;
}

home_widgets::native::Surface surface(GfxRenderer& renderer, Context& ctx) {
  home_widgets::native::Surface s{renderer};
  s.owner = &ctx;
  s.resolveBitmap = resolveBitmap;
  s.drawScene = drawScene;
  s.drawNative = drawNative;
  s.isProviderReady = providerAvailable;
  return s;
}

void testLayering() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  auto s = surface(screen, ctx);
  auto h = home_widgets::native::hooks(s);
  const auto report = paint(p, d, rects, h);
  assert(report.status == PaintStatus::Ok);
  assert(!report.backgroundFallback && report.failedWidgets == 0);
  assert(ctx.bitmapCount == 1 && ctx.sceneCount == 1);
  assert(ctx.headerCount == 1 && ctx.bookCount == 1 && ctx.menuCount == 1);
  assert(screen.pixel(0, 0) == 0);   // opaque native header
  assert(screen.pixel(1, 1) == 1);
  assert(screen.pixel(0, 10) == 1);  // underlay in Hero
  assert(screen.pixel(0, 15) == 1);  // transparent Book shows underlay
  assert(screen.pixel(0, 20) == 0);  // opaque menu protects its background
  assert(screen.pixel(1, 19) == 1);
  assert(screen.getClipRect() == (std::array<int, 4>{0, 0, 32, 24}));
  assert(hitTest(p, rects, 1, 1, h) == -1);
  assert(hitTest(p, rects, 1, 10, h) == -1);
  assert(hitTest(p, rects, 1, 14, h) == 2);
  assert(hitTest(p, rects, 1, 19, h) == 3);
  assert(nextFocusable(p, -1, true, h) == 2);
  assert(nextFocusable(p, 2, true, h) == 3);
  assert(nextFocusable(p, 3, true, h) == 2);
  assert(nextFocusable(p, 2, false, h) == 3);
}

void testCorruptBackgroundAndPanelFallback() {
  GfxRenderer screen;
  Context ctx{screen};
  ctx.image = "corrupt";
  current = &ctx;
  auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  p.underlayCount = 0;
  p.widgets[2].kind = Kind::Empty;
  auto s = surface(screen, ctx);
  const auto report = paint(p, d, rects, home_widgets::native::hooks(s));
  assert(report.status == PaintStatus::Ok && report.backgroundFallback);
  assert(screen.pixel(10, 12) == 0); // partially drawn bad CPB rolled back
  assert(screen.pixel(10, 16) == 0);
  assert(screen.pixel(10, 21) == 0);
}

void testOptionalPluginAndFailureRecovery() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  p.widgets[2].kind = Kind::Plugin;
  p.widgets[2].resource = 1;
  p.widgets[2].fallback = Kind::Scene;
  p.widgets[2].fallbackResource = 2;
  auto s = surface(screen, ctx);
  auto h = home_widgets::native::hooks(s);
  auto result = paint(p, d, rects, h);
  assert(result.missingProviders == 1 && result.failedWidgets == 0);
  assert(ctx.pluginCount == 0);
  assert(hitTest(p, rects, 1, 14, h) == -1);
  assert(nextFocusable(p, -1, true, h) == 3);

  ctx.providerEnabled = true;
  ctx.sceneCount = 0;
  result = paint(p, d, rects, h);
  assert(result.missingProviders == 0 && ctx.pluginCount == 1);
  assert(ctx.sceneCount == 1); // underlay; plugin fallback not rendered
  assert(hitTest(p, rects, 1, 14, h) == 2);

  p.widgets[2].kind = Kind::Book;
  p.widgets[2].resource = 0;
  p.widgets[2].panel = Panel::White;
  ctx.failBook = true;
  result = paint(p, d, rects, h);
  assert(result.failedWidgets == 1);
  assert(screen.pixel(8, 15) == 0); // damaged Book wiped and panel restored
  assert(screen.pixel(8, 10) == 1); // Halo underlay remains outside Book
}

void testPartialRefreshAndClipRestore() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  auto s = surface(screen, ctx);
  auto h = home_widgets::native::hooks(s);
  assert(paint(p, d, rects, h).status == PaintStatus::Ok);
  ctx.bookCount = 0;
  ctx.menuCount = 0;
  screen.drawPixel(2, 7, false); // outside future dirty clip
  screen.drawPixel(2, 20, true);
  screen.setClipRect(1, 1, 30, 22);
  const auto report = paint(p, d, rects, h, Rect{1, 19, 12, 4});
  assert(report.status == PaintStatus::Ok && !report.backgroundFallback);
  assert(ctx.bookCount == 0 && ctx.menuCount == 1);
  assert(screen.pixel(2, 7) == 0);
  assert(screen.pixel(2, 20) == 0);
  assert(screen.getClipRect() == (std::array<int, 4>{1, 1, 30, 22}));
}

void testFailedSceneUnderlayFallback() {
  GfxRenderer screen;
  Context ctx{screen};
  ctx.failScene = true;  // Scene paints partially and then reports failure.
  current = &ctx;
  const auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  p.background.mode = BackgroundMode::White;
  auto s = surface(screen, ctx);
  auto report = paint(p, d, rects, home_widgets::native::hooks(s));
  assert(report.status == PaintStatus::Ok && report.failedUnderlays == 1);
  assert(screen.pixel(4, 10) == 0);  // Failed scene restored solid-white base.
  assert(screen.pixel(4, 15) == 1);  // Native Book remains usable and visible.
  assert(screen.pixel(4, 20) == 0);  // Menu retains its opaque panel.

  p.background.mode = BackgroundMode::Bitmap;
  report = paint(p, d, rects, home_widgets::native::hooks(s));
  assert(report.failedUnderlays == 1);
  assert(screen.pixel(4, 10) == 1);  // Failed scene preserves CPB wallpaper.
}

void testSolidBackgroundModesAndMissingAsset() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  const auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  p.underlayCount = 0;
  p.widgets[2].kind = Kind::Empty;
  auto s = surface(screen, ctx);
  const auto h = home_widgets::native::hooks(s);
  p.background.mode = BackgroundMode::Black;
  p.background.bitmap = 0;
  assert(paint(p, d, rects, h).status == PaintStatus::Ok);
  assert(screen.pixel(4, 10) == 1 && screen.pixel(4, 14) == 1);
  assert(screen.pixel(4, 20) == 0);  // menu remains opaque white

  p.background.mode = BackgroundMode::White;
  assert(paint(p, d, rects, h).status == PaintStatus::Ok);
  assert(screen.pixel(4, 10) == 0 && screen.pixel(4, 14) == 0);

  p.background.mode = BackgroundMode::Bitmap;
  p.background.bitmap = 1;
  ctx.image = "missing";
  const auto report = paint(p, d, rects, h);
  assert(report.status == PaintStatus::Ok && report.backgroundFallback);
  assert(screen.pixel(4, 10) == 0 && screen.pixel(4, 14) == 0);
}

void testRejectInvalidPlanAndHooks() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  const auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 24}, rects) == Status::Ok);
  auto p = plan();
  auto s = surface(screen, ctx);
  const auto h = home_widgets::native::hooks(s);
  p.widgets[0].kind = Kind::Empty;
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
  p = plan();
  p.widgets[2].minHeight = 7; // Book rectangle is only 6px
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
  p = plan();
  p.underlays[0] = Underlay{0, 2, 1}; // cannot cover firmware header
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
  p = plan();
  p.underlays[0] = Underlay{1, 3, 1}; // cannot cover shortcuts
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
  p = plan();
  p.widgets[3].kind = static_cast<Kind>(254); // untrusted JSON type
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
  p = plan();
  PaintHooks bad = h;
  bad.popClip = nullptr;
  assert(paint(p, d, rects, bad).status == PaintStatus::InvalidHooks);
  p.background.fit = static_cast<ImageFit>(99);
  assert(paint(p, d, rects, h).status == PaintStatus::InvalidPlan);
}

void testWallpaperOptionsAndSafeHints() {
  GfxRenderer screen;
  Context ctx{screen};
  current = &ctx;
  const auto d = definition();
  Result rects;
  assert(resolve(d, Screen{32, 28, 0, 0, 0, 4}, rects) == Status::Ok);
  auto p = plan();
  p.background.fit = ImageFit::Contain;
  p.background.inkOnly = true;
  auto s = surface(screen, ctx);
  const auto report = paint(p, d, rects, home_widgets::native::hooks(s));
  assert(report.status == PaintStatus::Ok);
  assert(ctx.lastFit == themebitmap::Fit::Contain && ctx.lastInkOnly);
  // The model has only 24 display scanlines; 4 reserved real-device hint
  // pixels lie below the Home canvas, so no hook sees those coordinates.
  assert(rects.canvas.height == 24);
}
} // namespace

// Link a fake device CPB function to the REAL native bitmap adapter. It uses
// the same signature/options as the bounded production themebitmap decoder.
namespace themebitmap {
bool draw(const GfxRenderer& renderer, const std::string& path, const Options& options) {
  assert(current);
  ++current->bitmapCount;
  current->lastFit = options.fit;
  current->lastInkOnly = options.inkOnly;
  renderer.fillRect(options.x, options.y, options.width, options.height, true);
  if (path == "corrupt") return false;
  return path == "valid";
}
}  // namespace themebitmap

int main() {
  testLayering();
  testCorruptBackgroundAndPanelFallback();
  testOptionalPluginAndFailureRecovery();
  testPartialRefreshAndClipRestore();
  testFailedSceneUnderlayFallback();
  testSolidBackgroundModesAndMissingAsset();
  testRejectInvalidPlanAndHooks();
  testWallpaperOptionsAndSafeHints();
}
