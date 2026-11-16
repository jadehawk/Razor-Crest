#pragma once

// Host-only mirror of the UITheme API used by HomeWidgetThemeAdapters.
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <GfxRenderer.h>

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct RecentBook {
  std::string title;
};

enum UIIcon { Folder, Library, Book };
constexpr int UI_12_FONT_ID = 12;

struct ThemeScene {
  enum class Region : unsigned char { Header, Home, Boot };
  enum class Phase : unsigned char { Full, Background, Foreground };
};

class UITheme {
 public:
  struct FakeTheme {
    mutable int headerCalls = 0;
    mutable int bookCalls = 0;
    mutable int menuCalls = 0;
    mutable int menuCount = 0;
    mutable int menuSelected = -1;
    mutable std::string lastMenuLabel;
    mutable UIIcon lastMenuIcon = Folder;

    void drawHeader(const GfxRenderer& renderer, Rect rect, const char*, const char*, bool) const {
      ++headerCalls;
      renderer.drawPixel(rect.x + 1, rect.y + 1, true);
    }

    void drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                             const std::vector<RecentBook>& books, int,
                             bool& rendered, bool& stored, bool& restored,
                             std::function<bool()> save) const {
      ++bookCalls;
      if (!books.empty()) renderer.drawPixel(rect.x + 3, rect.y + 1, true);
      rendered = true;
      stored = save();
      restored = false;
    }

    int getMenuRowHeight(const GfxRenderer&) const { return 2; }

    void drawButtonMenu(GfxRenderer& renderer, Rect rect, int count, int selected,
                        const std::function<std::string(int)>& labels,
                        const std::function<UIIcon(int)>& icons) const {
      ++menuCalls;
      menuCount = count;
      menuSelected = selected;
      if (count > 0) {
        lastMenuLabel = labels(0);
        lastMenuIcon = icons(0);
      }
      renderer.drawPixel(rect.x + 1, rect.y + 1, true);
    }
  };

  mutable FakeTheme theme;
  mutable int backgroundCalls = 0;
  mutable int namedSceneCalls = 0;
  mutable uint16_t lastNamedHandle = 0;
  mutable int foregroundCalls = 0;
  mutable Rect lastArea{};
  mutable size_t lastBookIndex = 999;
  bool sceneAvailable = true;

  const FakeTheme& getTheme() const { return theme; }

  uint8_t homeSceneHandle(const char* name) const {
    if (!name) return 0;
    if (std::strcmp(name, "halo.hero") == 0) return 2;
    if (std::strcmp(name, "snoopy.footer") == 0) return 3;
    return 0;
  }

  bool renderExternalHomeScene(uint16_t handle, const GfxRenderer& renderer,
                               Rect area, const char* title,
                               const std::vector<RecentBook>* books, int selector,
                               ThemeScene::Phase phase, size_t bookIndex) const {
    if (handle == 1)
      return renderExternalScene(ThemeScene::Region::Home, renderer, area,
                                 title, books, selector, phase, bookIndex);
    if (!sceneAvailable || handle < 2 || handle > 3) return false;
    ++namedSceneCalls;
    lastNamedHandle = handle;
    renderer.drawPixel(area.x + 1, area.y + 1, true);
    return true;
  }

  bool renderExternalScene(ThemeScene::Region region, const GfxRenderer& renderer,
                           Rect area, const char*, const std::vector<RecentBook>*,
                           int, ThemeScene::Phase phase, size_t bookIndex) const {
    if (!sceneAvailable || region != ThemeScene::Region::Home) return false;
    lastArea = area;
    lastBookIndex = bookIndex;
    if (phase == ThemeScene::Phase::Background) {
      ++backgroundCalls;
      renderer.fillRect(area.x, area.y, area.width, area.height, true);
    } else if (phase == ThemeScene::Phase::Foreground) {
      ++foregroundCalls;
      renderer.drawPixel(area.x + 3, area.y + 10, true);
    }
    return true;
  }
};
