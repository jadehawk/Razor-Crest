#pragma once

#include <EpdFontFamily.h>
#include <SdCardFont.h>

#include <array>
#include <memory>
#include <optional>
#include <string>

class GfxRenderer;

// Theme-only UI families. Uses CrossPoint's existing v4 SD font loader;
// reader font selection, caches, and fallback registrations remain unchanged.
class ThemeFonts {
 public:
  static constexpr int DISPLAY_ID = 0x54484d44;  // "THMD", reserved for theme display
  void deactivate(GfxRenderer& renderer);
  bool activate(GfxRenderer& renderer, const std::array<std::string, 4>& files);
  bool active() const { return active_; }

 private:
  std::array<std::unique_ptr<SdCardFont>, 4> fonts_{};
  std::array<std::optional<EpdFontFamily>, 3> stock_{};
  bool active_ = false;
};
