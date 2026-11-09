#!/usr/bin/env python3
"""Host-only checks for the seven-target nightly feed generator."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from nightly_manifest import DEVICES, make_manifest  # noqa: E402

COMMIT = "b" * 40
VERSION = f"2.0.0n-{COMMIT[:7]}"
TAG = f"rn-v{VERSION}"


class NightlyManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dist = Path(self.temp.name)
        for device in DEVICES:
            filename = f"razor-crest-{VERSION}-{device}.bin"
            (self.dist / filename).write_bytes(b"\xe9" + b"\0" * (64 * 1024 - 1))

    def generate(self):
        return make_manifest(self.dist, "jadehawk/Razor-Crest", TAG, VERSION, COMMIT)

    def test_exactly_seven_compatible_targets_and_pinned_urls(self):
        meta = self.generate()
        self.assertEqual(meta["channel"], "nightly")
        self.assertEqual(meta["source_commit"], COMMIT)
        self.assertEqual(meta["tag_name"], TAG)
        self.assertEqual(len(meta["assets"]), 7)
        self.assertEqual(
            {a["name"] for a in meta["assets"]},
            {f"razor-crest-{VERSION}-{device}.bin" for device in DEVICES},
        )
        for asset in meta["assets"]:
            self.assertEqual(asset["size"], 64 * 1024)
            self.assertEqual(len(asset["sha256"]), 64)
            self.assertTrue(asset["browser_download_url"].startswith(
                f"https://github.com/jadehawk/Razor-Crest/releases/download/{TAG}/"
            ))

    def test_missing_or_extra_target_prevents_publication(self):
        (self.dist / f"razor-crest-{VERSION}-sticky.bin").unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            self.generate()
        (self.dist / f"razor-crest-{VERSION}-sticky.bin").write_bytes(b"\xe9" + b"\0" * (64 * 1024 - 1))
        (self.dist / f"razor-crest-{VERSION}-other.bin").write_bytes(b"\xe9" + b"\0" * (64 * 1024 - 1))
        with self.assertRaisesRegex(ValueError, "extra"):
            self.generate()

    def test_truncated_and_wrong_magic_images_prevent_publication(self):
        path = self.dist / f"razor-crest-{VERSION}-sticky.bin"
        path.write_bytes(b"\xe9" * 10)
        with self.assertRaisesRegex(ValueError, "Invalid ESP"):
            self.generate()
        path.write_bytes(b"\0" * (64 * 1024))
        with self.assertRaisesRegex(ValueError, "Invalid ESP"):
            self.generate()

    def test_reject_invalid_channel_version_and_source(self):
        with self.assertRaises(ValueError):
            make_manifest(self.dist, "jadehawk/Razor-Crest", "razor-crest-v2.0.0-r1", VERSION, COMMIT)
        with self.assertRaises(ValueError):
            make_manifest(self.dist, "jadehawk/Razor-Crest", TAG, VERSION, "untrusted")
        with self.assertRaisesRegex(ValueError, "match the source commit"):
            make_manifest(self.dist, "jadehawk/Razor-Crest", TAG, VERSION, "a" * 40)
        with self.assertRaises(ValueError):
            make_manifest(self.dist, "jadehawk/Razor-Crest", "rn-v2.0.0-n20261108.23", "2.0.0-n20261108.23", COMMIT)
        with self.assertRaises(ValueError):
            make_manifest(self.dist, "jadehawk/Razor-Crest", "rn-vRC.2.0.0n-bbbbbbb", "RC.2.0.0n-bbbbbbb", COMMIT)
        with self.assertRaises(ValueError):
            make_manifest(self.dist, "https://bad.invalid", TAG, VERSION, COMMIT)


if __name__ == "__main__":
    unittest.main()
