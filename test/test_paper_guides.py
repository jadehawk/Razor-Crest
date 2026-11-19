#!/usr/bin/env python3
"""Host-side structural regressions for the paper guide feature."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
settings_h = (root / "src/CrossPointSettings.h").read_text(encoding="utf-8")
settings_list = (root / "src/SettingsList.cpp").read_text(encoding="utf-8")
settings_screen = (root / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
settings_actions = (root / "src/activities/settings/SettingsActivity.h").read_text(encoding="utf-8")
guides_h = (root / "src/activities/settings/PaperGuidesActivity.h").read_text(encoding="utf-8")
guides_ui = (root / "src/activities/settings/PaperGuidesActivity.cpp").read_text(encoding="utf-8")
menu_h = (root / "src/activities/reader/EpubReaderMenuActivity.h").read_text(encoding="utf-8")
menu_cpp = (root / "src/activities/reader/EpubReaderMenuActivity.cpp").read_text(encoding="utf-8")
reader = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
english = (root / "lib/I18n/translations/english.yaml").read_text(encoding="utf-8")

fields = (
    ("paperGuidesMode", "STR_PAPER_GUIDES"),
    ("paperGuidesDarkness", "STR_RULE_DARKNESS"),
    ("paperGuidesThickness", "STR_RULE_THICKNESS"),
    ("paperGuidesOffset", "STR_RULE_POSITION"),
    ("paperGuidesInset", "STR_RULE_INSET"),
    ("paperGuidesDotted", "STR_DOTTED_RULES"),
    ("paperGuidesDashLength", "STR_RULE_DASH_LENGTH"),
    ("paperGuidesGap", "STR_RULE_DASH_GAP"),
    ("paperGuidesGridSize", "STR_GRAPH_GRID_SIZE"),
)
for field, label in fields:
    assert f"uint8_t {field} =" in settings_h, field
    assert settings_list.count(f'"{field}"') == 1, field
    assert f"&CrossPointSettings::{field}" in settings_list, field
    assert f"{label}:" in english, label

# Generic persistence uses the shared visitor; no new JSON store or SD file.
storage = (root / "src/CrossPointSettings.cpp").read_text(encoding="utf-8")
assert storage.count("forEachBaseSetting(visit);") == 2
assert all(".withWebHidden()" in settings_list[settings_list.index(f'"{field}"'):settings_list.index(f'"{field}"') + 80] for field in ("paperGuidesMode", "paperGuidesDotted"))

# The reader has one Decorators entry, with an independent Paper Guides child.
assert "Decorators," in settings_actions
assert "SettingsSection::Reader, StrId::STR_DECORATORS, SettingAction::Decorators" in settings_screen
assert "makeUniqueNoThrow<PaperGuidesActivity>" in settings_screen
assert "enum class Page : uint8_t { Decorators, Guides }" in guides_h
assert "startActivityForResult(std::move(child)" in guides_ui

# Off has no secondary options; the mode-specific row is exclusive.
assert "if (SETTINGS.paperGuidesMode != 0)" in guides_ui
assert "if (SETTINGS.paperGuidesMode == 1)" in guides_ui
assert "Row::Position" in guides_ui and "Row::GridSize" in guides_ui
assert "if (SETTINGS.paperGuidesDotted)" in guides_ui
assert "Row::Thickness" in guides_ui
assert "showPixels(StrId::STR_RULE_THICKNESS, &CrossPointSettings::paperGuidesThickness, 1, 4, 1)" in guides_ui
assert "popup_.handleInput(mappedInput" in guides_ui
assert "UiListActivity::render(std::move(lock))" in guides_ui

# Drawing is part of the existing EPUB renderer, before foreground text, and
# repeats for grayscale planes (including strip uploads on constrained panels).
paint = reader.split("void EpubReaderActivity::renderContents(", 1)[1]
assert "drawPaperGuides(0, renderer.getDisplayHeight());\n  drawClippingHighlights" in paint
assert "auto renderGrayscalePass = [&](const int stripTop, const int stripBottom)" in paint
assert "drawPaperGuides(stripTop, stripBottom);" in paint
assert paint.count("renderGrayscalePass(y, y + rows)") == 3
assert paint.count("renderGrayscalePass(0, renderer.getDisplayHeight())") == 2
assert "case GfxRenderer::PortraitInverted:" in paint
assert "case GfxRenderer::LandscapeClockwise:" in paint
assert "case GfxRenderer::LandscapeCounterClockwise:" in paint
assert "TAG_PageLine" in paint and "CssTextAlign::Center" in paint
assert "originY + grid" in paint and "originX + grid" in paint
assert "const int thickness = std::clamp<int>(SETTINGS.paperGuidesThickness, 1, 4)" in paint
assert "y + thickness <= y0" in paint and "x + thickness <= x0" in paint
assert "std::min(y + thickness, y1) - top" in paint
assert "x0 - (x0 - originX) % period" in paint
assert "y0 - (y0 - originY) % period" in paint
assert "makeUniqueNoThrow" not in paint.split("const auto drawPaperGuides =", 1)[1].split("ImageBlock::clearRenderFailures()", 1)[0]

# Dash phase is anchored to the page origin, even when rendering adjacent strips.
def rendered_dashes(origin, begin, end, dash=4, gap=4):
    period = dash + gap
    start = begin - (begin - origin) % period
    result = []
    for start_pos in range(start, end, period):
        result.extend(range(max(start_pos, begin), min(start_pos + dash, end)))
    return result

whole = rendered_dashes(5, 5, 132)
stripped = rendered_dashes(5, 5, 80) + rendered_dashes(5, 80, 132)
assert whole == stripped

# A thick horizontal guide crossing a strip boundary must be contiguous.
def clipped_rule(y, thickness, start, end):
    if y >= end or y + thickness <= start:
        return []
    return list(range(max(y, start), min(y + thickness, end)))

for thickness in range(1, 5):
    for y in range(76, 83):
        assert clipped_rule(y, thickness, 0, 160) == (
            clipped_rule(y, thickness, 0, 80) + clipped_rule(y, thickness, 80, 160)
        )

# The list and toolbar More panels share this menu builder. Both can open the
# same decorators subpage; existing Text Settings remains a distinct shortcut.
assert "DECORATORS," in menu_h
assert "MAX_MENU_ITEMS = 20" in menu_h
assert "items.push_back({MenuAction::DECORATORS, StrId::STR_DECORATORS})" in menu_cpp
assert "case EpubReaderMenuActivity::MenuAction::DECORATORS:" in reader
assert "makeUniqueNoThrow<PaperGuidesActivity>" in reader
assert "rememberCurrentContentOffset();" in reader
assert "startActivityForResult(std::move(decorators)" in reader
more = reader.split("void EpubReaderActivity::buildMoreActions()", 1)[1].split("void EpubReaderActivity::moreRowName", 1)[0] if "void EpubReaderActivity::moreRowName" in reader else reader.split("void EpubReaderActivity::buildMoreActions()", 1)[1].split("std::string EpubReaderActivity::moreRowName", 1)[0]
assert "EpubReaderMenuActivity::buildMenuItems(moreItems" in more
assert "MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS" in more

print("Paper Guides: persistent settings, navigation, render passes, strip dash phase PASS")
