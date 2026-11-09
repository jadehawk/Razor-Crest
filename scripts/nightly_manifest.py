#!/usr/bin/env python3
"""Create the published OTA feed only after all Razor-Crest nightly images pass."""

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path

DEVICES = ("x3-x4", "sticky", "x4pro", "x4c", "papermono", "metalio-eink4", "eego-a4")
MAX_FIRMWARE_BYTES = 32 * 1024 * 1024
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+n-[0-9a-f]{7}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


def make_manifest(directory: Path, repo: str, tag: str, version: str, commit: str):
    if not VERSION_PATTERN.fullmatch(version) or tag != f"rn-v{version}":
        raise ValueError("Nightly tag/version must follow rn-v<base>n-<7-char SHA>")
    if not COMMIT_PATTERN.fullmatch(commit):
        raise ValueError("Nightly source commit must be a full SHA-1")
    if not version.endswith(f"n-{commit[:7]}"):
        raise ValueError("Nightly version must match the source commit")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("Invalid GitHub release repository")
    expected = {f"razor-crest-{version}-{device}.bin" for device in DEVICES}
    actual = {p.name for p in directory.glob("razor-crest-*.bin")}
    if actual != expected:
        raise ValueError(f"Expected exactly {len(expected)} nightly firmware images; missing={expected - actual}, extra={actual - expected}")

    assets = []
    for device in DEVICES:
        filename = f"razor-crest-{version}-{device}.bin"
        data = (directory / filename).read_bytes()
        if not (64 * 1024 <= len(data) <= MAX_FIRMWARE_BYTES) or data[0] != 0xE9:
            raise ValueError(f"Invalid ESP application firmware image: {filename}")
        assets.append({
            "name": filename,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "browser_download_url": f"https://github.com/{repo}/releases/download/{tag}/{filename}",
        })
    return {
        "tag_name": tag,
        "channel": "nightly",
        "version": version,
        "source_commit": commit,
        "published_at": datetime.now(timezone.utc).isoformat(),
        "assets": assets,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = make_manifest(args.dist, args.repository, args.tag, args.version, args.commit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
