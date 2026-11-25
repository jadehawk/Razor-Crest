"""Host-only test of the production adaptive footer geometry."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CODE = r"""
#include "components/themes/HintLayout.h"
#include <cassert>
using hint_layout::allocate;
int main() {
  for (int screen : {480, 528}) {
    for (int preferred : {80, 104, 106, 120}) {
      for (auto need : {std::array<int,4>{50,0,155,155},
                        std::array<int,4>{50,60,140,140},
                        std::array<int,4>{140,155,0,50},
                        std::array<int,4>{200,200,200,200}}) {
        auto boxes = allocate(screen, 8, 8, need, preferred);
        assert(boxes.front().x >= 8);
        assert(boxes.back().x + boxes.back().width <= screen-8);
        int sum = 0;
        for (int i=0;i<4;++i) sum += std::max(12,need[i]);
        for (int i=0;i<4;++i) {
          assert(boxes[i].width > 0);
          if (i) assert(boxes[i].x >= boxes[i-1].x+boxes[i-1].width+8);
          if (sum <= screen-40) assert(boxes[i].width >= need[i]);
        }
      }
    }
    auto groups = allocate(screen,20,10,std::array<int,2>{74,312},(screen-50)/2);
    assert(groups[1].width>=312 && groups[0].width>=74);
    auto slots = allocate(groups[1].width,4,4,std::array<int,2>{152,148},(groups[1].width-12)/2);
    assert(slots[0].width>=152 && slots[1].width>=148);
  }
}
"""
with tempfile.TemporaryDirectory() as td:
    src=pathlib.Path(td)/"test.cpp"
    exe=pathlib.Path(td)/"test"
    src.write_text(CODE,encoding="utf-8")
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-I",str(ROOT/"src"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
for name in ("BaseTheme.cpp","lyra/LyraTheme.cpp","roundedraff/RoundedRaffTheme.cpp","ThemeChrome.cpp"):
    assert "hint_layout::allocate(" in (ROOT/"src/components/themes"/name).read_text(encoding="utf-8")
print("Adaptive hint geometry: PASS (480/528px, empty labels, remapping, overflow, grouped hints)")
