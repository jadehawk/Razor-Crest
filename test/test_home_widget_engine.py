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
SOURCE = ROOT / "test/home_widget_engine/HomeWidgetEngineTest.cpp"
STUBS = ROOT / "test/home_widget_engine/stubs"

compilers = [
    executable for executable in ("g++", "clang++", "c++")
    if shutil.which(executable)
]
if not compilers:
    raise SystemExit("A C++17 host compiler is required")

with tempfile.TemporaryDirectory() as directory:
    for compiler in compilers[:2]:
        exe = Path(directory) / ("widget-test.exe" if os.name == "nt" else "widget-test")
        subprocess.run(
            [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-I", str(STUBS), "-I", str(ROOT / "src"),
             str(SOURCE), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)
        print(f"Home Widget Engine: {compiler} tests passed", flush=True)
