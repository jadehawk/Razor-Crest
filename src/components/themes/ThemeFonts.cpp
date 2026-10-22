#include "ThemeFonts.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include "fontIds.h"

namespace {
constexpr int UI_IDS[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID, ThemeFonts::DISPLAY_ID};
constexpr const char* WARM_TEXT =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 "
    ".,:;!?-/()[]%+@&#_'\"";
}  // namespace

void ThemeFonts::deactivate(GfxRenderer& renderer) {
  if (!active_) return;
  for (int i = 0; i < 4; ++i) renderer.removeFont(UI_IDS[i]);
  // Restore the actual families registered before the theme applied, not
  // guessed copies from a different firmware build.
  for (int i = 0; i < 3; ++i) {
    if (stock_[i]) renderer.insertFont(UI_IDS[i], *stock_[i]);
    stock_[i].reset();
  }
  for (auto& font : fonts_) font.reset();
  active_ = false;
  LOG_INF("THEME", "Restored built-in UI font families");
}

bool ThemeFonts::activate(GfxRenderer& renderer, const std::array<std::string, 4>& paths) {
  deactivate(renderer);
  if (paths[0].empty() || paths[1].empty() || paths[2].empty() || paths[3].empty()) {
    // No theme-specific UI font family: retain built-in fonts.
    return false;
  }
  // Stage all four sources before replacing a single built-in registration.
  std::array<std::unique_ptr<SdCardFont>, 4> loaded;
  for (int i = 0; i < 4; ++i) {
    loaded[i] = makeUniqueNoThrow<SdCardFont>();
    if (!loaded[i] || !loaded[i]->load(paths[i].c_str()) || !loaded[i]->hasStyle(0)) {
      LOG_ERR("THEME", "Cannot load theme font slot %d", i);
      return false;
    }
  }
  const auto& registered = renderer.getFontMap();
  for (int i = 0; i < 3; ++i) {
    const auto original = registered.find(UI_IDS[i]);
    if (original == registered.end()) {
      LOG_ERR("THEME", "Cannot activate theme fonts before UI fonts are ready");
      return false;
    }
    stock_[i] = original->second;
  }
  for (int i = 0; i < 4; ++i) {
    renderer.removeFont(UI_IDS[i]);
    renderer.registerSdCardFont(UI_IDS[i], loaded[i].get());
    renderer.insertFont(UI_IDS[i], EpdFontFamily(loaded[i]->getEpdFont(0), loaded[i]->getEpdFont(1)));
  }
  fonts_ = std::move(loaded);
  active_ = true;
  for (int i = 0; i < 4; ++i) {
    // Warm the common Latin/UI glyphs once; all other glyphs fault from SD
    // through the same bounded resident mini-cache used by the reader.
    fonts_[i]->prewarm(WARM_TEXT, 0x03, false, false, true);
  }
  LOG_INF("THEME", "External SD UI fonts active");
  return true;
}
