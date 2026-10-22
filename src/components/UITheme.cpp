#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <memory>
#include <optional>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "RecentBooksStore.h"
#include "components/CoverGridHomeUi.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/ThemePackRegistry.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

// UI theme font ownership is separate from the reader's selected font family.
void UITheme::syncFonts() {
  // Fonts and glyph caches are used by the render task. Theme switching may
  // also happen from an onExit() that already owns the nonrecursive mutex.
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  extern GfxRenderer renderer;
  if (renderer.getFontMap().count(UI_10_FONT_ID) == 0) return;  // early boot
  themeFonts.deactivate(renderer);
#if defined(CROSSPOINT_THEME_FORCE_STOCK_UI_FONTS)
  // Optional build-time isolation switch for any board. Normal builds
  // exercise the shared theme-font path on every supported device.
  if (externalActive && !externalScene.fontFiles()[0].empty())
    LOG_INF("THEME", "Diagnostic: using stock UI fonts (theme font isolation)");
  return;
#endif
  if (externalActive && !externalScene.fontFiles()[0].empty() &&
      !themeFonts.activate(renderer, externalScene.fontFiles(), externalScene.usesUiFontFamilyVariants())) {
    LOG_ERR("THEME", "External font activation failed; retaining stock UI fonts");
  }
}

void UITheme::reload() {
  // Protect scene/metric replacement as well as font replacement from the
  // render task; onExit() can invoke reload while already owning this mutex.
  std::optional<RenderLock> lock;
  if (!RenderLock::heldByCurrentTask()) lock.emplace();
  if (SETTINGS.sdThemeId[0] != '\0') {
    ThemeMetrics candidate{};
    std::string scenePath;
    if (ThemePackRegistry::getInstance().loadMetrics(SETTINGS.sdThemeId, candidate, &scenePath)) {
      auto theme = makeUniqueNoThrow<BaseTheme>();
      ThemeScene scene;
      if (theme && scene.load(scenePath)) {
        currentTheme = std::move(theme);
        externalScene = std::move(scene);
        externalActive = true;
        externalMetrics = candidate;
        currentMetrics = &externalMetrics;
        metricsValid = false;
        syncFonts();
        LOG_INF("THEME", "Activated external theme: %s", SETTINGS.sdThemeId);
        return;
      }
      LOG_ERR("THEME", "Cannot load external scene or theme, using Classic");
    } else {
      LOG_ERR("THEME", "Invalid or missing external theme %s, using Classic", SETTINGS.sdThemeId);
    }
    setTheme(CrossPointSettings::CLASSIC);
    syncFonts();
    return;
  }
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
  syncFonts();
}

bool UITheme::supportsCoverGrid() { return HalMemory::getPsramHeap().totalBytes > 0; }

bool UITheme::hasCoverGridHome() { return SETTINGS.uiTheme == CrossPointSettings::COVER_GRID && supportsCoverGrid(); }

void UITheme::drawCoverGridHome(CoverGridHomeUi& home) { home.renderUi(); }

bool UITheme::renderExternalScene(ThemeScene::Region region, const GfxRenderer& renderer, Rect area, const char* title,
                                  const std::vector<RecentBook>* books, int selection, ThemeScene::Phase phase) const {
  return externalActive && externalScene.hasRegion(region) &&
         externalScene.draw(renderer, region, area, title, books, selection, phase);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  externalActive = false;
  externalScene = ThemeScene{};
  if (type == CrossPointSettings::COVER_GRID && !supportsCoverGrid()) type = CrossPointSettings::LYRA;

  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::COVER_GRID:
    case CrossPointSettings::UI_THEME::LYRA: {
      // The cover home owns its screen-lifetime UI state; other screens retain Lyra styling.
      auto theme = makeUniqueNoThrow<LyraTheme>();
      if (!theme) {
        LOG_ERR("UI", "OOM: Lyra theme");
        return;
      }
      currentTheme = std::move(theme);
      currentMetrics = &LyraMetrics::values;
      LOG_DBG("UI", "Using Lyra theme");
      break;
    }
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
  }
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  int visibleTop = 0;
  if (touch && externalActive) {
    // X4Pro's visible panel begins 9px below the framebuffer top. Reserve
    // the oriented top inset for standard navigation headers, including Home.
    extern GfxRenderer renderer;
    int right = 0, bottom = 0, left = 0;
    renderer.getOrientedViewableTRBL(&visibleTop, &right, &bottom, &left);
  }
  if (!metricsValid || touch != metricsForTouch || visibleTop != metricsVisibleTop) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
      // A custom Home header on touch boards must reach at least the same
      // bottom edge as standard headers (top inset + band); otherwise they sit
      // above the visible chrome, shrinking the usable tap/status band.
      if (externalActive) {
        adjustedMetrics.topPadding = std::max(adjustedMetrics.topPadding, visibleTop);
        adjustedMetrics.homeTopPadding =
            std::max(adjustedMetrics.homeTopPadding, adjustedMetrics.topPadding + adjustedMetrics.headerHeight);
      }
    }
    metricsForTouch = touch;
    metricsVisibleTop = visibleTop;
    metricsValid = true;
  }
  return adjustedMetrics;
}

// Full drawable content area: screen minus the board's viewable insets (bezel /
// rounded-corner clearance), oriented to the current rotation. The single place
// the inset math lives; every screen derives its content bounds from here (or
// from getScreenSafeArea, which builds on it).
Rect UITheme::getContentArea(const GfxRenderer& renderer) {
  int viTop = 0, viRight = 0, viBottom = 0, viLeft = 0;
  renderer.getOrientedViewableTRBL(&viTop, &viRight, &viBottom, &viLeft);
  return Rect{viLeft, viTop, renderer.getScreenWidth() - viLeft - viRight,
              renderer.getScreenHeight() - viTop - viBottom};
}

// Content area excluding the button hints.
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  Rect safeArea = getContentArea(renderer);
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasImageExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  return (sb.textLaneVisible(true) ? (metrics.statusBarVerticalMargin) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
