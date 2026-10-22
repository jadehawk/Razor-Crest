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
  enum class Region : uint8_t { Header, Home, Boot, Sleep, Hints };
  enum class Phase : uint8_t { Full, Background, Foreground };
  bool load(const std::string& themeJsonPath);
  bool draw(const GfxRenderer& renderer, Region region, Rect area, const char* screenTitle = nullptr,
            const std::vector<RecentBook>* recentBooks = nullptr, int selectorIndex = 0,
            Phase phase = Phase::Full) const;
  bool hasRegion(Region region) const;
  const std::array<std::string, 4>& fontFiles() const { return fonts; }
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
    std::string source;
    std::string font;
    std::vector<std::string> gallery;
  };
  std::array<std::vector<Op>, 5> regions{};
  std::array<std::string, 4> fonts{};
  int cornerCut = 0;  // Text plates and legacy fallback.
  int cardCut = 0;
  int rowCut = 0;
  int hintCut = 0;
  bool rowSeparators = false;
  bool settingsNavigationTabs = false;
};
