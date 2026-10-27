#!/usr/bin/env python3
"""Source guards for the cross-device shared UI-font contract.

These checks do not substitute for a firmware build or X3/X4Pro tests.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
read = lambda name: (ROOT / name).read_text(encoding="utf-8")
scene = read("src/components/themes/ThemeScene.cpp")
registry = read("src/components/themes/ThemePackRegistry.cpp")
firmware_identity = read("src/util/FirmwareIdentity.h")
fonts = read("src/components/themes/ThemeFonts.cpp")
ui = read("src/components/UITheme.cpp")
cache = read("lib/GfxRenderer/FontCacheManager.cpp")
reader_manager = read("lib/EpdFont/SdCardFontManager.cpp")
renderer_h = read("lib/GfxRenderer/GfxRenderer.h")
renderer = read("lib/GfxRenderer/GfxRenderer.cpp")
render_lock = read("src/activities/RenderLock.h")
manager = read("src/activities/ActivityManager.cpp")

assert 'fontRoles["ui"]' in scene
assert "sharedUiFont ? 1 : 4" in scene
assert "sharedUiFont ? \"ui\" : roleNames[i]" in scene
assert "const bool familyVariants = fontRoles.size() == 1" in scene
assert "uiFamilyVariants_ = familyVariants;" in scene
assert "if (familyFormat != advertisedFontFamily) return false;" in scene
assert "theme.font-family.v1" in registry and "theme.font-family.v1" in firmware_identity
assert "variants[variantNames[i]]" in scene
assert "const int sourceCount = shared ? 1 : 4;" in fonts
assert "loaded[shared ? 0 : i].get()" in fonts
assert "renderer.registerSdCardFont(UI_IDS[i], font);" in fonts
assert "renderer.enableDirectSdUiText(UI_IDS[i]);" in fonts
assert "for (int i = 0; i < (onDemandVariants ? 0 : sourceCount); ++i)" in fonts
assert "externalScene.usesUiFontFamilyVariants()" in ui
assert "adjustedMetrics.topPadding = std::max(adjustedMetrics.topPadding, visibleTop);" in ui
assert "renderer.getOrientedViewableTRBL(&visibleTop" in ui
assert "for (int i = 0; i < 4; ++i) renderer.removeFont(UI_IDS[i]);" in fonts

# Aliased font IDs must share a single scan slot and glyph arena.
assert "const auto aliased = sdCardFonts_.find(fontId);" in cache
assert "if (registeredFont == aliased->second)" in cache
assert "fontId = registeredId;" in cache
# Reader-font changes must not clear the global font registry owned by themes.
assert "renderer.clearSdCardFonts();" not in reader_manager
assert "renderer.removeFont(lf.fontId);" in reader_manager

# Unlike book-reading fonts, direct UI text requests a resident glyph batch.
assert "std::set<int> directSdUiFontIds_;" in renderer_h
assert "directSdUiFontIds_.erase(fontId);" in renderer_h
assert "directSdUiFontIds_.clear();" in renderer_h
assert renderer.count("directSdUiFontIds_.count(resolvedFontId)") >= 2
assert "ensureSdGlyphsResident(resolvedFontId, renderedText, style, false);" in renderer

# Theme switch must synchronize with the render task without deadlocking
# when called from an activity's already-locked onExit().
assert "static bool heldByCurrentTask();" in render_lock
assert "bool RenderLock::heldByCurrentTask()" in manager
assert ui.count("if (!RenderLock::heldByCurrentTask()) lock.emplace();") == 3
assert "#if defined(CROSSPOINT_THEME_FORCE_STOCK_UI_FONTS)" in ui

print("Shared theme UI font, renderer caching, and lifecycle source checks PASS")
