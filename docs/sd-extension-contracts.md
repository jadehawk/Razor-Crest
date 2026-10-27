# SD extensions: capability and package contract (development v1)

This is a capability-based community contract, not an upstream CrossPoint
guarantee. Three different package types share firmware primitives, but they
are not interchangeable. Authors must require the actual exposed firmware
capability, not a firmware or fork name.

## Capability discovery

While File Transfer / Calibre web service is active, firmware exposes
`GET /api/status` with `firmwareFamily`, `firmwareId`, `pluginApi`
(currently 1), and comma-separated `pluginCapabilities`. The definitions
live in `src/util/FirmwareIdentity.h`; the native catalog also supports
`{plugin_capabilities}`. These are real integration surfaces, not new
endpoints. The optional Plugin System toggle applies to plugins, not to the
theme and wallpaper loaders.

For complete publisher contracts, ready-to-copy AI development instructions, capability primitives and automatic catalog eligibility, see [Plugin Hub's authoring guide](https://github.com/crosspoint-reader/crosspoint-plugin-hub/blob/main/docs/BUILD_AND_PUBLISH_EXTENSIONS.md), [HALO's theme and AI contract](https://github.com/jadehawk/halo.crosspoint-theme/blob/main/docs/THEME_CONTRACT.md), and [Star Wars wallpaper contract and AI guide](https://github.com/jadehawk/starwars.crosspoint-wallpaper/blob/main/docs/WALLPAPER_PACK_CONTRACT.md).

**Compatibility (2026-10-22):** stock upstream CrossPoint does **not** implement SD theme or standalone wallpaper-pack loaders. The only established implementation is current development [Razor-Crest](https://github.com/jadehawk/Razor-Crest); do not treat this as an allowlist or assume every Razor-Crest release has the feature. Runtime capabilities are authoritative.

## Separate SD package contracts

| Kind | Install root | Required metadata | Contents |
|---|---|---|---|
| Plugin | `/.crosspoint/plugins/<id>/` or supported legacy roots | `manifest.json` / `device.json` as applicable | `plugin.js` (browser code), `device.json` (declarative on-device catalog), README/assets |
| Theme | `/.crosspoint/themes/<id>.crosspoint-theme/` | `kind: "theme-pack"`, `themes/<id>/theme.json` | bounded monochrome CPB bitmaps, optional CPFONT UI fonts, declarative regions |
| Wallpaper pack | `/.crosspoint/wallpapers/<id>.crosspoint-wallpaper/` | `kind: "wallpaper-pack"` manifest | uncompressed BMP images and declared README/DETAILS metadata; no CPB/theme/plugin scripts |

### Required package files and rejection

**New Plugin Hub catalog ZIP installs for both themes and wallpaper packs
REQUIRE all three metadata files inside the single package folder:**

1. `manifest.json` (identity, version, declared file list)
2. `README.md` (bounded, device-readable plain text)
3. `DETAILS.md` (UTF-8 rich Markdown; catalog details source)

**Omitting any of the three, omitting any declared image/theme asset, or
including undeclared ZIP entries causes the firmware ZIP installer to reject
the package.** The host authoring validator and Plugin Hub publisher must
reject it earlier. The wallpaper installer requires exactly `images.size()
+ 3` ZIP entries: a 19-image pack therefore needs **22 files**, with no
exceptions for new published packages. The expected destination is
`/.crosspoint/wallpapers/<id>.crosspoint-wallpaper/` and the ZIP must have
a matching single directory prefix. After a successful network transfer,
a rejected ZIP may still surface as **“Download failed”**—consult `[EXT]`
logs for the validation reason. This rule is enforced by
`src/util/ExtensionPackages.cpp`; host regression checks live in
`test/extension_packages/`.

**Existing manually copied wallpaper folders may still be discovered using
the older two-metadata layout.** That is a migration allowance in the
on-SD registry, *not* acceptance of incomplete newly downloaded ZIPs.

### Wallpaper uninstall and missing-pack fallback

Wallpaper collections, unlike SD themes, do not hold live UI resources.
**Any installed wallpaper pack may be removed or reinstalled from Plugin Hub,
even when it is selected for Custom Sleep Screen.** If the removed pack was
selected, the firmware clears `wallpaperPackId`, saves the setting, and
uses **All Wallpapers** (`/.sleep` + `/sleep`) thereafter. Removing an
unrelated pack leaves the chosen collection unchanged. If a selected pack
was manually deleted, became corrupt, or is missing when the user returns
to Settings → Display → Sleep Screen → Custom, the picker rescans and falls
back automatically to All Wallpapers instead of showing "Unavailable".
The fallback is persisted only when SD storage is accessible; a temporarily
unmounted SD card must not erase a user's preference.

**SD themes are intentionally different:** an active SD theme continues to
block removal or updating until another theme has been selected, because
its fonts and artwork may still be in use.

**Regular plugins are different.** The Plugin Hub *publication* gate
requires `manifest.json` plus one or both of `plugin.js` and `device.json`,
and every file named in `manifest.files`; it rejects a missing manifest or
both absent entry points. `README.md` and `DETAILS.md` remain optional for
plugins (but mandatory **when explicitly declared**). A plugin manually
copied to SD may be loaded by firmware with only `plugin.js` or
`device.json`; manual firmware loading does not necessarily require a
manifest, README, or DETAILS. **Do not apply theme/wallpaper metadata
rejection to plugins.** See
[Plugin Hub contract](https://github.com/crosspoint-reader/crosspoint-plugin-hub/blob/main/PLUGIN_HUB_CONTRACT.md).

Publish theme collections in individual GitHub repositories. Wallpaper pack repositories
may instead contain up to eight independently installable collections, each
with its own manifest and unique ID, packaged as **separate** install-only ZIP
assets in one release. Multiple variants share the release version and carry
validated `target_resolution` metadata; no ZIP should install other variants.
The device discovers installed packs independently from the repository layout.
No firmware name-based or all-variant automatic installer is implied.

A standalone collection uses the same catalog fields as plugins: `name`, `title`,
`description`, `author`, `version`, and a declared `DETAILS.md` inside its
installable ZIP. Plugin Hub verifies the ZIP contents, renders that Markdown
to safe, versioned HTML, and includes `details.url` / `details.format` in
the catalog. Tap opens details; installation is a separate action. Older
installed wallpaper packs without the new metadata remain usable.

A standalone collection is published with a stable
lowercase ID, a matching manifest version, tag `v<version>`, and a release
archive with only the nested installable directory. Generated previews,
tests, tools, workflow files and editable source media are repository files,
not device payload.

## Plugin runtime primitives

The canonical [SD plugin contract](sd-plugins.md) and
[plugin event contract](plugin-events.md) document the implemented features:

- Browser `plugin.js` uses the supported generic HTTP APIs, including
  `/api/relay`, `/api/crypto`, `/api/fetch`, and `/api/plugin-fs`.
- The browser-executed action queue has
  `POST /api/plugin-jobs`, `GET /api/plugin-jobs/claim`,
  `POST /api/plugin-jobs/complete`, and
  `GET /api/plugin-jobs/status`. Actions run only while a hosting page
  (or `/plugins-run`) is open, **not** as device-side JavaScript.
- `device.json` provides firmware-rendered catalog, authentication,
  browsing, network templates and downloads within the declared vocabulary.
- Firmware-supported events include `reader.open`, `reader.exit`,
  `reader.session`, `book.downloaded`, `sleep.enter`.

Advertised named capabilities include `catalog.lists`,
`catalog.notices`, `catalog.bundle_install`,
`catalog.version_tracking`, `catalog.compatibility`,
`catalog.images`, `catalog.cache`, `catalog.refresh`,
`catalog.item_details`, `stats.native.v1`, `ui.dashboard.v1`
(bounded on-device metric/hero cards, icon assets, book lists, and native progress).
Their exact schema and limits are in [sd-plugins.md](sd-plugins.md) and
[webserver-endpoints.md](webserver-endpoints.md).

**Not implemented:** general on-device plugin JavaScript execution,
arbitrary plugin button shapes, or generic plugin long-press/double-press
bindings. Do not advertise future ideas as present capabilities.

## Theme capabilities

`src/components/themes/ThemePackRegistry.cpp` and the rendering helpers
validate and use these independent names:

| Capability | Firmware primitive |
|---|---|
| `theme.sd.v1` | Bounded collection discovery/selection and Classic fallback |
| `theme.metrics.v1` | ThemeMetrics layout overrides |
| `theme.images.v1` | Bounded streamed one-bit CPB bitmaps |
| `theme.composition.v1` | Declarative named header/home/boot/sleep/hints regions and fill/rect/bitmap/text/line/book-card/progress operations |
| `theme.fonts.v1` | Legacy four-role external UI-font map, restoring the built-in fonts safely |
| `theme.font-family.v1` | Bounded single-`ui` file or four-size `ui` family declaration with demand-loaded glyph caches; requires `theme.fonts.v1` |
| `theme.settings-navigation.v1` | List/Tabs theme preference, subject to user override |
| `theme.book-cover.v1` | Native Home `book-card` cover variant: saved cover BMP, title/author, and genuine progress bar if known; the featured book remains firmware-controlled |
| `theme.menu-decoration.v1` | Optional native Home double-framed button panels and validated decorative footer above the hardware hints |
| `theme.screen-decoration.v1` | One opt-in, one-bit ink-only decorative bitmap in the verified empty lower band of non-reading screens (lists and File Transfer) |
| `theme.screen-background.v1` | Optional PSRAM-only full-screen ink-only underlay beneath native list/File Transfer content, clipped above independently reserved footer artwork |
| `theme.header-back.v1` | Theme-selectable touch Back presentation (`arrow`, `emblem`, `both`) with firmware-owned Back hitbox and per-bitmap optional `_touch` artwork variants |
| `theme.usb-scene.v1` | Optional full-screen graphic USB Drive scene painted before the firmware unmounts the SD filesystem for raw PC access |

The legacy `theme.screen-decoration.v1` region remains a **bounded decoration**: the `screen` region accepts one `bitmap` with `paint: "ink-only"` and `max_height` no greater than 128px. The shared UI list renderer and File Transfer pass only spare lower-screen bounds. It skips crowded/scrolling pages, QR codes, Home, reading, and sleep; text, selection, and control hitboxes are never modified. Theme compatibility is negotiated through `theme.screen-decoration.v1`, not a firmware brand or board name.


The optional `theme.sleep-grayscale.v1` capability makes a theme's existing `assets.sleep_art` gallery directory accept **only native four-tone `.bmp` images** (no mixing with `.cpb`). Put device-sized BMPs under `themes/<id>/sleep/`; the firmware validates the directory and BMP dimensions, rotates a random scene without keeping a filename array, and uses the same four-tone hardware waveform as custom sleep wallpapers. If the panel lacks grayscale, the firmware still renders a black-and-white approximation. Publish separate 528×792 and 480×800 gallery files in their respective ZIPs, with the capability declared in the theme's `requires` list.

The optional `theme.usb-grayscale.v1` capability adds an independent `usb_gray_art` role: a **device-sized 4-tone grayscale BMP** under `themes/<id>/assets/` listed in `manifest.files`. Provide both `theme.usb-scene.v1` and `theme.usb-grayscale.v1` in `manifest.themes[].requires`, retain the existing `regions.usb` single-CPB operation for black/white fallback, and include `assets.usb_gray_art` pointing at the BMP. Firmware displays the BMP via the hardware grayscale planes entirely **before** SD handoff, or uses the CPB if the panel does not support native grayscale. Color source files are converted to grayscale before packing, not stored as PNG. Install a matching 528×792 or 480×800 BMP for each device ZIP. The screen contains no text or translatable strings.

The optional USB Drive scene is image-only: add `"usb_art": "themes/<id>/assets/usb.cpb"` to `assets`, a single `{"op":"bitmap","asset":"usb_art","fit":"contain"}` operation to `regions.usb`, and `theme.usb-scene.v1` to the matching theme's `manifest.json` `requires` list. Include the CPB in `manifest.files` and the package archive. Drawn directly into the existing e-ink framebuffer **before** the SD filesystem is detached, the graphic remains displayed while the computer owns the card. No fonts, text, live status messages, additional framebuffer, or PSRAM are needed. Missing or unreadable theme artwork falls back to an in-firmware transfer icon; USB startup errors keep the firmware's existing error screen. USB disconnect restarts directly to Home without a Loading popup. USB art does not affect ordinary File Transfer/Join Network pages.

The declarative `header` region is used on button and touch UI screens,
including Settings, Library and Plugin Catalog. For pushed touch screens,
firmware maps the leftmost 58 pixels of the themed header to Back; themes
should keep a recognizable emblem in that area. Home does not acquire a
Back target. Firmware draws the battery in the header and preserves native
trailing controls such as Search and Rebuild without replacing the artwork.

Font validation is layered. Firmware validates declared capabilities, SD resource paths,
font sizes and v4 headers before activation and validates glyph entries when
rendering. It does not exhaustively audit every glyph bitmap up front. Theme
authors and future catalog maintainers **must** run the stricter independent
host validator (interval layout, glyph bitmap bounds, role ordering and the
four-size hierarchy) and publish only a successfully validated ZIP, following
the [HALO theme contract](https://github.com/jadehawk/halo.crosspoint-theme/blob/main/docs/THEME_CONTRACT.md#required-font-checks-before-publication).
Do not gate compatibility by firmware branding; use advertised capabilities.

Themes **only describe appearance**. They do not register actions,
intercept gestures, run scripts, access the network, or modify firmware
reading/navigation behavior. Their internal sleep galleries apply to
supported Dark/Light sleep rendering, not to Custom wallpaper packs.

Detailed syntax, bounds and author instructions:
[HALO theme contract](https://github.com/jadehawk/halo.crosspoint-theme/blob/main/docs/THEME_CONTRACT.md).

## Wallpaper-pack capability

`wallpaper.packs.v1` enables `src/components/wallpapers/WallpaperPackRegistry.cpp`
to discover bounded, manifest-validated BMP collections. The scrolling
**Wallpaper Collection** selector appears immediately below **Sleep Screen**
when Sleep Screen is **Custom**. A named pack is a random BMP source for
regular Custom sleep and timed refresh; the default **All wallpapers**
preserves `/.sleep` plus `/sleep` (and ordinary `/sleep.bmp` priority).
Selecting a pack never changes the active UI theme. PNG/JPEG are authoring
inputs that must be converted to BMP first; CPB is not required.

**Live virtual collection:** when the installed and enabled Reading Statistics
plugin exposes `dashboard.source: reading-stats.v1`, the selector additionally
offers **Reading Statistics**. This source is not a wallpaper pack or
an authored bitmap. Firmware uses the installed plugin's current V1 manifest
and renders page 1 from native statistics at every normal sleep entry, without
dashboard navigation controls. Missing or disabled plugins fall back to
ordinary Custom wallpapers. Cover + Custom honors the selected live source;
Quick Resume remains unchanged. With Custom timed refresh enabled, the
selected source is redrawn on every scheduled wake: wallpaper packs rotate BMPs,
while Reading Statistics redraws Overall Statistics from saved native data, then
returns to deep sleep. Its sleep image omits navigation header and buttons;
the centered page title remains. Future providers (weather/calendar) require
their own bounded, capability-gated refresh implementation.

The independent authoring reference is
`crosspoint-wallpaper-template/docs/WALLPAPER_PACK_CONTRACT.md`.
Support for `/.sleep` folder collections and timed rotation of theme-only
galleries is still deferred.

## Capability-gated Community catalog (firmware integration in testing)

Plugin Hub v0.1.10 adds an optional `browse.capability_lists_url` pointing to
`catalog-lists-capabilities.json`. This field is ignored by older firmware;
the original `browse.lists_url` and plugin-only catalogs remain unchanged.

The v2 index provides `kind` (`plugin`, `theme-pack`, `wallpaper-pack`),
`items` (the JSON array key), and `requires` (all-of firmware capabilities).
Only supported lists appear. If the new index is unavailable over HTTP, the
firmware falls back to the legacy list URL. Themes require the declared
`themes[].requires` capabilities; unknown requirements are rejected.

The device chooses **both wallpaper and theme variants** using measured display
pixels, never model names. A theme is one collection with a catalog `variants[]`
array containing the exact `target_resolution`, release URL, SHA-256, and byte
count for each publisher-provided layout. Each ZIP's `manifest.target_resolution`
must also match the panel. Only that device's asset ZIP is downloaded.

A release asset named `<name>-x3-<version>.crosspoint-theme.zip` still unpacks
under `<name>.crosspoint-theme/` with a manifest `name` of `<name>`; theme
IDs, installed theme preference and shared font metadata stay stable. An X4+
variant of the same theme contains only its 480x800 CPBs, not X3 artwork.
The installer verifies the checksum, manifest name and resolution, bounded
sizes and exact ZIP member paths before an atomic swap. Wallpaper BMP dimensions
are verified separately. No migration or duplicate installed theme ID is needed.

The installed extension picker retains OPDS and Plugin Hub pinned at the top.
A shaded **Plugins** section groups any remaining installed plugins; separate
**Themes** and **Wallpaper Packs** sections appear only when installed.
Divider rows are visual-only and skipped by button navigation, including holds.
Theme packs with multiple compatible internal themes appear once per package.
Each row displays the installed version and opens a read-only details page with
author and the publisher's manifest `description`, or a direct 2 KiB README
fallback. The firmware **does not parse or sanitize Markdown/emoji**; the
publisher must ship concise printable ASCII package READMEs and short titles.
Theme selector labels come directly from `themes[].title`, without joining in
the collection title. No `device.json` is used for these extension types. A one-second press-and-hold on a package row
offers removal, with confirmation. Active themes and selected wallpaper packs
cannot be removed or overwritten until a different selection is made in Settings.
The online Community catalog also shows **Installed** or **Update Available**
based on the matching local manifest, including the portrait-resolution-specific
wallpaper ZIP that is compatible with this display.

**Hardware verification:** X3 plugin and extension catalogs have been exercised, but the complete theme/wallpaper installation/update/removal path still requires final device validation. The firmware integration is unreleased; never assume upstream CrossPoint support or equivalent feature availability on untested devices. No firmware name is
a substitute for a capability check.

X3 firmware and HALO/wallpaper work was hardware-tested by its author.
Other devices, stock CrossPoint, and existing public releases must not
be assumed compatible until separately verified.
