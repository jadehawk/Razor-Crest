"""Check the production interval gate, including the disabled Metalio target."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = "59555361aef55608cc56fdb5a9fccc167c37db34"
sources = [("original", subprocess.check_output(["git", "show", base + ":src/CrossPointSettings.h"], cwd=root, text=True)),
           ("candidate", (root / "src/CrossPointSettings.h").read_text())]
with tempfile.TemporaryDirectory(prefix="papermono-interval-") as temporary:
    temp = Path(temporary)
    for name, source in sources:
        start = source.index("  uint64_t getTimedSleepScreenRefreshUs() const {")
        end = source.index("\n  }", start) + len("\n  }")
        enum_start = source.index("  enum TIMED_SLEEP_SCREEN_REFRESH")
        enum_end = source.index("\n  };", enum_start) + len("\n  };")
        body = "#include <cstdint>\nstruct Settings {\n" + source[enum_start:enum_end]
        body += "\nuint8_t timedSleepScreenRefresh = 0;\n" + source[start:end] + "\n};\n"
        body += """int main() {
          Settings s;
          if (s.getTimedSleepScreenRefreshUs() != 0) return 1;
          s.timedSleepScreenRefresh = Settings::TIMED_SLEEP_REFRESH_1_MIN;
          if (s.getTimedSleepScreenRefreshUs() != (FREEINK_DEVICE_METALIO_EINK4 ? 0ULL : 60000000ULL)) return 42;
          s.timedSleepScreenRefresh = 255;
          return s.getTimedSleepScreenRefreshUs() == 0 ? 0 : 2;
        }"""
        file = temp / "interval.cpp"
        file.write_text(body)
        for paper, metalio in ([(1, 0)] if name == "original" else [(1, 0), (0, 0), (0, 1)]):
            exe = temp / "interval"
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-fsanitize=address,undefined",
                            f"-DFREEINK_DEVICE_PAPERMONO={paper}", f"-DFREEINK_DEVICE_METALIO_EINK4={metalio}",
                            str(file), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)])
            assert result.returncode == (42 if name == "original" else 0)
print("Interval: baseline negative42, PaperMono/ESP enabled, Metalio disabled, Off/invalid PASS")
