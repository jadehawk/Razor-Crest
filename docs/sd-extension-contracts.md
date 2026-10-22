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

## Separate SD package contracts

| Kind | Install root | Required metadata | Contents |
|---|---|---|---|
| Plugin | `/.crosspoint/plugins/<id>/` or supported legacy roots | `manifest.json` / `device.json` as applicable | `plugin.js` (browser code), `device.json` (declarative on-device catalog), README/assets |
| Theme | `/.crosspoint/themes/<id>.crosspoint-theme/` | `kind: "theme-pack"`, `themes/<id>/theme.json` | bounded monochrome CPB bitmaps, optional CPFONT UI fonts, declarative regions |
| Wallpaper pack | `/.crosspoint/wallpapers/<id>.crosspoint-wallpaper/` | `kind: "wallpaper-pack"` manifest | uncompressed BMP images and README; no CPB/theme/plugin scripts |

Publish each collection in its **own GitHub repository** with a stable
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
| `theme.fonts.v1` | Four optional UI font roles, restoring stock fonts |
| `theme.settings-navigation.v1` | List/Tabs theme preference, subject to user override |

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

## Future catalogs and compatibility

A catalog must **hide the Themes section** without `theme.sd.v1`,
and hide **Wallpaper Packs** without `wallpaper.packs.v1`. A theme also
requires all of its `themes[].requires` strings. Existing
`catalog.lists` / `catalog.compatibility` provide generic
infrastructure, but a supported theme/wallpaper installer and automatic
catalog integration **have not been implemented**. No firmware name is
a substitute for a capability check.

X3 firmware and HALO/wallpaper work was hardware-tested by its author.
Other devices, stock CrossPoint, and existing public releases must not
be assumed compatible until separately verified.
