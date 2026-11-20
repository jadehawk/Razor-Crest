#!/usr/bin/env python3
"""Check advertised dashboard capability matches the renderer's supported widgets."""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_IDENTITY = ROOT / "src/util/FirmwareIdentity.h"
DASHBOARD = ROOT / "src/activities/plugins/StatsDashboardActivity.cpp"


class DashboardCapabilityTests(unittest.TestCase):
    def test_current_widget_support_is_advertised_separately_from_v1(self):
        header = FIRMWARE_IDENTITY.read_text(encoding="utf-8")
        match = re.search(
            r"#define CROSSPOINT_PLUGIN_CAPABILITIES(?P<body>.*?)\n#endif",
            header,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "missing firmware capability declaration")
        advertised = set("".join(re.findall(r'"([^"]*)"', match.group("body"))).split(","))
        self.assertTrue(
            {"stats.native.v1", "ui.dashboard.v1", "ui.dashboard.v2"} <= advertised
        )

        renderer = DASHBOARD.read_text(encoding="utf-8")
        for widget in ("time_of_day", "month_calendar", "month_insights"):
            with self.subTest(widget=widget):
                self.assertIn(f'widget.type == "{widget}"', renderer)

        required = {"stats.native.v1", "ui.dashboard.v2"}
        self.assertTrue(required <= advertised)
        self.assertFalse(required <= {"stats.native.v1", "ui.dashboard.v1"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
