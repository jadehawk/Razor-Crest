#pragma once

// Stage 3 Home Layout V1: native delegates to existing themed GUI methods.
// This is deliberately not activated by HomeActivity until the Stage 4
// geometry/input integration and SD capability negotiation are complete.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "components/UITheme.h"
#include "components/home/HomeWidgetNativePaint.h"
#include "components/home/HomeWidgetReadouts.h"

namespace home_widgets::firmware {

// Bounded transitional resources: only these already supported SD scene
// regions can be used until a validated named-scene registry is implemented.
// Never interpret resource handles as file paths or executable expressions.
constexpr uint16_t kLegacyHomeScene = 1;
constexpr uint16_t kWallpaperBitmap = 1;

inline ::Rect toThemeRect(home_grid::Rect r) {
  return ::Rect{r.x, r.y, r.width, r.height};
}

struct View {
  UITheme& theme;
  GfxRenderer& renderer;

  // Non-owning, only valid for the duration of one synchronous render.
  const std::vector<RecentBook>* recentBooks = nullptr;
  const std::string* wallpaperPath = nullptr;
  const char* headerTitle = nullptr;
  int selectorIndex = 0;
  size_t currentBook = 0;

  // The legacy external scene places its book card relative to the whole
  // Hero+Book rectangle, not relative to the new Book-only widget viewport.
  home_grid::Rect externalBookScene{};
  bool useExternalBookCard = false;

  bool* coverRendered = nullptr;
  bool* coverBufferStored = nullptr;
  bool* bufferRestored = nullptr;
  const std::function<bool()>* storeCoverBuffer = nullptr;

  int visibleMenuRows = 0;
  int selectedMenuRow = -1;
  const std::function<std::string(int)>* menuLabel = nullptr;
  const std::function<UIIcon(int)>* menuIcon = nullptr;

  void* providerContext = nullptr;
  bool (*providerReady)(void*, uint16_t) = nullptr;
  ReadoutSource readouts{};
};
static_assert(sizeof(View) <= 256, "Home delegate view exceeds bounded V1 size");

inline const std::string* getBitmap(void* owner, uint16_t resource) {
  const View& view = *static_cast<View*>(owner);
  return resource == kWallpaperBitmap ? view.wallpaperPath : nullptr;
}

inline bool drawScene(void* owner, uint16_t resource, home_grid::Rect area) {
  View& view = *static_cast<View*>(owner);
  if (resource != kLegacyHomeScene) return false;
  // ThemeScene::Phase::Background excludes the native BookCard operation;
  // Stage 4 foreground paints the book at the same anchored scene geometry.
  return view.theme.renderExternalScene(ThemeScene::Region::Home, view.renderer, toThemeRect(area),
                                        nullptr, view.recentBooks, view.selectorIndex,
                                        ThemeScene::Phase::Background, view.currentBook);
}

inline bool drawNative(void* owner, Kind kind, uint16_t resource, home_grid::Rect area, Panel panel) {
  View& view = *static_cast<View*>(owner);
  const ::Rect nativeArea = toThemeRect(area);
  switch (kind) {
    case Kind::Header:
      view.theme.getTheme().drawHeader(view.renderer, nativeArea, view.headerTitle, nullptr, false);
      return true;
    case Kind::Book:
      if (view.useExternalBookCard) {
        if (!view.recentBooks || view.externalBookScene.width <= 0 || view.externalBookScene.height <= 0)
          return false;
        // The compositor already clipped to the Book widget rectangle. The
        // *drawn* scene origin remains the original Hero+Book union; without
        // this, selected-card outlines move when row weights change.
        return view.theme.renderExternalScene(ThemeScene::Region::Home, view.renderer,
                                              toThemeRect(view.externalBookScene), nullptr,
                                              view.recentBooks, view.selectorIndex,
                                              ThemeScene::Phase::Foreground, view.currentBook);
      }
      if (!view.recentBooks || !view.coverRendered || !view.coverBufferStored ||
          !view.bufferRestored || !view.storeCoverBuffer)
        return false;
      view.theme.getTheme().drawRecentBookCover(view.renderer, nativeArea, *view.recentBooks,
                                                view.selectorIndex, *view.coverRendered,
                                                *view.coverBufferStored, *view.bufferRestored,
                                                *view.storeCoverBuffer);
      return true;
    case Kind::Shortcuts: {
      if (view.visibleMenuRows < 0 || !view.menuLabel || !view.menuIcon) return false;
      const int rowHeight = view.theme.getTheme().getMenuRowHeight(view.renderer);
      if (rowHeight <= 0 || (view.visibleMenuRows && nativeArea.height < rowHeight)) return false;
      const int rows = std::min(view.visibleMenuRows, nativeArea.height / rowHeight);
      const int selected = view.selectedMenuRow >= 0 && view.selectedMenuRow < rows
                               ? view.selectedMenuRow : -1;
      view.theme.getTheme().drawButtonMenu(view.renderer, nativeArea, rows,
                                           selected, *view.menuLabel, *view.menuIcon);
      return true;
    }
    case Kind::Clock:
    case Kind::Metric:
      return paintReadout(view.renderer, kind, resource, area, panel, view.readouts, UI_12_FONT_ID);
    case Kind::Plugin:
    case Kind::Scene:
    case Kind::Empty:
      return false;
  }
  return false;
}

inline bool isProviderReady(void* owner, uint16_t resource) {
  const View& view = *static_cast<View*>(owner);
  return view.providerReady && view.providerReady(view.providerContext, resource);
}

inline home_widgets::native::Surface makeSurface(View& view) {
  home_widgets::native::Surface surface{view.renderer};
  surface.owner = &view;
  surface.resolveBitmap = getBitmap;
  surface.drawScene = drawScene;
  surface.drawNative = drawNative;
  surface.isProviderReady = isProviderReady;
  return surface;
}
}  // namespace home_widgets::firmware
