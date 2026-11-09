#!/usr/bin/env python3
"""Host-only release notes regression tests."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from release_notes import DEVICE_LABELS, build_notes, changes, previous_stable  # noqa: E402

SOURCE = "b" * 40
REPO = "jadehawk/Razor-Crest"
VERSION = "2.0.0-r10"
TAG = "razor-crest-v2.0.0-r10"


class ReleaseNotesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        directory = Path(self.temp.name)
        self.assets = []
        for device in DEVICE_LABELS:
            filename = f"razor-crest-{VERSION}-{device}.bin"
            file = directory / filename
            file.write_bytes(b"\xe9" + b"\0" * 99)
            self.assets.append(file)

    @patch("release_notes.ancestor", return_value=True)
    @patch("release_notes.git")
    def test_grouped_changelog_and_compare(self, mock_git, _ancestor):
        mock_git.return_value = (
            "12345678\tfeat(plugins): add catalog notice\n"
            "34567890\tfix: repair Wi-Fi\n"
            "56789012\tperf(ttf): share font allocations\n"
            "78901234\tdocs: release notes"
        )
        notes = changes("previous", SOURCE, REPO)
        for text in ("**Features**", "**Fixes**", "**Performance**",
                     "**Other changes**", "feat(plugins): add catalog notice",
                     "compare/previous..." + SOURCE):
            self.assertIn(text, notes)

    @patch("release_notes.ancestor", return_value=True)
    @patch("release_notes.git")
    def test_stable_previous_release_is_nearest_ancestor(self, mock_git, _ancestor):
        mock_git.return_value = "razor-crest-v2.0.0-r9\nrazor-crest-v2.0.0-r7"
        self.assertEqual(previous_stable(VERSION, SOURCE), "razor-crest-v2.0.0-r9")

    @patch("release_notes.changes", return_value="A changelog entry")
    @patch("release_notes.previous_stable", return_value="razor-crest-v2.0.0-r9")
    @patch("release_notes.ancestor", return_value=True)
    def test_stable_contains_eight_device_table_and_changelog(self, _ancestor, _previous, _changes):
        notes = build_notes("stable", VERSION, TAG, SOURCE, REPO, "2.0.0", self.assets)
        self.assertEqual(notes.count(" | `razor-crest-2.0.0-r10-"), 8)
        self.assertIn("### Changes since previous Stable", notes)
        self.assertIn("A changelog entry", notes)
        self.assertIn("BOOX Picco", notes)
        self.assertIn("USB app-only", notes)
        self.assertIn("| Device | Firmware asset | Size |", notes)

    @patch("release_notes.changes", return_value="Only Nightly changes")
    @patch("release_notes.ancestor", return_value=True)
    def test_nightly_uses_previous_nightly_commit(self, _ancestor, mock_changes):
        version = "2.0.0n-bbbbbbb"
        assets = []
        for file in self.assets:
            target = file.parent / file.name.replace(VERSION, version)
            target.write_bytes(file.read_bytes())
            assets.append(target)
        notes = build_notes("nightly", version, "rn-v" + version, SOURCE, REPO, "2.0.0",
                            assets, previous="a" * 40)
        self.assertIn("### Changes since previous Nightly", notes)
        self.assertIn("Only Nightly changes", notes)
        self.assertEqual(mock_changes.call_args.args[0], "a" * 40)

    def test_missing_picco_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            build_notes("stable", VERSION, TAG, SOURCE, REPO, "2.0.0",
                        [f for f in self.assets if not f.name.endswith("-picco.bin")])


if __name__ == "__main__":
    unittest.main()
