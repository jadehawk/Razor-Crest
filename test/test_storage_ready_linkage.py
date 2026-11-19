"""Guard theme/wallpaper SD readiness linkage after upstream merges.

The registries call Storage.ready(); the corresponding HalStorage method must
be defined, not merely declared, or device linking fails.
"""

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class StorageReadyLinkageTests(unittest.TestCase):
    def test_hal_storage_declares_and_defines_ready(self):
        header = (ROOT / "lib/hal/HalStorage.h").read_text(encoding="utf-8")
        source = (ROOT / "lib/hal/HalStorage.cpp").read_text(encoding="utf-8")
        sdk_header = (
            ROOT / "freeink-sdk/libs/hardware/SDCardManager/include/SDCardManager.h"
        ).read_text(encoding="utf-8")
        self.assertRegex(header, r"\bbool\s+ready\s*\(\s*\)\s*const\s*;")
        self.assertRegex(
            source,
            r"\bbool\s+HalStorage::ready\s*\(\s*\)\s*const\s*\{\s*"
            r"return\s+SDCard\.ready\s*\(\s*\)\s*;\s*\}",
        )
        self.assertRegex(sdk_header, r"\bbool\s+ready\s*\(\s*\)\s*const\s*;")

    def test_theme_and_wallpaper_scan_require_ready(self):
        theme = (ROOT / "src/components/themes/ThemePackRegistry.cpp").read_text(
            encoding="utf-8"
        )
        wallpaper = (
            ROOT / "src/components/wallpapers/WallpaperPackRegistry.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("if (!Storage.ready()) return;", theme)
        self.assertIn("if (!Storage.ready()) return;", wallpaper)
        self.assertIn("if (!Storage.ready()) return false;", wallpaper)


if __name__ == "__main__":
    unittest.main(verbosity=2)
