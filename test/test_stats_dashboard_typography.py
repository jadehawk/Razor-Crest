#!/usr/bin/env python3
"""Source checks for theme-independent statistics typography and compact labels."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
dashboard = (ROOT / "src/activities/plugins/StatsDashboardActivity.cpp").read_text(encoding="utf-8")
theme = (ROOT / "src/components/themes/ThemeFonts.cpp").read_text(encoding="utf-8")
main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")

# The theme replaces only UI IDs; the statistics renderer must reference a
# separately registered built-in font for all of its content.
assert "constexpr int kStatsFont = NOTOSANS_12_FONT_ID;" in dashboard
assert not re.search(r"\b(?:UI_10_FONT_ID|UI_12_FONT_ID|SMALL_FONT_ID)\b", dashboard)
assert "renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);" in main
assert "renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);" in main
assert "UI_IDS[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID" in theme
assert "NOTOSANS_12_FONT_ID" not in theme

# Compact cards try to fit whole labels first and wrap only if necessary.
assert "const bool labelFits = renderer.getTextWidth(kStatsFont, widget.label.c_str()" in dashboard
assert "renderer.wrappedText(kStatsFont, widget.label.c_str(), textWidth, 2," in dashboard
assert "const int labelSlotH = labelH * 2 + 2;" in dashboard
assert "top + labelSlotH + 4, fitted.c_str()" in dashboard
assert "const int dividerX = x + std::min(w * 3 / 10, 76);" in dashboard

# Page titles occupy the existing interactive header rather than a second row.
base_theme = (ROOT / "src/components/themes/BaseTheme.cpp").read_text(encoding="utf-8")
scene = (ROOT / "src/components/themes/ThemeScene.cpp").read_text(encoding="utf-8")
theme_interface = (ROOT / "src/components/UITheme.cpp").read_text(encoding="utf-8")
assert "page.title.c_str(), nullptr," in dashboard
assert "true, kStatsFont);" in dashboard
assert "if (interactive) {" in dashboard and "} else {" in dashboard
assert "int y = metrics.topPadding + (interactive ? metrics.headerHeight : 0) + 7;" in dashboard
assert "return metrics.topPadding + metrics.headerHeight + 7;" in dashboard
assert "titleFontId ? titleFontId : spec.titleFontId" in base_theme
assert "drawExternalHeaderScene(renderer, sceneArea, title, backButton, titleFontId)" in base_theme
assert "headerTitleFontId" in theme_interface and "headerTitleFontId" in scene
assert 'op.source == "screen_title" && headerTitleFontId' in scene
assert "overrideTitle ? shortened : upperAscii(shortened)" in scene
rounded = (ROOT / "src/components/themes/roundedraff/RoundedRaffTheme.cpp").read_text(encoding="utf-8")
assert "BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton, titleFontId)" in rounded
assert "GUI.drawButtonHints(renderer" in dashboard

# Tight month insight cards use a two-line fallback instead of painting
# tertiary text beyond their bottom border.
assert "const bool fullLabels = topH >= fullBlockH + 10;" in dashboard
assert 'if (fullLabels) {' in dashboard
assert '" - " + duration(month.bestSeconds)' in dashboard

# The Overall card must keep all three captions away from its bottom border.
assert "const int dividerY = y + h / 2 + 8;" in dashboard
assert "const int valueY = dividerY + 8;" in dashboard
assert "const int labelY = valueY + renderer.getLineHeight(NOTOSANS_14_FONT_ID) + 2;" in dashboard

# Wider X3 hint boxes use available horizontal space before font fallback.
assert "const int buttonWidth = wide ? 120 : 106;" in base_theme
assert "constexpr int wideButtonPositions[] = {12, 140, 268, 396};" in base_theme
wide_positions = (12, 140, 268, 396)
assert wide_positions[0] >= 0 and wide_positions[-1] + 120 <= 528
assert all(wide_positions[i + 1] - (wide_positions[i] + 120) == 8 for i in range(3))
assert "renderer.getTextWidth(NOTOSANS_12_FONT_ID, label) <= maxTextWidth" in base_theme
assert "textFont = NOTOSANS_12_FONT_ID;" in base_theme
assert "renderer.wrappedText(textFont, label, maxTextWidth, 2)" in base_theme

print("Statistics typography, summary borders, X3 button hints PASS")
