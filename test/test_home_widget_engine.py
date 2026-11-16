"""Compile the native Home Widget Engine and CPB surface adapter on the host.

No firmware build or theme installation. Tests use the production headers and
a minimal fake GfxRenderer/CPB implementation to exercise painter order,
clipping, fallback, touch targets, and background recovery.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ROOT / "test/home_widget_engine/HomeWidgetEngineTest.cpp",
    ROOT / "test/home_widget_engine/HomeWidgetThemeAdaptersTest.cpp",
    ROOT / "test/home_widget_engine/HomeWidgetReadoutsTest.cpp",
    ROOT / "test/home_widget_engine/HomeSceneNamesTest.cpp",
    ROOT / "test/home_widget_engine/HomeGridActivationTest.cpp",
    ROOT / "test/home_widget_engine/HomeLayoutJsonTest.cpp",
)
STUBS = ROOT / "test/home_widget_engine/stubs"
ARDUINOJSON = ROOT / ".pio/libdeps/default/ArduinoJson/src"

compilers = [
    executable for executable in ("g++", "clang++", "c++")
    if shutil.which(executable)
]
if not compilers:
    raise SystemExit("A C++17 host compiler is required")
if not ARDUINOJSON.is_dir():
    raise SystemExit(
        "Missing ArduinoJson host headers: expected .pio/libdeps/default/ArduinoJson/src"
    )

with tempfile.TemporaryDirectory() as directory:
    for compiler in compilers[:2]:
        for source in SOURCES:
            exe = Path(directory) / (
                source.stem + (".exe" if os.name == "nt" else "")
            )
            subprocess.run(
                [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-I", str(STUBS), "-I", str(ROOT / "src"),
                 "-I", str(ARDUINOJSON),
                 str(source), "-o", str(exe)],
                check=True,
            )
            subprocess.run([str(exe)], check=True)
            print(f"Home Widget Engine: {source.name} {compiler} passed", flush=True)
