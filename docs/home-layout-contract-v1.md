# Home Layout V1 — proposed SD theme and widget contract

**Status:** Stage 1 design specification, NOT YET IMPLEMENTED in firmware or Theme Studio. This contract can be source-controlled; implementation tracking stays local and outside Git.

## Goal

Razor-Crest renders a theme-controlled **weighted row layout**, where each row independently contains 1–4 weighted columns. Each column owns one widget. Grid geometry is resolved once, then widgets paint and receive input within their own rectangles. Theme Studio offers editable presets or an empty grid and exports installable SD theme ZIPs. No device-side editor, JavaScript, recursive layouts or plugin-executed code.

**Painter order:** base fill → optional Home wallpaper/backdrop (not a widget) → widget panels and content → firmware-owned status, focus and hardware hints. A widget may request `panel: "opaque"` or `panel: "transparent"`. An opaque panel must cover underlying artwork; transparent content must not erase the wallpaper during a refresh. Firmware-owned status/hints and required touch targets are never covered by arbitrary theme art.

The first row MUST be one `native.header` widget containing the native-themed header; the firmware draws its own clock/battery within its reserved top band. The lower non-touch hardware-button hint area is excluded from the allocatable canvas, so the theme may place its Snoopy footer in its own row above the hints. A layout never creates two header/status strips.

## Versioned capabilities and files

Proposed new tokens (NOT advertised by current firmware):
- `theme.home-layout.v1` — bounded Home layout parser, geometry, native widget support and safe fallback.
- `home.widgets.v1` — bounded allowlisted native/provider widget registry and input dispatch.
- `theme.home-background.v1` — optional full Home wallpaper CPB; required only for `background.mode: "image"`, not for plain solid/none.

An installable theme keeps its `theme.json` `schema_version: 1` and adds a field
`"home_layout": "themes/<theme-id>/home-layout.json"`. The path is rooted in the same theme collection, uses the same safe-path checks as other theme assets, and must be listed in `manifest.files`. `manifest.themes[].requires` includes the versioned layout/widget capabilities and any other used theme capabilities; if an image backdrop is used, its CPB asset must likewise be declared. Unknown capability tokens must be rejected until supported. Theme Studio must not publish a ZIP claiming these are implemented before firmware provides them.

Proposed **separate Home layout JSON** maximum: **8,192 UTF-8 bytes**, JSON nesting limit **8**. Keep the current `theme.json` and theme manifest 12,288-byte limits, theme registry 64-file limit and extension installer size limits unchanged. Runtime parsing should produce bounded POD-like descriptors and release the JSON tree before drawing. No duplicate full-screen framebuffer or mandatory PSRAM.

### Initial low-memory envelope (2026-11-16 serial baseline)

The **X3 is the limiting device**: its Snoopy-active runtime reported 109,384 bytes free internal heap, **48,980 bytes minimum-ever free**, 90,100-byte largest block, and **1,016 bytes minimum unused render-task stack**. The Picco reported 144,080 bytes free internal heap, 128,616 bytes minimum-ever free, plus ~5.54 MB free PSRAM. X3 has **no reported PSRAM**. These snapshots are not a full worst-case test and do not establish firmware binary headroom.

**Provisional engineering targets, to verify rather than claim achieved:** at most **2 KiB additional retained internal heap** for bounded layout/descriptor state and **8 KiB additional transient internal heap** for parse/render operations above an equivalent baseline; no added large per-widget allocations or independently cached bitmaps; no recursion; budget **≤256 bytes of additional render-call stack use** for grid logic because X3's measured render stack margin is only ~1 KiB. Record before/after minimum-ever heap, largest allocatable block and render stack high-water under worst-case Home, plugin, and wallpaper scenarios; fail the performance gate if the new renderer approaches stack exhaustion or introduces a significant unexplained reduction in heap margin. These figures are **design goals, not proven limits**. Stage 2–4 implementation may need streaming/compact parsing: an 8 KiB input string *plus* an in-memory JSON tree must not silently exceed this envelope.

A native one-bit full-frame 528×792 X3 image is approximately **52,272 bytes** of pixel data; on Picco 480×800 it is **48,000 bytes**. Home wallpaper therefore must use device-prepared CPB with row/stream rendering, not a second full-screen heap buffer; preserve the existing CPB validation, image clipping, and an opaque/ink-only drawing policy. Picco's PSRAM is an optional optimization **only**, never a prerequisite for installing/using Home backgrounds on X3. Keep 8 rows / 4 columns per row / 16 widgets as **provisional descriptor bounds**, pending measured host/device footprint and complete input-navigation tests.

## Bounds and document structure

Initial limits (subject to Stage 0 X3 resource measurements):
- Rows 1–8; **independently** 1–4 columns in each row; at most 16 total widget instances including header/footer. The backdrop is not a widget.
- No nested rows/columns or spanning in V1. These may be explicitly versioned later.
- Row and column weights: integers 1–10. Pixel minima: integers >=1. Optional maxima must be >= corresponding minima. Row and column gaps: integers 0–32.
- Row IDs unique across the layout; column IDs unique within a row; 1–32 ASCII characters matching `[a-z][a-z0-9_-]*`.
- Unknown keys/types, duplicate IDs, unsupported features and geometry that violates device constraints are errors, not silently ignored.

Example of a minimal valid root:

~~~json
{
  "schema_version": 1,
  "background": {"mode": "solid", "color": "white"},
  "row_gap": 0,
  "rows": [
    {
      "id": "header", "weight": 1,
      "min_height": 62, "max_height": 62, "column_gap": 0,
      "columns": [{
        "id": "main", "weight": 1, "min_width": 1,
        "widget": {"type": "native.header", "panel": "opaque"}
      }]
    }
  ]
}
~~~

Required top-level: `schema_version: 1` and `rows`. Optional `background` (defaults to `{"mode":"none"}`) and `row_gap` (defaults to zero). Required row keys: `id`, `weight`, `min_height`, `columns`. Optional: `max_height`, `column_gap`. Required column keys: `id`, `weight`, `min_width` and `widget`; optional `max_width`. Missing maxima mean unbounded inside the Home canvas. A native/provider widget may enforce a larger minimum than the author declared.

**Row sizing:** optional `size_mode` is `weighted` (default), `fixed` or `content`. A `fixed` row MUST declare `max_height == min_height`, reserving exactly that height. A `content` row MUST declare `max_height`; for V1 it contains exactly one `native.shortcuts` widget and requests its natural height from installed/enabled menu entries using the theme's row height and spacing, clamped between its minimum/maximum; overflow scrolls. Firmware measures content once on Home entry/plugin changes, not on every paint. Allocate fixed and content requests first, shrink content toward its minimum if the available height is tight, then distribute remaining space among `weighted` rows, respecting each maximum. Never resize individual menu buttons to consume surplus pixels. Columns are always independently weighted within their row, with minimum/maximum widths; this avoids a second dynamic-content measurement pass.

### Supported V1 widget descriptor types (proposed)

| Type | Additional required fields | Renderer behavior |
|---|---|---|
| `native.header` | none | Native-themed header + firmware status-safe composition, first row only |
| `theme.scene` | `scene` | Declared, bounded scene artwork/text ops; no scripting |
| `native.book` | none | Native cover/title/progress and optional single-book carousel |
| `native.shortcuts` | none | Existing scrollable HomeMenuRegistry entries including plugin shortcuts |
| `native.clock` | none | Native clock/date (unknown time uses safe placeholder) |
| `native.metric` | `field` | Allowlisted native statistic (field list needs Stage 3 validation) |
| `plugin.widget` | `plugin`, `widget`, `fallback` | Allowlisted installed plugin provider/data, with deterministic fallback |
| `empty` | none | Blank noninteractive slot |

All widgets may use `panel` = `opaque` (default) or `transparent`. `native.book` may opt into `carousel: true`; `native.shortcuts` uses `source: "home_shortcuts"`, a theme-defined native menu row height and `overflow: "scroll"`. A widget cannot stretch buttons simply because extra height is available. Optional `id` is a stable widget slot identifier.

**Slot assignment categories are implicit in the type**, so no extra mutable binding field is needed: `fixed` = one theme/native scene or native widget; `compatible` = `plugin.widget` whose installed provider is checked and whose absence activates the fallback; `auto-populated` = `native.shortcuts`, whose existing registry dynamically supplies enabled built-in and installed-plugin actions. No plugin can inject new screen geometry or duplicate a required header. Each interactive widget owns its internal focus, but Home traverses the fixed row-major resolved rectangle table and always keeps Settings reachable.

`theme.scene` IDs now map to a bounded, theme-local `home_scenes` object inside the existing `theme.json` (**Stage 3 code, not yet enabled on device**). For example:

~~~json
{
  "home_scenes": {
    "halo.hero": [
      {"op":"bitmap","asset":"home_art","paint":"opaque","fit":"contain"}
    ],
    "snoopy.footer": [
      {"op":"bitmap","asset":"footer_art","paint":"opaque","fit":"contain"}
    ]
  }
}
~~~

The values use the **same validated declarative operations** as existing ThemeScene regions, with the same manifest-declared CPB assets and path checks. Named Home scenes are decorative only; `book-card` is deliberately forbidden there and remains a native separately interactive widget. At most **four** named scenes, **16 operations per scene**, and **48 operations total including legacy regions**. Scene identifiers are 1–32 characters beginning with lowercase ASCII `a-z`, followed by lowercase ASCII letters, numbers, `_`, `-`, or `.`. Invalid names or over-budget declarations fail theme loading. The native registry rejects duplicate insertions; Theme Studio must reject duplicate JSON object keys before export. `ThemeScene` resolves names into compact handles **2–5**, retaining handle **1** for legacy Home art. Missing names resolve to 0 and fail safely on painting. `UITheme::homeSceneHandle` and `renderExternalHomeScene` are the only new native delegates; no new interpreter, arbitrary filename or plugin execution is introduced.

Named `home_scenes` declarations **require** `theme.home-layout.v1` in the package manifest. That capability is **not advertised/supported by the firmware installer yet**; no existing theme is migrated or activated by this code alone. The separate `home-layout.json` parser will resolve `theme.scene` and `scene_underlays` strings to these handles during Stage 4. The existing **12,288-byte theme JSON ceiling** remains unchanged.

### Wallpaper / background

~~~json
{"mode":"none"}
{"mode":"solid","color":"white"}
{"mode":"image","asset":"home_background","fit":"cover","paint":"opaque","focus":[50,50]}
~~~

`mode`: none/solid/image. `color`: white or black (monochrome). Image `asset`: approved named role referencing one declared theme CPB bitmap. `paint`: opaque or ink-only (default opaque). `fit`: cover, contain or native. `focus`: two integer percentages 0–100, default 50/50. If a given runtime fit is expensive, Theme Studio pre-renders the chosen fit/crop into a device-resolution 1-bit CPB, and the device paints it via existing bounded streaming bitmap functionality. It must NOT require full-frame decoding to RAM or mandatory PSRAM. Missing/corrupt backgrounds fall back to solid white.

This is an independent Home feature. Existing `theme.screen-background.v1` is for other non-reading screens and cannot be treated as existing Home wallpaper support. Theme Studio controls the image, position, crop and optional device-specific variants separately from the grid editor. Different widgets can reveal or conceal the wallpaper using their panel settings. Background covers the Home canvas behind the header and grid; firmware status and non-touch hints stay readable. **V1 supports one canvas-wide wallpaper** plus bounded *optional scene underlays* described below, not independently stacked wallpaper systems.

### Scene underlays that span adjacent rows (Halo parity)

Optional top-level `scene_underlays` contains **at most two** non-overlapping entries such as `{"scene":"halo.hero","from_row":"hero","through_row":"book"}`. Each references an existing theme scene; both endpoints must identify **adjacent-or-contiguous full-width one-column rows**, in forward order. A scene underlay is drawn exactly once behind the union of those rows, after the wallpaper but before the rows' independent widgets. It is **not** a widget and has **no focus or touch region**. Native book cards within those rows can have transparent panels so Halo's image/date/outline composition survives without cut seams. A scene underlay cannot cross into the header/status strip, non-touch hint lane, or decorative footer; all painting must clip to the resolved union rectangle. Missing assets fail to a neutral background, not broken input geometry. These bounded underlays reuse existing declarative scene operations and streamed CPB—not a second full-canvas buffer or recursive layout.

## Resolver invariants and errors

1. Derive usable Home canvas from actual display pixels, safe insets, reserved hardware hints and native status/header ownership. Require exactly one `native.header` in the first row (one column), with enough height for firmware status.
2. Satisfy row minima, maxima and row gaps, then distribute remaining height by weights subject to maxima. Do the same independently for columns within each row. Use deterministic integer rounding (no overlap or unexpected gaps).
3. Reject root/row/column configurations that cannot fit, rather than allowing widgets to draw over footer/status/hints. Validate strong provider/widget minimum sizes. Studio errors should identify the row/column/requirement.
4. Produce one bounded rectangle table, reused by draw, touch hitboxes, focus, menu scrolling and refresh clipping.
5. Widget `overflow` is clip by default; scroll only for supported native interactive widgets. Empty/hidden slots do not consume extra rows or steal adjacent/footer space.
6. Missing optional plugin data selects a declared slot fallback; an invalid whole layout selects built-in safe Home. No unreachable Settings button or corrupt navigation state.
7. Refresh background first only as necessary; partial widget rerenders must restore the correct underlying wallpaper and maintain legible focus/text.

### Plugin contract

A compatible installed plugin MAY add `home_widgets` next to its existing `home_shortcut`:

~~~json
{
  "home_widgets": [
    {"id":"reading_snapshot","type":"native.reading_snapshot","min_width":180,"min_height":100}
  ]
}
~~~

A theme slot may request:

~~~json
{"type":"plugin.widget","plugin":"reading-statistics","widget":"reading_snapshot","fallback":{"type":"empty"}}
~~~

Firmware accepts a provider only when the plugin is installed/discoverable, package identity matches, required capabilities are advertised, its widget type/data source is allowlisted and its minimum size fits. The plugin supplies **data and a descriptor**, never executable native code or device scripts. Missing/disabled/uninstalled provider falls back within the same rectangle. The original plugin dashboard shortcut remains available in the native Home Buttons list.

## Stage 1 fixtures and tests

Reference fixtures under `test/fixtures/home_layout_v1/`:
- `halo.json`: 1×4 weighted rows with a bounded `scene_underlays` Hero-to-Book span and a separately interactive transparent book card (unchanged aesthetic).
- `snoopy.json`: 1×5 weighted rows with distinct footer scene above hints.
- `mixed.json`: independently 1-, 2- and 3-column rows, asymmetric widths, background artwork, optional plugin widget.
- `missing_plugin.json`: missing plugin slot with explicit fallback.
- `invalid_minima.json`: deliberately impossible minimum heights for an X3-sized canvas; MUST fail geometry validation.

Fixture names/scenes express intended semantics; they are not device-installable ZIPs. The separate Home layout parser and HomeActivity integration are **not implemented yet**, although bounded named-scene parsing is now present in the existing ThemeScene loader. A host-only schema test can validate syntax, limits and 528×792/480×800 capacity constraints without advertising the capability.

**Stage 1 design decisions recorded (2026-11-16):** Halo's picture/date/tagline uses the bounded noninteractive `scene_underlays` pass spanning Hero+Book; the book card remains independently interactive with a transparent panel. Scenes reuse the existing declarative `ThemeScene` primitives/CPB assets, with bounded name-to-handle registration supplied by Stage 3. A wallpaper/underlay has no input target; focus and hitboxes are owned only by widgets. The proposed **8 rows / 4 columns per row / 16 widgets** are V1 *host-enforced ceilings*; they are deliberately conservative against the measured X3 ~49 KiB heap low-water mark and ~1 KiB render stack margin, but need device validation after integration. The full theme JSON plus separate layout parser must respect the recorded transient heap budget. Binary-size headroom remains an independent Stage 0/9 measurement, not a reason to block host-only geometry. Do not advertise any capabilities before Stage 3–4 runtime implementation.

## Stage 2 isolated geometry implementation (not wired into Home)

`src/util/HomeGridGeometry.h` is a C++17 pure geometry resolver. It accepts caller-owned `home_grid::Definition` and `home_grid::Result`, and `Screen` safe-area margins supplied by firmware. No SD access, JavaScript, background rendering, graphics allocation, or HomeActivity changes. Both descriptor and resolved output have compile-time storage bounds (**512 / 256 bytes maximum**, respectively) and the scratch track array is bounded to eight small records. A `Status` result fails closed on invalid canvases/tracks, minimum-size conflicts or too many columns.

The resolver reserves safe margins, then satisfies all row minima, honors fixed rows, grants content-sized shortcuts their clamped preferred height, and distributes remaining height to weighted rows subject to maxima. Each row independently divides its width among 1–4 columns with deterministic largest-remainder pixel rounding. If every expandable row/column hits its maximum, excess space remains unallocated rather than stretching controls. It returns one stable rectangle table for future painting, navigation, touch and clipping. `spanRows` returns the clipped geometry union of a permitted interior scene underlay.

Host regression command: `python test/test_home_grid_geometry.py`. Tests compile the *actual production header* in a standalone host C++ program and exercise Halo/Snoopy, mixed splits, device-safe bounds, underlays, min/max, rounded weights, invalid inputs, the 8/4/16 limit and 500 deterministic stress layouts. **Host success does not establish on-device memory use, binary size or visual fidelity.** The fixture/schema test remains `python test/test_home_layout_contract.py`.

## Stage 3 widget compositor and native surface (partial; not yet integrated)

`src/components/home/HomeWidgetEngine.h` now implements a bounded, script-free widget plan, compositing order, optional background (white/black/device-prepared CPB), cross-row scene underlays, opaque/transparent widget panels, safe optional-provider fallback, dirty-area backdrop replay, focus traversal and hit-test using the **same resolved rectangles**. The plan is capped at 320 bytes. A parsed `panel: "opaque"` maps to the theme's **white or black** native panel style; it is not a raw RGB/alpha framebuffer. Transparent panels skip clearing the artwork behind them.

`src/components/home/HomeWidgetNativePaint.h` is a GfxRenderer adapter that saves/restores the incoming clip, fills bounded regions, and delegates CPB painting to the existing `themebitmap::draw` streaming/CRC-validated bitmap implementation; the native paint surface is bounded at 128 bytes. **`src/components/home/HomeWidgetThemeAdapters.h`** now maps the compositor's native Header, Book and Home Buttons requests to the existing `UITheme::getTheme().drawHeader`, `drawRecentBookCover` and `drawButtonMenu` methods. It also delegates the original SD Home background/BookCard phases to `UITheme::renderExternalScene`, keeping the foreground BookCard's geometry anchored to the shared Hero+Book scene while drawing only within the Book widget's clip. Its non-owning view passes existing cover-cache state, carousel book index and visible menu label/icon callbacks without recreating GUI styling. This is host-tested with API-matching stubs but **not yet wired into HomeActivity or compiled as firmware**. The named Home-scene registry now resolves valid theme-local IDs into handles 2–5, retaining handle 1 for the legacy Home region. The separate layout parser will bind those handles in Stage 4; the new capability remains unsupported until integration. **`src/components/home/HomeWidgetReadouts.h`** adds native Clock and Metric presentation using fixed 40-byte UTF-8-safe display buffers, existing `UI_12_FONT_ID`, explicit native-only data callbacks and an allowlist of seven metric IDs. Clock has an `--:--` placeholder when RTC data is unavailable, metrics show `NO DATA`/`--` without guessing values, and missing fonts or invalid IDs fail closed. A black panel gets white ink; opaque-white/transparent panels default to black ink. Studio must provide a high-contrast panel or validated foreground text policy for dark imagery before enabling transparent metric widgets. The Home Buttons adapter also bounds requested visible rows using the current theme's `getMenuRowHeight`, preserving native chamfer/outline drawing without stretching. The formatter is host-tested, **but there is no live RTC/statistics source connected to HomeActivity yet**. Plugin rendering, provider discovery, touch/focus integration and native chrome remain pending. `background.focusX/focusY` is retained only as crop metadata: Theme Studio must pre-bake focal-point crop into a resolution-specific CPB before export; firmware does not dynamically resample focal positions.

Host verification is `python test/test_home_widget_engine.py`, compiling the production compositor and native adapter headers under both GCC and Clang with stub GfxRenderer/CPB calls. Tests cover broken/missing CPB and scene fallback, paint-order, panel opacity, clip restoration, partial refresh and input/focus geometry. **This alone does not enable `theme.home-layout.v1`, `home.widgets.v1` or `theme.home-background.v1` in current firmware**. Those tokens must remain unsupported until real native callbacks, parsing, and device validation are done.

## Stage 4 activation preflight (host-only groundwork)

`src/components/home/HomeGridActivation.h` now exposes a bounded, allocation-free, fail-closed `prepare()` call for a proposed parsed layout and widget plan. It requires explicit installed capabilities, rejects an image background when the background capability is absent, resolves device-specific safe-area geometry, validates widget sizing and scene handles, and insists on exactly **one native Home Shortcuts widget** so that Settings remains reachable. On any failure, the caller-owned active layout is reset and the existing HomeActivity path must remain authoritative. A valid result returns the same resolved rectangles for future rendering, hit-testing and focus.

`test/home_widget_engine/HomeGridActivationTest.cpp` tests valid X3/touch screen envelopes, missing capability/layout, unsupported bitmap background, absent or duplicated shortcuts, invalid scene/fallback handles and impossible geometry. The host runner compiles this with GCC and Clang. **Stage 4 activation is not yet connected to HomeActivity and does not enable any new firmware capability.**

## Stage 4 native Home layout JSON parser (host verified, not activated)

`src/components/home/HomeLayoutJson.h` now converts a version-1 layout JSON document into bounded `home_grid::Definition` and `home_widgets::Plan` structures using the **actual ArduinoJson 7.4.2 API**. The cold-path `parseFile()` accepts the existing HalFile methods (`fileSize64`, `read`, `read(void*,size_t)`) through a small adapter. `parseStream()` enforces an **8,192-byte** limit and nesting depth of 8, rejects trailing garbage, and uses a fixed-size raw-key guard to reject duplicate JSON object keys before ArduinoJson overwrites them. Explicit null values, unsupported properties, invalid IDs, dimensions, missing scene/bitmap handles, and invalid underlays fail closed. Only bounded native descriptors remain after the temporary JSON tree is destroyed.

Names for decorative scenes, CPB wallpaper roles, and optional plugin providers are resolved only through **trusted native callbacks**; the parser cannot execute theme or plugin code. A missing plugin receives a fallback inside the same rectangle, including a separate optional fallback panel that may reveal the wallpaper. The selected-theme manifest and declared file/asset identity checks are now enforced in `ThemeScene` before opening the layout, and HomeActivity performs one cold-path preflight. The parser itself does not open arbitrary file paths or advertise new capabilities.

`test/home_widget_engine/HomeLayoutJsonTest.cpp` uses real ArduinoJson and a simulated HalFile source. It covers Halo/Snoopy/mixed/optional-plugin fixtures on X3 and Picco-sized canvases, malformed/oversized/truncated input, duplicate keys, explicit nulls, invalid resource references and fallback behavior; GCC and Clang strict C++17 tests pass. **Actual firmware compilation, on-device performance, peak JSON parse heap, and Home visual/navigation validation are still pending.**

### Stage 4 grid painting and input plumbing (host-tested, device validation pending)

`HomeActivity::renderGridHome()` now passes the retained descriptor, preflight geometry and existing UITheme native adapters to the bounded compositor, then draws firmware-owned button hints and refreshes the same framebuffer. The compositor paints the wallpaper first, decorative cross-row underlays second, and native book, clock, metric and menu widgets afterward. It reuses the legacy theme's footer-art renderer outside the menu clipping rectangle, in the independently protected footer lane. Book-only cached cover pixels are restored **after** wallpaper painting within the resolved Book rectangle, without a duplicate framebuffer. Because the existing `GfxRenderer::copyBufferToRegion` copies complete physical bytes (including up to seven pixels outside a rectangle's edges), **cache saving and restoring is disabled for split-column and rotated Book widgets**. Those cards redraw with the existing native cover renderer on each refresh until masked-edge restoration is supported; full-row portrait Book widgets retain their fast snapshot path. This avoids corrupting neighboring widgets but may increase SD read/paint work for split layouts.

`src/components/home/HomeGridInteraction.h` maps exactly one scrolling Home Shortcuts widget and at most one Book card to the **same resolved rectangles** used for rendering. Touch hitboxes, carousel-swipe region and menu-scroll touch bounds now follow those rectangles when a validated layout is active. Physical buttons keep the existing `ButtonNavigator`, `SingleCardPager` and `HomeMenuRegistry` logical navigation; the grid changes viewport capacity, **not the action mappings**. Unsupported multi-card focus models, incompatible native row heights and missing Book/menu rectangles fall back to legacy Home. If painting fails, the grid state and cached book image are discarded and the legacy screen is repainted. The existing SD theme installer still rejects new V1 capability tokens, so this path is **not user-enabled** and has **not been firmware-compiled, flashed or visually verified on X3/Picco**.

Host checks: `test/home_widget_engine/HomeGridInteractionTest.cpp` tests Book-versus-underlay hitboxes, mixed-column geometry, menu gaps, one-visible-row Settings accessibility and viewport bounds on X3/touch geometry. `test/test_home_grid_runtime_wiring.py` guards HomeActivity's shared paint/input integration, physical navigation reuse, cold-only JSON reads and legacy fallback. Both GCC and Clang host suites, the contract/geometry tests and existing Home regressions pass. **Runtime stack budget, header/book/menu visual alignment, footer artwork, partial-refresh ghosting and hardware input behavior remain device acceptance gates.**
