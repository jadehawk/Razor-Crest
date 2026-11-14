"""Regression coverage for per-theme Home menu footer reservations.

Run with: python test/test_home_menu_footer_layout.py (requires a C++ compiler).
Compiles the production footer policy in a hardware-free harness.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
chrome = (ROOT / "src/components/themes/ThemeChrome.cpp").read_text(encoding="utf-8")
home = (ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
method = "int ThemeScene::homeMenuFooterHeight(" + chrome.split(
    "int ThemeScene::homeMenuFooterHeight(", 1
)[1].split("\nbool ThemeScene::drawMenu(", 1)[0]

# Row capacity, paint bounds and touch swipe bounds must use the same policy.
assert home.count("scene->homeMenuFooterHeight(renderer)") == 2
assert "external->homeMenuFooterHeight(renderer)" in home
assert "homeMenuFooterHeight(renderer) == 80" in chrome

harness = r"""
#include <cassert>
#include <string>
#include "util/HomeMenuGeometry.h"

struct GfxRenderer {
    int width, height;
    int getScreenWidth() const { return width; }
    int getScreenHeight() const { return height; }
};
struct MockGpio {
    bool touch = false;
    bool hasTouch() const { return touch; }
} gpio;
struct ThemeScene {
    std::string menuFooterFile;
    std::string menuFooterTouchFile;
    int homeMenuFooterHeight(const GfxRenderer&) const;
};
""" + method + r"""
int main() {
    GfxRenderer x3{480, 800};
    GfxRenderer touchDevice{600, 800};
    GfxRenderer otherDevice{480, 640};
    ThemeScene theme;

    // Built-in and external themes without footer artwork reserve nothing.
    assert(theme.homeMenuFooterHeight(x3) == 0);
    gpio.touch = true;
    assert(theme.homeMenuFooterHeight(touchDevice) == 0);
    gpio.touch = false;

    // A standard footer uses the original 46px band even on touch hardware.
    theme.menuFooterFile = "/themes/footer.bmp";
    assert(theme.homeMenuFooterHeight(otherDevice) == 46);
    gpio.touch = true;
    assert(theme.homeMenuFooterHeight(touchDevice) == 46);

    // A declared alternate footer needs the original 80px panel on touch
    // and on 480x800 portrait displays, while smaller devices use 46px.
    theme.menuFooterTouchFile = "/themes/footer-touch.bmp";
    assert(theme.homeMenuFooterHeight(touchDevice) == 80);
    gpio.touch = false;
    assert(theme.homeMenuFooterHeight(x3) == 80);
    assert(theme.homeMenuFooterHeight(otherDevice) == 46);

    using namespace home_menu;
    const int noFooter = capacityFor(800, 400, 64, 8, 0, 0);
    const int withFooter = capacityFor(800, 400, 64, 8, 0, 80);
    assert(noFooter == 5 && withFooter == 4);
    assert(safeBottom(800, 0, 0) == 792);
    assert(safeBottom(800, 0, 80) == 712);

    // Physical-button hints remain protected while no-art themes recover a row.
    assert(capacityFor(800, 350, 64, 8, 40, 0) == 5);
    assert(capacityFor(800, 350, 64, 8, 40, 46) == 4);

    // Every menu entry remains selectable and scrollable when capacity changes.
    assert(scrollTopForSelection(0, 7, 8, noFooter) == 3);
    assert(scrollTopForSelection(0, 7, 8, withFooter) == 4);
    assert(visibleRows(8, 3, noFooter) == 5);
    assert(scrollTopBy(0, 50, 8, noFooter) == 3);

    // Final-row spacing is not needed without a decorative footer.
    assert(rowsFitting(111, 54, 3, false) == 2);
    assert(rowsFitting(111, 54, 3, true) == 1);
    assert(capacityFor(800, 480, 46, 0, 0, 0) == 6);
    assert(reclaimForNextRow(312, 46, 0, 16) == 10);
    assert(capacityFor(800, 470, 46, 0, 0, 0) == 7);
    assert(reclaimForNextRow(312, 46, 0, 8) == 0);
    assert(reclaimForNextRow(310, 46, 0, 16) == 12);
    assert(reclaimForNextRow(90, 54, 3, 16) == 0);
    // Physical-button hints and decorative footers stay protected.
    assert(capacityFor(800, 480, 46, 0, 40, 0) == 5);
    assert(capacityFor(800, 480, 46, 0, 0, 80) == 5);
}
"""

with tempfile.TemporaryDirectory() as temp_dir:
    cpp = Path(temp_dir) / "home_menu_footer.cpp"
    exe = Path(temp_dir) / "home_menu_footer.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run([
        "c++", "-std=c++17", "-I", str(ROOT / "src"),
        str(cpp), "-o", str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
print("Home menu footer layout checks passed")
