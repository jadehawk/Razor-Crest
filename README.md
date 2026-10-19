# Razor-Crest — Personal CrossPoint Fork

> [!IMPORTANT]
> This repository is a **personal fork of CrossPoint Reader**. It tracks the upstream `develop` branch, but it also carries firmware behavior and plugin-system work that is **not currently part of the baseline CrossPoint `develop` branch**. It is not an official CrossPoint release and should not be treated as an upstream-supported build.
>
> Razor-Crest deliberately keeps the broad firmware family as `crosspoint` for plugin compatibility, while advertising the exact firmware ID `Razor-Crest` so plugins and catalogs can distinguish this fork when needed.

## Public firmware downloads

Razor-Crest publishes device-specific firmware binaries through this fork's GitHub Releases. Release tags use `razor-crest-v<upstream-version>-r<revision>` (for example `razor-crest-v1.6.5-r1`), so the CrossPoint baseline and Razor-Crest revision are both visible.

Each release builds and publishes firmware for every device currently covered by the upstream CrossPoint release matrix:

- X3 / X4
- Seeed reTerminal Sticky
- X4 Pro
- X4 Classic
- Paper Mono
- MetalIO EInk4
- EEGO A4

Release assets use names such as `razor-crest-1.6.5-r1-x3-x4.bin`. Use only the binary intended for your device. Each release also includes `SHA256SUMS.txt` and a machine-readable `release-manifest.json` containing the source commit, upstream baseline, firmware identity, and asset mapping.

These are Razor-Crest fork builds, not official CrossPoint releases.

Timed Sleep-Screen Refresh is now part of the standard `razor-crest` firmware and release channel; it no longer requires a separate experimental branch or timed-wake release workflow. On X3, enabling a new timed wallpaper interval displays a warning with the estimated **full-battery, wallpaper-only** runtime; it does **not** include reading or other daily use. See [Timed wake testing](docs/timed-wake-testing.md) and the [X3 battery drain measurements](docs/x3-timed-refresh-battery.md).

## Current Razor-Crest-only additions

The list below describes the current **net differences from upstream `develop`**. Intermediate experiments that were later removed or superseded are intentionally not listed.

### Razor-Crest identity and branding

- Adds a formal firmware identity contract with:
  - firmware family: `crosspoint`
  - exact firmware ID: `Razor-Crest`
  - plugin API level: `1`
  - advertised plugin capabilities: `catalog.lists`, `catalog.notices`, `catalog.bundle_install`, `catalog.version_tracking`, `catalog.compatibility`, `catalog.images`, `catalog.cache`, and `catalog.refresh`
- Exposes firmware family, exact firmware ID, plugin API level, and capability list through `/api/status`.
- Makes the same identity values available to plugin/catalog templates through `{firmware_family}`, `{firmware_id}`, `{firmware_version}`, `{plugin_api}`, and `{plugin_capabilities}`.
- Replaces the stock CrossPoint branding artwork with the Razor-Crest logo in firmware/web assets.
- Uses a larger 200×200 Razor-Crest logo on the boot and default sleep screens and labels those screens with the exact firmware ID instead of the stock CrossPoint name.

### Experimental timed sleep-screen refresh

- Adopts the optional one-shot timer-wake primitive from upstream CrossPoint PR [#3820](https://github.com/crosspoint-reader/crosspoint-reader/pull/3820) without adding a scheduler or new plugin API.
- Adds **Timed Sleep-Screen Refresh** for **Custom** sleep screens, defaulting to Off with 1 minute, 15 minute, 30 minute, 1 hour, 3 hour, 6 hour, 12 hour, and 24 hour intervals.
- Timer wakes perform only local SD/display maintenance: select another valid BMP from `.sleep`, refresh the retained e-ink image, and immediately return to deep sleep without Wi-Fi or plugin execution.
- X3 requires a board-specific power detail because GPIO13 switches its SD-card rail; the rail remains powered only when a timer wake is successfully armed. X3 battery-only functional testing passes, while standby-current measurement remains in progress.
- See [Timed wake testing](docs/timed-wake-testing.md) for the validation matrix and test procedure.

### Global Plugin System control

- Adds **Settings → System → Advanced → Plugin System** as a global enable/disable switch for SD-card plugins.
- New installations default the Plugin System to **Disabled**.
- Firmware upgrades automatically preserve an existing plugin setup by enabling the Plugin System when plugin folders are already present on disk and the setting did not previously exist.
- When enabled, **Plugins** is shown on the Home screen even if no plugin is installed yet.
- Disabling the Plugin System leaves plugin files untouched but stops normal plugin discovery and execution.
- Plugin-backed web endpoints are gated while the Plugin System is disabled, including relay/fetch/crypto, plugin filesystem access, book-key handling, the headless plugin runner, and plugin jobs.
- Internal plugin-system settings are persisted but intentionally hidden from the generic Web Settings API.
- Adds raw on-disk plugin probes for migration/bootstrap/recovery while keeping normal discovery subject to the Plugin System state and installation quarantine.

### Optional Plugin Hub bootstrap

- Adds a synthetic **Plugin Hub — Download, discover, and update plugins** entry under **Home → Plugins** when the Plugin System is enabled but a usable Plugin Hub is not installed.
- The synthetic Hub entry supports **Install**, **Hide**, and **Cancel**.
- **Hide** affects only the synthetic installer row and persists across reboots and Plugin System disable/enable cycles.
- Adds **Restore Plugin Hub installer** under **Settings → System → Advanced** when the installer was hidden.
- The built-in installer downloads the Plugin Hub manifest/device/plugin/README files over Wi-Fi using staged replacement rather than exposing a half-installed plugin.
- Cancelling out of Wi-Fi selection cleanly aborts the Hub install flow and returns to the plugin picker.
- A manually sideloaded Plugin Hub is recognized from any supported plugin root when it has a valid `device.json` and is not quarantined by an in-progress install marker.

### Plugin installation, update, and removal hardening

- Continues to support all three plugin roots: `/.crosspoint/plugins`, `/plugins`, and `/.plugins`.
- Adds `.installing` transaction markers so partially installed plugins are quarantined from normal discovery and file resolution until installation completes.
- Generic bundle updates reuse an already installed same-named plugin directory from any supported root instead of creating a duplicate in the preferred root, preserving that plugin's local settings and tokens.
- Multi-file bundle downloads are staged to `.new` files and swapped into place only after the complete bundle has downloaded successfully.
- Rejects unsafe bundle subdirectories and entries, including traversal, absolute paths, ambiguous backslash paths, and writes outside protected plugin paths.
- Adds plugin uninstall from the plugin picker by holding **Confirm** for one second, followed by an explicit confirmation before recursively deleting that plugin directory.
- Extends confirmation dialogs so callers can provide custom Confirm/Cancel labels.
- Shared plugin HTTP/download helpers now refuse network requests when Wi-Fi is not actually connected instead of attempting a request against a disabled/disconnected interface.

### Generic plugin catalog platform extensions

These capabilities are implemented as **plugin-agnostic firmware features**. Plugin Hub uses several of them, but any plugin/catalog can use the same contracts.

- **Dynamic named catalogs** — supports static `browse.lists` plus remote `browse.lists_url` indexes, allowing catalog groups to be generated or split without rebuilding firmware or the plugin.
- **Per-list notices** — a named list can show an optional notice with custom title, message, Confirm, and Cancel labels before entering that catalog.
- **Bundle installation** — catalog items can install a complete multi-file plugin or supported bundle from a base URL plus a declared file list.
- **Version tracking** — catalog versions are compared with installed plugin manifests so rows can show Installed/Update state.
- **Compatibility filtering** — catalog entries can be restricted by firmware family, exact firmware ID, minimum plugin API, and required capabilities; incompatible entries are omitted while entries without compatibility metadata remain backward-compatible.
- **Additional template values** — catalog templates can consume firmware identity/capability values and item thumbnail URLs in addition to the existing token/config/item fields.
- **Native plugin tabs** — manifests can expose a bounded native tab shell for Browse, Settings, and legacy Cache views.
- **Native plugin settings** — declarative `choice` and `toggle` settings can edit a plugin's existing JSON config file and immediately drive presentation through `{cfg.KEY}` values.
- **Grid/list presentation controls** — manifests can independently configure grid/list page sizes, 1–4 grid columns, 1–4 visible grid rows, list thumbnails, selection style, and rounded preview corners.
- **Native footer pagination** — enhanced catalog UIs use Previous/Next footer actions instead of consuming catalog rows for paging.
- **Catalog refresh action** — manifests can opt into a native Refresh footer action that refetches the current catalog page; it is separate from Previous/Next pagination and also works when only one page exists.
- **Image-backed catalogs** — items can declare thumbnail/image requests using BMP, JPEG, or PNG sources, or `auto` to detect those formats from the downloaded bytes. JPEG/PNG previews are converted to display-ready BMP data on SD and failed previews fall back to the normal placeholder.
- **Full-screen image preview before save** — direct image downloads can opt into a Close/Save preview flow before the final file is published to its destination. Grayscale previews show a Processing state until the final action footer is safe to reveal.
- **Image format-aware publishing** — downloaded image content is signature-checked and can be converted to BMP when the destination requires BMP rather than trusting the remote filename alone.
- **Bitmap upscaling support** — the renderer can optionally upscale bitmap content to a requested target instead of only downscaling, supporting larger native image previews.

### Progressive plugin catalog cache

- Adds an optional per-plugin SD cache for JSON catalog pages and converted preview images.
- Fresh cached pages can open directly from SD without turning on Wi-Fi.
- Cache fill is progressive: only pages the user actually reaches are fetched and stored; browsing past the cached frontier fetches the next missing page and appends it to the cache.
- Catalog-page freshness is configurable, including Always, 3 days, 7 days, and 14 days.
- Cached images are keyed independently from page freshness, so unchanged image URLs can be reused across catalog refreshes.
- If a network refresh fails, an existing stale page can be used as a fallback.
- Per-plugin cache storage is bounded to configurable limits between 8 MB and 128 MB, with image entries evicted before catalog pages while protecting the currently displayed page/images.
- Adds cache statistics and controls for last refresh, cached pages, cached images, storage usage, Refresh now, and Clear cache.
- Cache controls can appear as an expandable section inside a plugin's native Settings tab; the legacy dedicated Cache tab remains supported.
- Supports an optional downloadable cache-seed ZIP for pre-populating plugin image/catalog cache data.
- Search results are intentionally not cached.

### Supporting documentation and development differences

- Expands the SD-plugin documentation for the Plugin System, Hub bootstrap, firmware identity/capabilities, dynamic lists/notices, compatibility filtering, native tabs/settings, image layouts, progressive caching, bundle update behavior, and related manifest schema.
- Expands the User Guide with the new Advanced/Plugin System and Plugin Hub workflow.
- Expands the webserver endpoint documentation with the firmware identity/plugin capability fields returned by `/api/status`.
- Adds/updates plugin manifest classification tests for the extended plugin discovery behavior.
- Adds a repository-local build policy in `AGENTS.md` requiring this workstation's canonical `C:\XP-FW` build environment for firmware builds instead of ad-hoc/direct PlatformIO builds.

---

# CrossPoint Reader

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

### Now running on:

- **ESP32C3-based** Xteink X4 and X3.
- **ESP32S3-based** Xteink X4Pro and X4Classic, Seeed reTerminal Sticky, M5PaperMono, Metalio E-Ink 4, EEGOREAD A4 and Boox Picco.

Check [our Devices page](https://crosspointreader.com/devices) for the full list.

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

> If you're planning to buy an Xteink device, consider purchasing an **X3/X4 Developer Edition** through https://crosspointreader.com. CrossPoint receives a small share of each sale, helping fund development costs.

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, adaptive table layouts, native CJK ruby annotations, chapter navigation, footnotes, bookmarks, dictionary lookups ([StarDict](docs/dictionary.md)), go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more.

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Touch reading**: follow EPUB links and look up words in the dictionary on touch-enabled devices.

- **Highlights and clippings**: highlight text in EPUBs and save excerpts as clippings. Use the reader menu on button devices, or hold a word and drag the selection handles on touch devices. Look up selected words or phrases in the dictionary. Enable Sync Clippings to sync saved excerpts through a compatible CrossPoint Sync server. See [Clippings and Highlights](USER_GUIDE.md#53-clippings-and-highlights).

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 and Sticky)**.

- **Bluetooth page turners**: pair a remote under Settings → Connectivity → Bluetooth Page Turner. Map short and long presses to page turns, chapter navigation, the reader menu, or clipping selection.

- **USB Drive mode (X4Pro)**: access the SD card as USB mass storage.

- **Library workflow**: indexed title/author search, recently-added and alphabetical views, multilingual grouping, folder browser, recent books, and SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download
  - OTA update checks and installs from GitHub releases

- **Customization**: night mode, multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes including transparent overlays, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 34 UI languages and counting, including CJK font fallback and RTL support.


- **SD card plugins**: Optional plugin support can be enabled from **Settings → System → Advanced → Plugin System**. Sideloaded plugins work independently of Plugin Hub; a hidden Hub installer can be restored from Advanced settings. See the [User Guide](USER_GUIDE.md) and [SD-card plugin documentation](docs/sd-plugins.md).

- On-device plugins use firmware-provided screens and operations. Browser-side JavaScript plugins require their browser page to remain open; see the [reading-focused scope](SCOPE.md#out-of-scope).


### Coming soon:

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash CrossPoint.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.

## Install firmware

### Web installer (recommended)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Go to https://crosspointreader.com/#flash-tools, select your device (X3, X4, Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono), and choose an official CrossPoint release.

### Web installer (specific version)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download the firmware file for your device from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases), or compile yourself.
3. Go to https://crosspointreader.com/#flash-tools, select your device, click "Custom .bin" and upload the firmware file.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download the firmware file for your device from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash an X3 or X4:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

   Flash an Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

On devices with external RAM enabled in CrossPoint, copy `.ttf`, `.otf`, or `.ttc` files to the SD card and select them as reader fonts. Put one file in `/fonts/` or `/.fonts/`, or put one family's files in a subfolder. See the [SD card font guide](./docs/sd-card-fonts.md) for the folder layout and styles.

On other devices, convert the font to `.cpfont` first. `.cpfont` files also work on devices with external RAM enabled and have better performance. No firmware reflash is needed to add fonts.

To make `.cpfont` files:

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — UX focused with minimal reading stats and broader customizations for the reading experience.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- [Witch(hunt) Reader](https://github.com/jpirnay/witchhunt-reader) — More faithful CSS styling and background work for slightly snappier interaction. Weather information panel. Markdown support.

**Note:** Many of these features will make their way into CrossPoint over time. Each project chooses its own priorities and tradeoffs.

Want to build your own device? Be sure to check out the [Silkscreen](https://silkscreenreader.com) project or [OnePage Reader](https://github.com/MoveCall/onepage-reader).

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.
