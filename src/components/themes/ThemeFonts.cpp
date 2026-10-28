#include "ThemeFonts.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include "fontIds.h"

namespace {
constexpr int UI_IDS[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID, ThemeFonts::DISPLAY_ID};
constexpr const char* WARM_TEXT =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 "
    ".,:;!?-/()[]%+@&#_'\"…";
}  // namespace

void ThemeFonts::deactivate(GfxRenderer& renderer) {
  if (!active_) return;
  // Unregister every alias before freeing the shared SdCardFont object.
  for (int i = 0; i < 4; ++i) renderer.removeFont(UI_IDS[i]);
  for (int i = 0; i < 3; ++i) {
    if (stock_[i]) renderer.insertFont(UI_IDS[i], *stock_[i]);
    stock_[i].reset();
  }
  for (auto& font : fonts_) font.reset();
  active_ = false;
  LOG_INF("THEME", "Restored built-in UI font families");
}

bool ThemeFonts::activate(GfxRenderer& renderer, const std::array<std::string, 4>& paths, const bool onDemandVariants) {
  // One 'ui' role shares one loaded font and glyph cache across every UI id.
  // Four explicit roles remain supported for older theme packs.
  const bool shared = !paths[0].empty() && paths[1].empty() && paths[2].empty() && paths[3].empty();
  if (paths[0].empty() || (!shared && (paths[1].empty() || paths[2].empty() || paths[3].empty()))) return false;

  std::array<std::unique_ptr<SdCardFont>, 4> loaded;
  const int sourceCount = shared ? 1 : 4;
  for (int i = 0; i < sourceCount; ++i) {
    loaded[i] = makeUniqueNoThrow<SdCardFont>();
    if (!loaded[i] || !loaded[i]->load(paths[i].c_str()) || !loaded[i]->hasStyle(0)) {
      LOG_ERR("THEME", "Cannot load theme font slot %d", i);
      return false;
    }
  }

  deactivate(renderer);
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
    SdCardFont* font = loaded[shared ? 0 : i].get();
    renderer.registerSdCardFont(UI_IDS[i], font);
    renderer.enableDirectSdUiText(UI_IDS[i]);
    const EpdFont* regular = font->getEpdFont(0);
    // Preserve the loaded .cpfont's actual line height with the new point-size API.
    const uint16_t pointSize = static_cast<uint16_t>(
        (static_cast<uint32_t>(regular->data->advanceY) * 72 + FONT_RASTER_DPI / 2) / FONT_RASTER_DPI);
    renderer.insertFont(UI_IDS[i], EpdFontFamily(pointSize ? pointSize : 1, regular, font->getEpdFont(1)));
  }
  fonts_ = std::move(loaded);
  active_ = true;

  // Warm each unique font once, not once per alias. Glyphs beyond this
  // common resident set still use SdCardFont's bounded on-demand cache.
  for (int i = 0; i < (onDemandVariants ? 0 : sourceCount); ++i) {
    const int missed = fonts_[i]->prewarm(WARM_TEXT, 0x03, false, false, true);
    if (missed < 0) LOG_ERR("THEME", "UI glyph prewarm failed for slot %d", i);
  }
  LOG_INF("THEME", shared             ? "Shared UI font active (one .cpfont, four UI roles)"
                   : onDemandVariants ? "UI family active (four sizes, on-demand glyph caches)"
                                      : "External SD UI fonts active (four roles)");
  return true;
}
