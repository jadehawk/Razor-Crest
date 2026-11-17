"""Stage 4 Home grid runtime wiring checks without compiling device firmware.

Runtime code must use the same resolved rectangle table for painting and
touch; original Home actions, carousel and scrolling remain in charge.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
home = (root / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
activity_h = (root / "src/activities/home/HomeActivity.h").read_text(encoding="utf-8")
adapter = (root / "src/components/home/HomeWidgetThemeAdapters.h").read_text(encoding="utf-8")
registry = (root / "src/components/themes/ThemePackRegistry.cpp").read_text(encoding="utf-8")

enter = home.split("void HomeActivity::onEnter()", 1)[1].split(
    "void HomeActivity::onExit()", 1
)[0]
loop = home.split("void HomeActivity::loop()", 1)[1].split(
    "bool HomeActivity::renderGridHome()", 1
)[0]
paint = home.split("bool HomeActivity::renderGridHome()", 1)[1].split(
    "void HomeActivity::render(RenderLock&&)", 1
)[0]
render = home.split("void HomeActivity::render(RenderLock&&)", 1)[1].split(
    "void HomeActivity::onRecentBookSelected", 1
)[0]

# An accepted SD layout must have one native menu and compatible book focus
# before being retained; its menu height comes from the resolved menu area.
assert "home_grid::session::load(" in enter
assert "home_grid::interaction::locate(" in enter
assert "home_grid::interaction::visibleMenuCapacity(" in enter
assert "rowHeightMatches && bookMatches" in enter
assert "homeMenuViewportRows = capacity;" in enter
assert "homeGridCandidate = std::move(candidate);" in enter
assert "ensureHomeMenuSelectionVisible();" in enter

# Navigation/activation remains the original selection-index based state
# machine. The new touch boundaries come from the exact same grid candidate.
assert "ButtonNavigator::nextIndex" in loop
assert "ButtonNavigator::previousIndex" in loop
assert "cycleCard(" in loop
assert "activateSelection();" in loop
assert "homeGridCandidate->active.rectangles" in loop
assert "home_grid::interaction::inside(gridAreas.book" in loop
assert "gridAreas.menu.y + gridAreas.menu.height" in loop
assert "menuLeft, menuRight, menuRowHeight" in loop
assert "homeMenuViewportTop = nextTop;" in loop

# On the live renderer, the *same* resolved table drives all native paint
# calls; failure invalidates the grid and fully repaints legacy Home.
assert "state.active.rectangles" in paint
assert "home_widgets::paint(" in paint
assert "home_widgets::firmware::makeSurface(" in paint
assert "view.externalBookScene = areas.bookScene;" in paint
assert "view.restoreCoverBuffer = &restoreSnapshot;" in paint
assert "GUI.drawButtonHints(" in paint
assert "scene->drawMenu(" in paint
assert "if (renderGridHome()) return;" in render
assert "homeGridCandidate.reset();" in render
assert "freeCoverBuffer();" in render
assert "renderer.clearScreen();" in render
assert "HomeMenuRegistry::capacityFor(" in render
assert "renderGridHome();" in activity_h

# Layout parsing is strictly cold-path. The installed package capability
# remains disabled until the final runtime/device validation gate.
assert "home_grid::json::parse" not in paint
assert "home_grid::session::load(" not in paint
assert "home_grid::session::load(" not in loop
assert 'strcmp(cap, "theme.home-layout.v1") == 0' not in registry
assert 'strcmp(cap, "home.widgets.v1") == 0' not in registry

# The same existing renderer manages cover cache state and menu decorations.
assert "view.theme.getTheme().drawRecentBookCover(" in adapter
assert "view.theme.getTheme().drawButtonMenu(" in adapter
assert "view.theme.renderExternalScene(" in adapter
assert "view.theme.renderExternalHomeScene(" in adapter

print("Home grid runtime paint/input/fallback wiring: PASS")
