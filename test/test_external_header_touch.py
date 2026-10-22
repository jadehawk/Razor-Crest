#!/usr/bin/env python3
"""Source guards for external SD theme headers and touch navigation."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
read = lambda filename: (ROOT / filename).read_text(encoding="utf-8")
base = read("src/components/themes/BaseTheme.cpp")
home = read("src/activities/home/HomeActivity.cpp")
panel = read("src/activities/util/FrontlightPanelActivity.cpp")
theme = read("src/components/UITheme.cpp")
catalog = read("src/components/CatalogScreens.cpp")
library = read("src/activities/library/LibraryListActivity.cpp")
hit = read("src/components/HeaderBackTapTarget.h")

scene = base.split("bool BaseTheme::drawExternalHeaderScene(", 1)[1].split("void BaseTheme::drawHeader(", 1)[0]
header = base.split("void BaseTheme::drawHeader(", 1)[1].split("namespace fui = freeink::ui;", 1)[0]

# No device-type gate: both touch and non-touch use the same scene and status.
assert "renderExternalScene(ThemeScene::Region::Header" in scene
assert "renderer.drawText(SMALL_FONT_ID" in scene
assert "drawExternalHeaderScene(renderer, sceneArea, title)" in header
assert "if (gpio.hasTouch() && backButton && title)" in header
assert "HeaderBackTapTarget::set(0, 0, 58, sceneArea.height)" in header
assert "HeaderBackTapTarget::clear();" in header
assert "(!gpio.hasTouch() || !backButton)" not in header

# The Home header still has no Back hit target.
assert re.search(
    r"GUI\.drawHeader\(renderer,\s*Rect\{0, metrics\.topPadding, pageWidth,"
    r"\s*metrics\.homeTopPadding - metrics\.topPadding\},.*?"
    r"nullptr, false\);", home, re.S,
)

# Home's external header ends at homeTopPadding, while pushed screens
# end at topPadding + headerHeight. Include the oriented bezel inset on
# touch boards so Home's banner and battery land at the exact same Y.
assert re.search(
    r"adjustedMetrics\.homeTopPadding\s*=\s*"
    r"std::max\(adjustedMetrics\.homeTopPadding,\s*"
    r"adjustedMetrics\.topPadding \+ adjustedMetrics\.headerHeight\)", theme,
)
assert "const Rect sceneArea{0, 0, renderer.getScreenWidth(), rect.y + rect.height};" in base
assert "metrics.homeTopPadding - metrics.topPadding" in home
assert "if (touch) {" in theme and "if (externalActive) {" in theme

# Example of the X4Pro discrepancy: 9-pixel inset + 62-pixel band = 71.
assert max(56, 9 + 62) == 71

# Native FreeInkUI headers must follow the same scene contract. Do not
# lose catalog Back, trailing action, or library Back/Search/Rebuild routing.
assert "BaseTheme::drawExternalHeaderScene(" in catalog
assert "HeaderBackTapTarget::set(0, 0, 58" in catalog
assert "action.action = trailingAction;" in catalog
assert "fui::button(screen.frame()," in catalog
assert "BaseTheme::drawExternalHeaderScene(" in library
assert "screen.frame().hit(" in library and "ACTION_BACK, 0, fui::InputTouch" in library
assert "action.action = ACTION_SEARCH;" in library
assert "action.action = ACTION_REBUILD;" in library
assert "HeaderBackTapTarget::clear();" in library
assert "tx >= x && tx < x + w" in hit

# The frontlight has no Back action and preserves the themed header.
assert re.search(
    r"BaseTheme::drawHeader\(\s*renderer,\s*Rect\{.*?\},\s*"
    r"nullptr, nullptr, false\);", panel, re.S,
)
assert panel.count("scene->hasRegion(ThemeScene::Region::Header)") == 2
assert "metrics.homeTopPadding - metrics.topPadding - tokens.spaceMd" in panel
assert "metrics.homeTopPadding - metrics.topPadding - theme.spaceMd" in panel

# Stock UI font fallback stays opt-in, not hardwired to X4Pro.
assert re.search(
    r"#if defined\(CROSSPOINT_THEME_FORCE_STOCK_UI_FONTS\).*?"
    r"stock UI fonts \(theme font isolation\).*?return;.*?#endif", theme, re.S,
)

print("Cross-device Halo header, native FUI actions and touch routing: PASS")
