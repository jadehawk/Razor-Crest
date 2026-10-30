"""Compile the real ClipSelectionActivity constructor declarations and definitions without firmware.

Run: python -X utf8 test/test_clipping_constructor.py
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / "src/activities/reader/ClipSelectionActivity.h").read_text(encoding="utf-8")
implementation = (root / "src/activities/reader/ClipSelectionActivity.cpp").read_text(encoding="utf-8")

declarations = header.split("class ClipSelectionActivity final : public Activity {", 1)[1].split(
    "  void onEnter()", 1
)[0]
definitions = "ClipSelectionActivity::ClipSelectionActivity" + implementation.split(
    "ClipSelectionActivity::ClipSelectionActivity", 1
)[1].split("void ClipSelectionActivity::onEnter()", 1)[0]

source = """
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
struct GfxRenderer {};
struct MappedInputManager {};
struct Page {};
struct Activity {
  Activity(const char*, GfxRenderer&, MappedInputManager&) {}
};
class ClipSelectionActivity : public Activity {
""" + declarations + """
 private:
  std::vector<std::unique_ptr<Page>> pages;
  std::span<const std::unique_ptr<Page>> borrowedPages;
  Selection initialSelection;
  const int marginLeft, marginTop, initialX, initialY;
  const Mode mode;
};
""" + definitions + """
int main() {
  GfxRenderer renderer;
  MappedInputManager input;
  ClipSelectionActivity normal(renderer, input, {}, 0, 0);
  ClipSelectionActivity lookup(renderer, input, {}, 0, 0, -1, -1,
                               ClipSelectionActivity::Mode::Lookup, {0, 0, 7, 7, true});
}
"""

with tempfile.TemporaryDirectory() as directory:
    source_file = Path(directory) / "constructor.cpp"
    executable = Path(directory) / "constructor.exe"
    source_file.write_text(source, encoding="utf-8")
    subprocess.run(
        ["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source_file), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("ClipSelectionActivity constructor compile check passed")
