#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>
#include <string>

#include "ThemeScene.h"
#include "components/UITheme.h"
#include "components/icons/blocks.h"
#include "components/icons/book.h"
#include "components/icons/bookmark.h"
#include "components/icons/folder.h"
#include "components/icons/hotspot.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

namespace {
void chamfer(const GfxRenderer& r, int x, int y, int w, int h, int cut, bool black) {
  if (w <= 0 || h <= 0) return;
  cut = std::max(0, std::min({cut, w / 3, h / 3}));
  const int xs[6] = {x + cut, x + w - 1, x + w - 1, x + w - 1 - cut, x, x};
  const int ys[6] = {y, y, y + h - 1 - cut, y + h - 1, y + h - 1, y + cut};
  r.fillPolygon(xs, ys, 6, black);
}

const uint8_t* menuIcon(UIIcon icon) {
  switch (icon) {
    case Folder:
      return FolderIcon;
    case Book:
      return BookIcon;
    case Recent:
      return RecentIcon;
    case Settings:
      return Settings2Icon;
    case Transfer:
      return TransferIcon;
    case Library:
      return LibraryIcon;
    case Plugins:
    case Blocks:
      return BlocksIcon;
    case Wifi:
      return WifiIcon;
    case Hotspot:
      return HotspotIcon;
    case Bookmark:
      return BookmarkIcon;
    default:
      return nullptr;
  }
}

void coloredIcon(const GfxRenderer& r, const uint8_t* image, int x, int y, bool black) {
  if (!image) return;
  constexpr int SIZE = 32;
  constexpr int ROW_BYTES = SIZE / 8;
  for (int row = 0; row < SIZE; ++row) {
    for (int col = 0; col < SIZE; ++col) {
      const uint8_t cell = image[row * ROW_BYTES + (col >> 3)];
      if (!(cell & (0x80u >> (col & 7)))) r.drawPixel(x + SIZE - 1 - row, y + col, black);
    }
  }
}

std::string uppercase(std::string input) {
  for (char& c : input)
    if (c >= 'a' && c <= 'z') c = c - ('a' - 'A');
  return input;
}
}  // namespace

bool ThemeScene::drawMenu(const GfxRenderer& renderer, const Rect area, int itemCount, int selected,
                          const std::function<std::string(int)>& label, const std::function<UIIcon(int)>& icon) const {
  if (rowCut <= 0 || itemCount < 0 || area.width < 80 || area.height < 30) return false;
  const int rowHeight = std::max(32, UITheme::getInstance().getMetrics().menuRowHeight);
  const int pageItems = std::max(1, area.height / rowHeight);
  const int pageStart = std::max(0, selected) / pageItems * pageItems;
  const int x = area.x + 12;
  const int width = area.width - 24;
  for (int i = pageStart; i < itemCount && i < pageStart + pageItems; ++i) {
    const int y = area.y + (i - pageStart) * rowHeight;
    const bool active = i == selected;
    if (active)
      chamfer(renderer, x, y, width, rowHeight - 2, rowCut, true);
    else if (rowSeparators && i + 1 != selected && i + 1 < itemCount)
      renderer.drawLine(x + 12, y + rowHeight - 1, x + width - 1, y + rowHeight - 1, true);

    int tx = x + 14;
    const uint8_t* picture = icon ? menuIcon(icon(i)) : nullptr;
    if (picture) {
      coloredIcon(renderer, picture, tx, y + (rowHeight - 34) / 2, !active);
      tx += 46;
    }
    const std::string text = uppercase(label(i));
    const auto fitted = renderer.truncatedText(UI_12_FONT_ID, text.c_str(), std::max(1, x + width - tx - 42));
    const int lineY = y + (rowHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, tx, lineY, fitted.c_str(), !active);
    const int cx = x + width - 18;
    const int cy = y + (rowHeight - 2) / 2;
    renderer.drawLine(cx - 4, cy - 6, cx + 2, cy, 2, !active);
    renderer.drawLine(cx + 2, cy, cx - 4, cy + 6, 2, !active);
  }
  return true;
}

bool ThemeScene::drawHints(GfxRenderer& renderer, const char* first, const char* second, const char* third,
                           const char* fourth) const {
  if (hintCut <= 0 || gpio.hasTouch()) return false;
  const auto originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  constexpr int BUTTON_WIDTH = 104;
  constexpr int BUTTON_HEIGHT = 32;
  const int narrow[4] = {25, 130, 245, 350};
  const int wide[4] = {38, 154, 268, 384};
  const int* positions = width >= 528 ? wide : narrow;
  const char* captions[4] = {first, second, third, fourth};
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();
  for (int i = 0; i < 4; ++i) {
    if (!captions[i] || !*captions[i]) continue;
    const int x = positions[i];
    const int y = height - BUTTON_HEIGHT - 5;
    if (grayscale) {
      renderer.fillRect(x, y, BUTTON_WIDTH, BUTTON_HEIGHT, true);
      continue;
    }
    chamfer(renderer, x, y, BUTTON_WIDTH, BUTTON_HEIGHT, hintCut, true);
    const auto text = uppercase(renderer.truncatedText(SMALL_FONT_ID, captions[i], BUTTON_WIDTH - 14));
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, text.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (BUTTON_WIDTH - tw) / 2,
                      y + (BUTTON_HEIGHT - renderer.getLineHeight(SMALL_FONT_ID)) / 2, text.c_str(), false);
  }
  renderer.setOrientation(originalOrientation);
  return true;
}
