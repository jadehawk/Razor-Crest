"""Compile and test production Home Grid Geometry without firmware build.

Run: python test/test_home_grid_geometry.py
Requires a host C++17 compiler (g++ or c++). Uses a temporary directory and
does not modify repository build artifacts.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "test" / "home_grid_geometry" / "HomeGridGeometryTest.cpp"

compiler = next((name for name in ("g++", "clang++", "c++") if shutil.which(name)), None)
if not compiler:
    raise SystemExit("No C++17 host compiler found: install g++ or clang++")

with tempfile.TemporaryDirectory() as temp:
    executable = Path(temp) / ("home_grid_geometry.exe" if os.name == "nt" else "home_grid_geometry")
    subprocess.run(
        [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
         "-pedantic", "-I", str(ROOT / "src"), str(SOURCE),
         "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("Home Grid Geometry V1: all deterministic, cap, failure and stress checks passed")
