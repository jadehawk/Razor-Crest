#!/usr/bin/env python3
"""Run the production timer branch against controlled input and sleep boundaries."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", help="also require the unpatched Git ref to reproduce the lost press")
parser.add_argument("--expect-regression", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent


def run(source, name, regression):
    start = source.index("    case HalGPIO::WakeupReason::Timer: {")
    end = source.index("    case HalGPIO::WakeupReason::PowerButton:", start)
    marker = "RTC_NOINIT_ATTR uint32_t timedPowerWakeStamp;"
    handoff = source[source.index(marker):source.index("void setup() {")] if marker in source else ""
    with tempfile.TemporaryDirectory(prefix="timed-power-wake-") as temporary:
        temp = Path(temporary)
        (temp / "TimerCase.inc").write_text(source[start:end])
        power_end = source.index("    case HalGPIO::WakeupReason::AfterUSBPower:", end)
        (temp / "PowerCase.inc").write_text(source[end:power_end])
        (temp / "HandoffSource.inc").write_text(handoff)
        executable = temp / "flow"
        subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-Wall", "-Wextra",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-DHAS_HANDOFF=" + str(int(bool(handoff))), "-I", str(temp),
             str(here / "FlowTest.cpp"), "-o", str(executable)], check=True
        )
        result = subprocess.run([str(executable)] + (["negative-control"] if regression else []))
        expected = 42 if regression else 0
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}")
        print(f"{name}: expected exit {expected}")


run((root / "src/main.cpp").read_text(), "candidate", args.expect_regression)
if args.baseline:
    original = subprocess.check_output(["git", "show", args.baseline + ":src/main.cpp"], cwd=root, text=True)
    run(original, "baseline", True)

capture = (root / "lib/hal/HalGPIO.cpp").read_text()
start = capture.find("void IRAM_ATTR HalGPIO::recordPowerWake(")
if start >= 0 and not args.expect_regression:
    end = capture.index("bool HalGPIO::verifyPowerButtonWakeup()", start)
    with tempfile.TemporaryDirectory(prefix="timed-power-capture-") as temporary:
        temp = Path(temporary)
        (temp / "CaptureSource.inc").write_text(capture[start:end])
        executable = temp / "capture"
        subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++20", "-O0", "-Wall", "-Wextra",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(temp),
             "-I", str(here / "stubs"), "-I", str(root / "lib/hal"),
             str(here / "CaptureTest.cpp"), "-o", str(executable)], check=True
        )
        subprocess.run([str(executable)], check=True)
