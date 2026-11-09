# Razor-Crest Nightly updates

Razor-Crest offers independent **Stable** and **Nightly** OTA checks. Stable
continues to use the latest published stable GitHub Release. Nightly uses the
compact, public feed at
`https://jadehawk.github.io/Razor-Crest/latest-nightly.json`.

## Initial setup

Enable GitHub Pages in `jadehawk/Razor-Crest` under **Settings → Pages →
Build and deployment → Source: GitHub Actions**. The Nightly release workflow
checks this before starting builds so it does not upload firmware without a
working update feed. The firmware files are hosted on GitHub Releases. Pages
hosts the JSON update feed, while the Pages root redirects to
[the Razor-Crest Firmware Flasher](https://flasher.razor-crest.com/).

## Publication

The existing `.github/workflows/razor-crest-release.yml` handles both channels
with the same host test suite, seven-board firmware build matrix, pinned
PlatformIO setup, and binary naming. Nightlies run at midnight
`America/New_York` (EST/EDT); GitHub Actions may queue a scheduled run.
The workflow can also run manually with channel **Nightly**, including an
option to force a rebuild of an unchanged commit.

Successful Nightlies use the version `<base>n-<7-char commit SHA>`, for
example `2.0.0n-a1b2c3d`, with GitHub prerelease tag `rn-v<version>`.
A new commit produces a new Nightly version; unchanged commits are skipped.
Firmware is published only when every board and the host tests pass. A JSON
feed and checksums identify the exact version, source commit, firmware
filenames, sizes, and download URLs. The first successful commit-based
Nightly retires the old date-numbered prereleases. The latest seven
commit-based Nightlies are retained.

The combined X3/X4 image uses `x3-x4`. Picco does not support OTA on the
current stock layout and is not in the Nightly matrix.

## Device update flow

Choose **Check for updates → Stable / Nightly / Cancel**. Only the selected
feed is checked. If a newer compatible firmware exists, the existing OTA
confirmation popup shows **Release: Stable** or **Release: Nightly** and keeps
**Update** selected by default. The existing HTTP download, board-image
validation, OTA partition writer, progress bar, and reboot path are shared.

A Nightly-to-Nightly update compares seven-character commit identities.
Installed date-numbered and `RC.`-prefixed Nightlies can upgrade to unprefixed commit-based
Nightlies; a date-numbered feed cannot replace a commit-based Nightly. Switching
from Nightly to Stable is an explicit channel change; the
confirmation warns that Stable may be older. Ordinary Stable-to-Stable checks
retain existing revision-based comparisons.
