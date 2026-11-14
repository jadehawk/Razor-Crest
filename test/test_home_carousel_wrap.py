"""Run with python test/test_home_carousel_wrap.py (requires a host C++ compiler).

Compile the actual Home navigation lambda with hardware-free test doubles.
"""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
start = "  const auto moveHomeSelection = [this, menuCount](int direction) {"
body = source.split(start, 1)[1].split("\n  };", 1)[0]

# Confirm the side-button carousel boundary handling uses that same navigation.
assert "if (cardPager.canMoveBounded(-1))" in source
assert "moveHomeSelection(-1);  // First book wraps" in source
assert "moveHomeSelection(1);  // Last book moves" in source
assert "if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveHomeSelection(-1);" in source

harness = r"""
#include <cassert>
#include <cstddef>
#include <vector>

struct ButtonNavigator {
    static int nextIndex(int i, int count) { return count > 0 ? (i + 1) % count : 0; }
    static int previousIndex(int i, int count) { return count > 0 ? (i + count - 1) % count : 0; }
};

struct TestHome {
    struct Pager {
        size_t index = 0;
        size_t size = 3;
        bool enabled = true;
        bool canCycle() const { return enabled && size > 1; }
    } cardPager;
    std::vector<int> recentBooks{10, 11, 12};
    int selectorIndex = 0;
    bool singleCardMode = true;
    int cardChanges = 0;
    int recentSlots() const { return recentBooks.empty() ? 0 : 1; }
    void showCardAt(size_t i, int direction) {
        assert(direction == -1 || direction == 1);
        assert(i < recentBooks.size());
        cardPager.index = i;
        ++cardChanges;
    }
    void ensureHomeMenuSelectionVisible() {}
    void requestUpdate() {}
    void navigate(int menuCount, int direction) {
        const auto moveHomeSelection = [this, menuCount](int direction) {
""" + body + r"""
        };
        moveHomeSelection(direction);
    }
};

int main() {
    TestHome home;
    home.navigate(8, -1);  // First book -> last shortcut, unchanged first book.
    assert(home.selectorIndex == 7 && home.cardPager.index == 0 && home.cardChanges == 0);

    home.navigate(8, 1);  // Last shortcut -> first book.
    assert(home.selectorIndex == 0 && home.cardPager.index == 0 && home.cardChanges == 1);

    home.selectorIndex = 1;
    home.navigate(8, -1);  // First shortcut -> last book.
    assert(home.selectorIndex == 0 && home.cardPager.index == 2 && home.cardChanges == 2);

    home.navigate(8, 1);  // Carousel forward -> first shortcut, hold last book.
    assert(home.selectorIndex == 1 && home.cardPager.index == 2);
    home.navigate(8, 1);  // Normal shortcuts stay in sequence.
    assert(home.selectorIndex == 2 && home.cardPager.index == 2);

    home.singleCardMode = false; // Legacy list still wraps flatly.
    home.selectorIndex = 0;
    home.navigate(8, -1);
    assert(home.selectorIndex == 7 && home.cardPager.index == 2 && home.cardChanges == 2);

    home.singleCardMode = true;
    home.cardPager.size = 1; // Disabled/one-book carousel falls back to old list.
    home.selectorIndex = 1;
    home.navigate(8, -1);
    assert(home.selectorIndex == 0 && home.cardChanges == 2);

    home.cardPager.size = 3;
    home.cardPager.enabled = false;
    home.selectorIndex = 7;
    home.navigate(8, 1);
    assert(home.selectorIndex == 0 && home.cardChanges == 2);
}
"""

with tempfile.TemporaryDirectory() as tmp_dir:
    cpp = Path(tmp_dir) / "home_carousel_wrap.cpp"
    exe = Path(tmp_dir) / "home_carousel_wrap.exe"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run(["c++", "-std=c++17", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

print("Home carousel circular navigation checks passed")
