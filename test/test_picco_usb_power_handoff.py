#!/usr/bin/env python3
"""Host regression: Picco USB-hold Power handoff survives a released button.

Compiles the production helper from main.cpp with simulated saved app state.
No firmware build or attached device is required.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/main.cpp").read_text(encoding="utf-8")
signature = "static bool resumeAfterPiccoUsbHold() {"
start = source.index(signature)
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
helper = source[start:end]

harness = r"""
#include <cassert>
#include <cstdint>
#include <string>
struct State {
  bool lastSleepFromReader = false;
  std::string openEpubPath;
} APP_STATE;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;
uint32_t silentRebootPayload = 0;
struct SavedSettings {
  int frontlightOn = 0;
  int frontlightRestoreOnWake = 0;
} SETTINGS;
struct Restarted {};
bool holdResult = false;
bool buttonStillHeld = false;
unsigned restarts = 0;
int target = -1;
bool holdSleepWhileUsbPowered() { return holdResult; }
void armSilentReboot(uint32_t value) { target = static_cast<int>(value); silentRebootPayload = 0; }
struct FakeESP {
  void restart() { ++restarts; throw Restarted{}; }
} ESP;
""" + helper + r"""
void testAcceptedPress(bool lastReader, std::string book, int expectedTarget, bool restoreLight) {
  APP_STATE.lastSleepFromReader = lastReader;
  APP_STATE.openEpubPath = std::move(book);
  holdResult = true;
  SETTINGS.frontlightOn = 1;
  SETTINGS.frontlightRestoreOnWake = restoreLight ? 1 : 0;
  // A press accepted by the hold can be released before later teardown.
  buttonStillHeld = false;
  target = -1;
  try {
    resumeAfterPiccoUsbHold();
    assert(false && "accepted press must initiate a restart");
  } catch (const Restarted&) {}
  assert(target == expectedTarget);
  assert(silentRebootPayload == (restoreLight ? SILENT_REBOOT_LIGHT_ON : 0));
}
int main() {
  holdResult = false;
  assert(!resumeAfterPiccoUsbHold() && restarts == 0 && target == -1);
  testAcceptedPress(true, "/books/current.epub", SILENT_REBOOT_TARGET_READER, true);
  testAcceptedPress(true, "/books/current.epub", SILENT_REBOOT_TARGET_READER, false);
  testAcceptedPress(false, "/books/current.epub", SILENT_REBOOT_TARGET_HOME, true);
  testAcceptedPress(true, "", SILENT_REBOOT_TARGET_HOME, false);
  assert(restarts == 4);
}
"""

with tempfile.TemporaryDirectory(prefix="picco-usb-handoff-") as directory:
    src = Path(directory) / "PiccoUsbHandoff.cpp"
    exe = Path(directory) / "PiccoUsbHandoff"
    src.write_text(harness, encoding="utf-8")
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Picco USB-held Power release handoff: reader/home/none PASS")
