#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;

namespace themebitmap {
enum class Fit : uint8_t { Native, Contain, Cover };

struct Options {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  Fit fit = Fit::Cover;
  bool inkOnly = false;
  bool inkBlack = true;
};

// Read-only integrity check, including dimensions, row padding, and CRC32.
bool validate(const std::string& path);

// Checks the complete file and its CRC before painting any pixels.
// Works with the existing display buffer and a fixed 128-byte scanline.
bool draw(const GfxRenderer& renderer, const std::string& path, const Options& options);
}  // namespace themebitmap
