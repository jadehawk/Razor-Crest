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
| Wallpaper pack | `/.crosspoint/wallpapers/<id>.crosspoint-wallpaper/` | `kind: "wallpaper-pack"` manifest | uncompressed BMP images and README; no CPB/theme/plugin scripts |

Publish theme collections in individual GitHub repositories. Wallpaper pack repositories
may instead contain up to eight independently installable collections, each
with its own manifest and unique ID, packaged as **separate** install-only ZIP
assets in one release. Multiple variants share the release version and carry
validated `target_resolution` metadata; no ZIP should install other variants.
The device discovers installed packs independently from the repository layout.
No firmware name-based or all-variant automatic installer is implied.

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
`catalog.item_details`, `stats.native.v1`, `ui.dashboard.v1`.
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

The device chooses wallpaper variants by the renderer's actual display pixels,
never model names. Each collection is independently installable, and only one
matching release ZIP is selected per collection. Downloads verify SHA-256,
manifest package identity, exact ZIP members, bounded sizes, and BMP dimensions.
Theme packs and wallpaper packs are staged and installed under their separate
SD roots with rollback on failed directory replacement.

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
