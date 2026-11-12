"""Source-level regressions for the on-demand About storage details view."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "src/activities/settings/AboutActivity.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src/activities/settings/AboutActivity.h").read_text(encoding="utf-8")


def method(name):
    start = CPP.index(f"void AboutActivity::{name}(")
    end = CPP.find("\nvoid AboutActivity::", start + 1)
    return CPP[start:end if end >= 0 else None]


class AboutStorageDetailsTest(unittest.TestCase):
    def test_about_does_not_scan_sd_usage_on_enter(self):
        enter = method("onEnter")
        self.assertIn("Storage.sdTotalBytes()", enter)
        self.assertNotIn("Storage.sdUsedBytes(", enter)
        self.assertNotIn("GUI.drawPopup(", enter)
        self.assertNotIn('"Calculate"', CPP)

    def test_single_navigation_row_replaces_usage_rows(self):
        self.assertIn("static constexpr int ITEM_COUNT = 13", HEADER)
        self.assertIn("static constexpr int DETAIL_COUNT = 2", HEADER)
        self.assertIn('"Storage Details",', CPP)
        self.assertIn('rowValues_[ITEM_STORAGE_DETAILS] = ">";', CPP)
        self.assertIn('detailItems_[0].label = "SD Used";', CPP)
        self.assertIn('detailItems_[1].label = "SD Free";', CPP)
        self.assertIn("showingStorageDetails_ ? detailItems_ : rowItems_", CPP)

    def test_detail_scan_runs_only_on_selection_and_is_cached(self):
        activate = method("activateIndex")
        self.assertIn("index != ITEM_STORAGE_DETAILS", activate)
        self.assertIn("if (!sdUsageCalculated_)", activate)
        self.assertIn("GUI.drawPopup(renderer, tr(STR_LOADING_POPUP))", activate)
        self.assertEqual(activate.count("Storage.sdUsedBytes("), 1)
        self.assertIn("sdUsageCalculated_ = true;", activate)
        self.assertIn('detailValues_[0] = detailValues_[1] = "Error.";', activate)

    def test_back_restores_about_and_storage_row_focus(self):
        back = method("onBackButton")
        self.assertIn("if (!showingStorageDetails_)", back)
        self.assertIn("finish();", back)
        self.assertIn("showingStorageDetails_ = false;", back)
        self.assertIn("nav.reset(ITEM_STORAGE_DETAILS);", back)
        self.assertIn("requestUpdate();", back)


if __name__ == "__main__":
    unittest.main()
