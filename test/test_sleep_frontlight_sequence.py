"""Regression: sleep network sync begins only after the frontlight turns off."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
HAL = (ROOT / "lib/hal/HalPowerManager.cpp").read_text(encoding="utf-8")


class SleepFrontlightSequenceTest(unittest.TestCase):
    def test_frontlight_off_before_sleep_setup_render_and_network_sync(self):
        flow = MAIN.split("void enterDeepSleep(bool fromTimeout = false) {", 1)[1].split(
            "\nvoid setupDisplayAndFonts(", 1
        )[0]
        steps = [
            flow.index("Frontlight.setOn(false);"),
            flow.index("bleturner::beforeSleep(2000);"),
            flow.index("emitSleepPluginEvents();"),
            flow.index("activityManager.captureSleepSyncProgress("),
            flow.index("drainSleepPlugins(pluginevents::DrainKind::SleepImage,"),
            flow.index("activityManager.goToSleep(fromTimeout);"),
            flow.index("drainSleepPlugins(pluginevents::DrainKind::Deferred,"),
            flow.index("KOReaderSyncActivity::syncOnSleep("),
            flow.index("powerManager.startDeepSleep(gpio, timerWakeUs);"),
        ]
        self.assertEqual(steps, sorted(steps))
        self.assertEqual(flow.count("Frontlight.setOn(false);"), 1)

    def test_hardware_still_parks_frontlight_on_deep_sleep(self):
        self.assertLess(
            HAL.index("Frontlight.setOn(false);"),
            HAL.index("Frontlight.prepareForDeepSleep();"),
        )


if __name__ == "__main__":
    unittest.main()
