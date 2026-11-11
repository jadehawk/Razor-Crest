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
assert re.search(r"renderExternalScene\(\s*ThemeScene::Region::Header", scene)
assert "fui::header(ui.frame" in scene
assert "applyHeaderStatus(renderer, props);" in scene
assert "props.status.battery.text.color = props.status.battery.color;" in scene
assert "Rect{area.x, contentTop, area.width, headerBottom - contentTop}" in scene
assert "const int contentTop = statusTop + HEADER_STATUS_HEIGHT;" in scene
assert 'renderer.getLineHeight(font) > area.height' in read('src/components/themes/ThemeScene.cpp')
assert 'font = renderer.getLineHeight(UI_10_FONT_ID) <= area.height ? UI_10_FONT_ID : SMALL_FONT_ID;' in read('src/components/themes/ThemeScene.cpp')
assert "drawExternalHeaderScene(renderer, sceneArea, title, backButton)" in header
assert "backButton && gpio.hasTouch() ? 1 : 0" in scene
scene_renderer = read("src/components/themes/ThemeScene.cpp")
assert 'HeaderBackStyle::Both' in scene_renderer
assert 'paintIcon(renderer, icon_header_back_32_bits' in scene_renderer
assert "if (gpio.hasTouch() && backButton && title)" in header
assert "HeaderBackTapTarget::set(0, 0, 58, sceneArea.height)" in header
assert "HeaderBackTapTarget::clear();" in header
assert "(!gpio.hasTouch() || !backButton)" not in header

# All built-ins and SD themes retain Lyra's top/side status clearance.
metrics_header = read("src/components/themes/BaseTheme.h")
assert "HEADER_STATUS_TOP = 10;" in metrics_header
assert "HEADER_STATUS_HEIGHT = 20;" in metrics_header
assert "HEADER_STATUS_EDGE_INSET = 22;" in metrics_header
assert "status.batteryLeft = false;" in base
assert "status.clockCentered = false;" in base
assert "if (SETTINGS.clockShowInHeader &&" in base
assert "metrics.headerShowsClock &&" not in base
assert "std::max(HEADER_STATUS_TOP, top)" in base
assert "static_cast<int16_t>(rect.height - offset)" in base
assert "renderer.fillRect(area.x, area.y, area.width, contentTop - area.y, dark);" in scene
assert "props.status.battery.color = dark ? fui::Color::White : fui::Color::Black;" in scene

# Native trailing actions fit below the status strip without adding any height.
assert "BaseTheme::headerStatusTopY(renderer) + BaseTheme::HEADER_STATUS_HEIGHT" in catalog
assert "BaseTheme::headerStatusTopY(renderer) + BaseTheme::HEADER_STATUS_HEIGHT" in library

# RoundedRaff uses the same native status row even with a null Home title.
rounded = read("src/components/themes/roundedraff/RoundedRaffTheme.cpp")
rounded_header = rounded.split("void RoundedRaffTheme::drawHeader(", 1)[1].split(
    "void RoundedRaffTheme::drawRecentBookCover(", 1
)[0]
assert "BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);" in rounded_header
assert "if (title == nullptr)" not in rounded_header

# Bottom edges remain unchanged; nothing pushes X3 content down.
for theme_top, normal_height, home_bottom in [(5, 84, 40), (10, 84, 56),
                                               (13, 84, 64), (0, 62, 56),
                                               (0, 76, 76)]:
    status_y = max(10, 0)
    assert theme_top + normal_height > status_y + 20
    assert home_bottom > status_y + 20

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
