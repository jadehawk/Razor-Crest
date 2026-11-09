#!/usr/bin/env python3
"""Generate concise, source-grounded Stable and Nightly GitHub release notes."""

import argparse
import re
import subprocess
from pathlib import Path

DEVICE_LABELS = {
    "x3-x4": "X3 / X4",
    "sticky": "Seeed reTerminal Sticky",
    "x4pro": "X4 Pro",
    "x4c": "X4 Classic",
    "papermono": "Paper Mono",
    "metalio-eink4": "MetalIO EInk4",
    "eego-a4": "EEGO A4",
    "picco": "BOOX Picco",
}
GROUPS = ("Features", "Fixes", "Performance", "Other changes")
MAX_ENTRIES = 60


def git(*args):
    result = subprocess.run(["git", *args], text=True, capture_output=True, check=True)
    return result.stdout.strip()


def ancestor(ref, source):
    try:
        return subprocess.run(["git", "merge-base", "--is-ancestor", ref, source],
                              capture_output=True, check=False).returncode == 0
    except OSError:
        return False


def previous_stable(version, source):
    match = re.fullmatch(r"(.+)-r([1-9][0-9]*)", version)
    if not match:
        return ""
    base, revision = match.groups()
    candidates = []
    for tag in git("tag", "--list", f"razor-crest-v{base}-r*").splitlines():
        found = re.fullmatch(rf"razor-crest-v{re.escape(base)}-r([1-9][0-9]*)", tag)
        if found and int(found.group(1)) < int(revision):
            candidates.append((int(found.group(1)), tag))
    for _, tag in sorted(candidates, reverse=True):
        if ancestor(tag, source):
            return tag
    return ""


def changes(previous, source, repo, max_entries=MAX_ENTRIES):
    if not previous or not ancestor(previous, source):
        return "First available release in this series; no prior comparable source revision."
    rows = git("log", "--no-merges", "--format=%h%x09%s",
               f"{previous}..{source}").splitlines()
    if not rows:
        return "No non-merge commits since the previous release."
    groups = {name: [] for name in GROUPS}
    for line in rows:
        sha, sep, subject = line.partition("\t")
        if not sep:
            continue
        subject = subject.replace("|", "/").replace("`", "'").strip()
        if re.match(r"(?i)^feat(?:\([^)]*\))?:", subject):
            group = "Features"
        elif re.match(r"(?i)^(fix|bugfix)(?:\([^)]*\))?:", subject):
            group = "Fixes"
        elif re.match(r"(?i)^perf(?:\([^)]*\))?:", subject):
            group = "Performance"
        else:
            group = "Other changes"
        groups[group].append(f"- {subject} ([{sha}](https://github.com/{repo}/commit/{sha}))")
    lines = []
    count = 0
    for group in GROUPS:
        selected = groups[group][:max(0, max_entries - count)]
        if selected:
            lines += [f"**{group}**", "", *selected, ""]
            count += len(selected)
    if len(rows) > count:
        lines.append(f"_And {len(rows) - count} additional commits; see the compare link below._")
    lines.append(f"[View full changes](https://github.com/{repo}/compare/{previous}...{source})")
    return "\n".join(lines)


def build_notes(channel, version, tag, source, repo, base, firmware_files,
                previous="", notice=""):
    expected = {f"razor-crest-{version}-{device}.bin" for device in DEVICE_LABELS}
    assets = {path.name: path for path in firmware_files}
    if assets.keys() != expected:
        raise ValueError(f"Expected eight matching firmware images: missing={expected - assets.keys()}, "
                         f"extra={assets.keys() - expected}")
    if channel == "stable":
        previous = previous or previous_stable(version, source)
    elif channel != "nightly":
        raise ValueError(f"Unrecognized channel: {channel}")
    if previous and not ancestor(previous, source):
        previous = ""
    lines = [
        f"Razor-Crest {'Nightly' if channel == 'nightly' else 'Stable'} firmware based on CrossPoint {base}.",
        "",
        f"**Version:** `{version}`  ",
        f"**Source commit:** [`{source[:7]}`](https://github.com/{repo}/commit/{source})  ",
        "**Channel:** " + ("Nightly (development build; not a stable release)" if channel == "nightly"
                           else "Stable"),
        "",
    ]
    if notice:
        lines += [f"**Release update:** {notice}", ""]
    lines += ["### Firmware downloads", "",
              "| Device | Firmware asset | Size |", "| --- | --- | ---: |"]
    for device, label in DEVICE_LABELS.items():
        filename = f"razor-crest-{version}-{device}.bin"
        size = assets[filename].stat().st_size / (1024 * 1024)
        lines.append(f"| {label} | `{filename}` | {size:.2f} MiB |")
    lines += ["",
              "Choose only the image built for your device. BOOX Picco uses USB app-only "
              "installation and does not support on-device OTA/SD firmware updates.",
              "",
              "### Changes since " + ("previous Stable" if channel == "stable" else "previous Nightly"),
              "",
              changes(previous, source, repo),
              "",
              "SHA256SUMS.txt contains checksums for the firmware assets. "
              + ("release-manifest.json documents the Stable release."
                 if channel == "stable" else "latest-nightly.json documents the Nightly release."),
              ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", choices=["stable", "nightly"], required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--base-version", required=True)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--previous", default="")
    parser.add_argument("--notice", default="")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        parser.error("Invalid GitHub repository")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source):
        parser.error("Source must be a full commit SHA")
    if args.tag != (f"razor-crest-v{args.version}" if args.channel == "stable"
                    else f"rn-v{args.version}"):
        parser.error("Release tag/version mismatch")
    args.output.write_text(build_notes(args.channel, args.version, args.tag, args.source,
                                       args.repository, args.base_version,
                                       list(args.dist.glob("razor-crest-*.bin")),
                                       args.previous, args.notice), encoding="utf-8")


if __name__ == "__main__":
    main()
