#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

class GfxRenderer {
 public:
  static constexpr int Width = 32;
  static constexpr int Height = 24;

  GfxRenderer() { reset(); }
  void reset() {
    for (auto& pixel : pixels) pixel = 0;
    clip = {0, 0, Width, Height};
  }
  std::array<int, 4> getClipRect() const { return clip; }
  void setClipRect(int x, int y, int width, int height) const {
    clip = {x, y, width, height};
  }
  int getScreenWidth() const { return Width; }
  int getScreenHeight() const { return Height; }
  void drawPixel(int x, int y, bool black) const {
    if (x >= 0 && x < Width && y >= 0 && y < Height &&
        x >= clip[0] && y >= clip[1] &&
        x < clip[0] + clip[2] && y < clip[1] + clip[3])
      pixels[y * Width + x] = black ? 1 : 0;
  }
  void fillRect(int x, int y, int width, int height, bool black) const {
    for (int dy = y; dy < y + height; ++dy)
      for (int dx = x; dx < x + width; ++dx) drawPixel(dx, dy, black);
  }
  int pixel(int x, int y) const { return pixels[y * Width + x]; }
  int getTextWidth(int, const char* text) const {
    return text ? static_cast<int>(std::strlen(text)) * 2 : 0;
  }
  int getLineHeight(int) const { return fontHeight; }
  int fontHeight = 4;
  void drawText(int, int x, int y, const char* text, bool black = true) const {
    if (!text) return;
    lastInkBlack = black;
    ++textDrawCount;
    const size_t copied = std::min(std::strlen(text), sizeof(lastText) - 1);
    std::memcpy(lastText, text, copied);
    lastText[copied] = '\0';
    for (int i = 0; text[i] && i < 36; ++i)
      drawPixel(x + i * 2, y, black);
  }
  mutable int textDrawCount = 0;
  mutable bool lastInkBlack = true;
  mutable char lastText[64]{};

 private:
  mutable std::array<int, 4> clip{};
  mutable std::array<uint8_t, Width * Height> pixels{};
};
