#!/usr/bin/env python3
"""Exercise actual PaperMono HAL/SDK code with simulated I2C, reset and shutdown."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-control", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
hardware = root / "freeink-sdk/libs/hardware"
rtc = hardware / "Rtc"
base = "59555361aef55608cc56fdb5a9fccc167c37db34"


def wake(source):
    return source[source.index("HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {"):]


with tempfile.TemporaryDirectory(prefix="papermono-timer-") as tmp:
    temp = Path(tmp)
    variants = [("candidate", (root / "lib/hal/HalGPIO.cpp").read_text())]
    if args.negative_control:
        variants.insert(0, ("original", subprocess.check_output(
            ["git", "show", base + ":lib/hal/HalGPIO.cpp"], cwd=root, text=True)))
    flags = ["c++", "-std=c++20", "-O0", "-Wall", "-Wextra", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-DFREEINK_DEVICE_PAPERMONO=1", "-DFREEINK_CAP_RTC=1"]
    for path in (temp, rtc / "test/host", rtc / "test/host/stubs", hardware / "BatteryMonitor/test/host/stubs",
                 rtc / "include", hardware / "BoardConfig/include", root / "lib/hal"):
        flags += ["-I", str(path)]
    for name, source in variants:
        (temp / "WakeSource.inc").write_text(wake(source))
        exe = temp / name
        subprocess.run([*flags, str(rtc / "src/Rtc.cpp"), str(here / "PaperMonoTimerTest.cpp"), "-o", str(exe)], check=True)
        if name == "original":
            result = subprocess.run([str(exe), "0", "negative-control"])
            assert result.returncode == 42
        else:
            for case in range(13):
                subprocess.run([str(exe), str(case)], check=True)
        print(name + ": expected wake outcomes confirmed")
    source = (root / "lib/hal/HalPowerManager.cpp").read_text()
    start = source.index("void HalPowerManager::startDeepSleep(")
    end = source.index("uint16_t HalPowerManager::getBatteryPercentage()", start)
    (temp / "SleepSource.inc").write_text(source[start:end])
    exe = temp / "sleep"
    subprocess.run([*flags, str(here / "SleepFlowTest.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    capture = (root / "lib/hal/HalGPIO.cpp").read_text()
    start = capture.index("void IRAM_ATTR HalGPIO::recordPowerWake(")
    end = capture.index("bool HalGPIO::verifyPowerButtonWakeup()", start)
    (temp / "CaptureSource.inc").write_text(capture[start:end])
    exe = temp / "capture"
    subprocess.run(["c++", "-std=c++20", "-O0", "-Wall", "-Wextra", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-DFREEINK_DEVICE_PAPERMONO=1", "-I", str(temp),
                    "-I", str(root / "test/timed_power_wake/stubs"), "-I", str(root / "lib/hal"),
                    str(here / "PowerCaptureTest.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PaperMono: 13 wake/reset cases, RTC API, 10 shutdown/handoff paths and PMIC capture PASS")

subprocess.run(["python3", str(here / "check_interval.py")], check=True)
