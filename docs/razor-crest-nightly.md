# Razor-Crest Nightly updates

Razor-Crest offers independent **Stable** and **Nightly** OTA checks. Stable
continues to use the latest published stable GitHub Release. Nightly uses the
compact, public feed at
`https://jadehawk.github.io/Razor-Crest/latest-nightly.json`.

## Initial setup

Enable GitHub Pages in `jadehawk/Razor-Crest` under **Settings → Pages →
Build and deployment → Source: GitHub Actions**. The Nightly release workflow
checks this before starting builds so it does not upload firmware without a
working update feed. The site needs only to serve the generated JSON, not
firmware files.

## Publication

The existing `.github/workflows/razor-crest-release.yml` handles both channels
with the same host test suite, seven-board firmware build matrix, pinned
PlatformIO setup, and binary naming. Nightlies run at midnight
`America/New_York` (EST/EDT); GitHub Actions may queue a scheduled run.
The workflow can also run manually with channel **Nightly**, including an
option to force a rebuild of an unchanged commit.

Successful Nightlies use the version `<base>-n<YYYYMMDD>.<run>` and GitHub
prerelease tag `rn-v<version>`. Firmware is published only when every board
and the host tests pass. A JSON feed and checksums identify the exact
version, source commit, firmware filenames, sizes, and download URLs.
The latest seven Nightly prereleases are retained.

The combined X3/X4 image uses `x3-x4`. Picco does not support OTA on the
current stock layout and is not in the Nightly matrix.

## Device update flow

Choose **Check for updates → Stable / Nightly / Cancel**. Only the selected
feed is checked. If a newer compatible firmware exists, the existing OTA
confirmation popup shows **Release: Stable** or **Release: Nightly** and keeps
**Update** selected by default. The existing HTTP download, board-image
validation, OTA partition writer, progress bar, and reboot path are shared.

A Nightly-to-Nightly update compares the embedded date and run number.
Switching from Nightly to Stable is an explicit channel change; the
confirmation warns that Stable may be older. Ordinary Stable-to-Stable checks
retain existing revision-based comparisons.
