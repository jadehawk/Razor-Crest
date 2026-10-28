#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "CpbBitmap.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;
struct RecentBook;

// Declarative, executable-code-free scenes, decoded once on theme activation.
class ThemeScene {
 public:
  enum class Region : uint8_t { Header, Home, Boot, Sleep, Hints, Screen, ScreenBackground, Usb };
  enum class Phase : uint8_t { Full, Background, Foreground };
  enum class HeaderBackStyle : uint8_t { Emblem, Arrow, Both };
  bool load(const std::string& themeJsonPath);
  bool draw(const GfxRenderer& renderer, Region region, Rect area, const char* screenTitle = nullptr,
            const std::vector<RecentBook>* recentBooks = nullptr, int selectorIndex = 0,
            Phase phase = Phase::Full) const;
  bool hasRegion(Region region) const;
  // Native full-size book-card scene (one visual slot regardless of recent count).
  bool hasNativeCoverCard() const;
  int bookCarouselRailWidth() const { return carouselRailWidth; }
  const std::string& usbGrayscalePath() const { return usbGrayFile; }
  bool selectSleepGrayscalePath(std::string& path) const;
  HeaderBackStyle headerBackStyle() const { return backStyle; }
  const std::array<std::string, 4>& fontFiles() const { return fonts; }
  // New ui-family variants retain independent raster sizes but warm on demand.
  bool usesUiFontFamilyVariants() const { return uiFamilyVariants_; }
  bool usesRowSeparators() const { return rowSeparators; }
  bool prefersTabbedSettings() const { return settingsNavigationTabs; }
  bool drawMenu(const GfxRenderer& renderer, Rect area, int itemCount, int selected,
                const std::function<std::string(int)>& label, const std::function<UIIcon(int)>& icon) const;
  bool drawHints(GfxRenderer& renderer, const char* first, const char* second, const char* third,
                 const char* fourth) const;

 private:
  enum class Kind : uint8_t { Fill, Rect, Bitmap, Text, Line, BookCard, Progress };
  struct Op {
    Kind kind;
    bool black = true;
    int anchorX = 0, anchorY = 0;
    int offsetX = 0, offsetY = 0;
    int maxWidth = 0, maxHeight = 0;
    int thickness = 1;
    int tracking = 0;
    bool plate = false;
    bool plateBlack = true;
    bool vertical = false;
    bool inkOnly = false;
    themebitmap::Fit fit = themebitmap::Fit::Contain;
    std::string data;
    std::string touchData;  // Independently authored art for touch panels.
    bool headerEmblem = false;
    std::string source;
    std::string font;
    bool sleepDirectory = false;
  };
  std::array<std::vector<Op>, 8> regions{};
  // Preserve a choice only across sleep renderings, not theme reloads.
  mutable int lastSleepIndex = -1;
  std::array<std::string, 4> fonts{};
  bool uiFamilyVariants_ = false;
  int cornerCut = 0;  // Text plates and legacy fallback.
  int cardCut = 0;
  int rowCut = 0;
  int hintCut = 0;
  int carouselRailWidth = 8;
  bool rowSeparators = false;
  bool settingsNavigationTabs = false;
  bool menuDoubleOutline = false;
  HeaderBackStyle backStyle = HeaderBackStyle::Emblem;
  std::string menuFooterFile;
  std::string menuFooterTouchFile;
  std::string screenTouchFile;
  std::string usbGrayFile;
  std::string sleepGrayDirectory;
  std::string sleepGrayId;
};
