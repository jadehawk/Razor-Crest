# SD-card plugins

CrossPoint keeps service integrations off the firmware. A plugin is a folder on
the SD card; the firmware provides three generic, scheme-neutral surfaces that
plugins compose. Adding or updating a plugin never requires a firmware build,
and the firmware carries no vendor names, URLs, or file-format knowledge.

```
<root>/<name>/
    manifest.json     web UI card metadata (optional): title, description, mount
    plugin.js         browser-side plugin (optional)
    device.json       on-device catalog screen (optional): title, description, ...
    README.md         usage instructions, shown on-device (optional)
    ...assets
```

**Discovery and the on-device list.** The plugin subsystem is controlled by
**Settings → System → Advanced → Plugin System**. When enabled, **Plugins** is
shown on the Home screen even when no plugins are installed. Every plugin folder
(anything holding a `manifest.json`, `plugin.js`, or `device.json`) appears under
**Home → Plugins**, showing its `title` and one-line `description`. Selecting a
plugin with an on-device catalog (`device.json`
with `browse.url`) opens that catalog. Selecting any other plugin shows its
`README.md` (up to 16KB, as plain paged text) when it has one, so even a
browser-only plugin (no `device.json`) can explain how to use it from the web
UI; without a README the row does nothing. `title`/`description` are read from
`manifest.json`, with `device.json` overriding when present.

`<root>` is any of `/.crosspoint/plugins`, `/plugins`, or `/.plugins` — the
first two-dot-free options exist so plugins are easy to copy onto the card
from a computer. All three roots are scanned; on a name collision the earlier
root in that order wins.

For bundle installs that target one of those plugin roots with a single direct
folder name, firmware first reuses an existing same-named plugin directory from
any supported root. This keeps updates in place with their local settings and
tokens. If no installed copy exists, the bundle uses its configured destination
root as before. Nested bundle subdirectories and non-plugin destinations are
unchanged.

During firmware-managed plugin installation, a plugin directory may contain a
`.installing` marker. Normal plugin discovery and file resolution quarantine that
directory until the transaction completes, even if `manifest.json`, `plugin.js`,
or `device.json` is already present. Raw on-disk probes used for migration and
installer recovery can still see the directory. The marker describes transaction
state; it does not change the normal plugin validity contract once removed.

### Enabling plugins and Plugin Hub

The **Plugin System** switch is the global gate for SD-card plugin discovery and
execution. New installations default to **Disabled**. Firmware upgrades detect
existing plugin folders and keep the Plugin System enabled so an upgrade does
not silently disable an existing setup. Turning the Plugin System off does not
delete or rename any plugin files; turning it back on makes them discoverable
again.

Plugin Hub is optional and is not required for sideloaded plugins. The native
Hub is considered available when the first matching `pluginhub` directory has a
`device.json` and is not quarantined by `.installing`; `manifest.json`,
`plugin.js`, and `README.md` are not required for native availability. When the
Plugin System is enabled, no native Hub is available, and the installer has not
been hidden, **Home → Plugins** contains a synthetic **Plugin Hub — Download,
discover, and update plugins** entry. Selecting it offers:

- **Install** — download and install Plugin Hub over Wi-Fi. Backing out of the
  Wi-Fi selector cancels installation, turns Wi-Fi back off, and returns to the
  Plugins picker without changing the Hide preference.
- **Hide** — hide only the synthetic Plugin Hub install entry. Installed and
  sideloaded plugins continue to work normally.
- **Cancel** — leave the entry visible and make no changes.

The **Hide** preference persists across reboots and Plugin System disable/enable
cycles. While the Plugin System is enabled and the Hub installer is hidden,
**Settings → System → Advanced** shows **Restore Plugin Hub installer**. Selecting
it clears only the hidden preference; installed and sideloaded plugins are not
restarted or changed.

For users who prefer manual installation, Plugin Hub can be downloaded directly
from <https://github.com/crosspoint-reader/crosspoint-plugin-hub/releases>. Copy or
extract the `pluginhub` folder so it ends up at one of these supported locations:
`/.crosspoint/plugins/pluginhub/`, `/plugins/pluginhub/`, or
`/.plugins/pluginhub/`.

A plugin can ship any combination: `plugin.js` alone (web-only),
`device.json` alone (on-device only), or both. A `device.json` may declare only
`events` and omit `browse.url`; that is a valid background/events plugin. It is
listed with its title and description (and may document setup in `README.md`),
and selecting it opens that README when present. It has no catalog action and
is never sent into the catalog error flow.

The plugin sources and examples live in the separate `sd-plugins` repository,
which also documents the browser-side `plugin.js` API. This document covers the
firmware surfaces: the job queue and the `device.json` on-device screens.

## Surface 1: browser plugins (`plugin.js`)

JS loaded into the File Manager or Settings web page, backed by generic device
endpoints (`/api/relay`, `/api/crypto`, `/api/fetch`, `/api/plugin-fs`). See
`docs/webserver-endpoints.md` for the endpoints and the sd-plugins repository
for the JS contract.

## Surface 2: the plugin job queue (external automation)

External systems (a companion app, a script) can trigger plugin actions
without a human clicking the web UI. The firmware stores small opaque
`{plugin, action, args}` JSON blobs in a fixed 6-slot pool; it never interprets
them. Any open page hosting the plugin — including the headless runner at
`GET /plugins-run` — claims jobs, executes the plugin's registered handler in
the browser context, and posts the result back.

| Endpoint | Purpose |
|---|---|
| `POST /api/plugin-jobs` | enqueue `{plugin, action, args?}` → `{id}` (503 when the pool is full) |
| `GET /api/plugin-jobs/claim?plugin=` | executor claims the next pending job |
| `POST /api/plugin-jobs/complete` | executor posts `{id, claim, ok, result?}` (`claim` from the claim response; 409 when stale) |
| `GET /api/plugin-jobs/status?id=` | caller polls: `{id, state, result}`; states: `pending`, `running`, `done`, `error`, `unknown` (recycled) |
| `GET /plugins-run` | headless page that loads every plugin (UI hidden) and executes jobs while open |

Flow for an external system:

```sh
# 1. upload whatever the action needs (e.g. a file, via /upload)
# 2. enqueue
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/folder/file"}}'
# -> {"id":7}
# 3. open http://crosspoint.local/plugins-run in a browser tab or hidden
#    webview (jobs only execute while a hosting page is open)
# 4. poll
curl "http://crosspoint.local/api/plugin-jobs/status?id=7"
# -> {"id":7,"state":"done","result":{...}}
```

Limits: plugin/action names < 24 chars, args and result < 192 bytes of JSON
each. A job claimed by a runner that dies is reclaimable after 10 minutes.
Results persist only until their slot is recycled — poll promptly.

Plugins register handlers in `plugin.js`:

```js
api.registerAction('myaction', async (args) => {
  // runs in the hosting page; throw -> state "error" with {error: message}
  return { anything: 'small' };  // -> state "done" with this result
});
```

## Surface 3: on-device manifests and catalog screens (`device.json`)

A declarative manifest the firmware's generic `PluginCatalogActivity` renders
under **Home → Plugins**. It expresses "authenticated JSON
catalog: sign in, browse, download, sidecar" — enough for most book services —
without any code running on the device. Anything beyond this vocabulary
belongs in `plugin.js`.

`browse.url` makes the manifest an openable catalog. An `events` section
without `browse.url` instead makes it a background/events manifest: discovery
keeps its title/description/README semantics, while the picker deliberately
does not open it as a catalog. A manifest may combine both sections.

The firmware piece (service-agnostic, in `src/activities/plugins/`):
`PluginCatalogActivity`, one activity that opens as a picker over the
installed `device.json` plugins (title + description rows) and morphs into
the manifest-driven browse / download / sign-in flow for the picked plugin.
`discoverPlugins()` rescans the plugin folders each time the picker opens;
nothing stays resident. Browser-only plugins (`plugin.js` without a
`device.json`) are listed as inert "Web-only plugin" rows — an install is
visibly installed — while their UI lives in the web interface.

### Firmware identity and plugin capabilities

Plugin compatibility is expressed by the firmware itself so any catalog or plugin
can decide whether an entry is safe to offer or use on a particular build. Plugin
Hub is the first consumer that motivated this contract, but these are generic
plugin-system capabilities, not Plugin Hub-specific flags. These values are part
of the plugin-system contract, not hardware detection.

| Value | Stock CrossPoint value | Purpose |
|---|---|---|
| `firmware_family` | `crosspoint` | Broad compatibility family. Forks that intentionally keep the same plugin contract can advertise the same family. |
| `firmware_id` | `crosspoint` | Exact firmware implementation ID. Use this when a plugin depends specifically on stock CrossPoint behavior rather than the wider family. |
| `plugin_api` | `1` | Numeric plugin contract level for requirements that need a minimum firmware API generation. |
| `plugin_capabilities` | comma-separated names below | Fine-grained feature advertisement. A catalog entry may require one or more capabilities without hard-coding a firmware version. |

Razor-Crest currently advertises these catalog capabilities:

| Capability | What it means | Why it exists in the plugin system |
|---|---|---|
| `catalog.lists` | The catalog manifest can expose named browse lists, including lists supplied dynamically through `browse.lists_url`. | Any plugin can split or dynamically publish multiple catalog views without adding plugin-specific firmware logic. Plugin Hub currently uses this for Official/Community catalogs. |
| `catalog.notices` | A browse list can display an optional confirmation notice with custom title, message, Confirm, and Cancel labels before opening. | Any plugin can require a generic user notice before entering a catalog or section. Plugin Hub currently uses this for the Community-plugin warning. |
| `catalog.bundle_install` | A catalog entry can install a multi-file plugin bundle from a base URL plus a file list into the plugin directory. | Any catalog can distribute a plugin or other supported bundle made of multiple files without a dedicated installer in firmware. Plugin Hub currently uses this to install complete plugins. |
| `catalog.version_tracking` | Catalog item versions are compared with installed plugin manifests to show Installed or Update status. | Any catalog can expose install/update state using the generic installed-plugin version contract. Plugin Hub currently uses it to identify available updates. |
| `catalog.compatibility` | JSON catalog entries can be filtered by firmware family, exact firmware ID, minimum plugin API, and required capabilities. | Any catalog can omit entries that require firmware behavior the current device does not provide, while entries with no compatibility metadata remain backward-compatible. Plugin Hub currently uses this for compatibility-aware discovery. |
| `catalog.images` | JSON catalog items can fetch and render native BMP/JPEG/PNG preview images in list or grid layouts. | Any plugin can add image-backed browsing without implementing its own renderer. |
| `catalog.cache` | JSON catalog pages and display-ready previews can use the bounded progressive SD cache. | Any plugin can reuse fetched catalog/image data without adding plugin-specific storage logic. |
| `catalog.refresh` | A JSON catalog can opt into a native Refresh footer action that forces a network refetch of the current page. | Any dynamic catalog can refresh in place, including one-page catalogs that have no paging action to trigger another request. |
| `catalog.item_details` | JSON catalog items can open a Details page with selected-row metadata and optional bounded on-demand HTML/text content. | Any catalog can keep browse rows concise while offering richer information before Fetch/Install/Update. Rich metadata is additive and failure falls back to the normal item details view. |

Capability names describe firmware behavior available to the entire plugin system.
They must not encode Plugin Hub policy or ownership, and another plugin may use the
same capability whenever it depends on the same generic firmware behavior.
Additional firmware features should advertise their own capability names only when
the corresponding runtime support is actually present. Catalog entries that omit
compatibility metadata remain visible for backward compatibility.

### Runtime device capabilities

Runtime device capabilities describe hardware/input features of the current device and
are intentionally separate from the static `plugin_capabilities` firmware contract.
Native plugin settings may use `visible_if.device_capability` to appear only when the
current hardware provides that capability. The first supported capability is `touch`,
which is true when the active device exposes usable touch input. Unknown capability
names evaluate false.

This is useful for controls that only make sense on touch hardware. For example, a
plugin can offer a touch-only pagination preference without exposing a dead setting on
button-only devices:

```jsonc
{
  "key": "hide_pagination",
  "title": "Hide pagination buttons",
  "type": "toggle",
  "default": false,
  "visible_if": { "device_capability": "touch" }
}
```

### Schema

String fields explicitly passed through the template resolver support substitutions.
Template-bearing fields include request URLs, bodies, and header values;
`browse.lists_url`; list notice title/message/Confirm/Cancel text; `bundle.subdir`;
download filename/username/password; and sidecar path/body.

| Variable | Meaning | Available in |
|---|---|---|
| `{token}` | contents of the token file at `token.path` | template fields |
| `{cfg.KEY}` | value of `KEY` in the flat JSON config file at `config.file` | template fields |
| `{firmware_family}` | stable firmware family (`crosspoint` on stock CrossPoint) | template fields |
| `{firmware_id}` | exact firmware implementation ID (`crosspoint` on stock CrossPoint) | template fields |
| `{firmware_version}` | current `CROSSPOINT_VERSION` value | template fields |
| `{plugin_api}` | numeric plugin API level | template fields |
| `{plugin_capabilities}` | comma-separated advertised capability names | template fields |
| `{page}` | current 1-based page | browse |
| `{limit}` | active layout item count + 1 (the extra item detects "more pages") | browse |
| `{id}` `{title}` `{author}` `{url}` `{thumbnail}` | fields of the selected item | image, download, sidecar |
| `{md5}` | MD5 hex of the destination file path | sidecar |
| `{dest}` | sanitized on-SD path of the downloaded file | sidecar |
| `{device_code}` | from the auth request response | auth poll |

Two browse formats:
- **`"json"`** (default) — a paged JSON list, using `items`/`fields` below.
- **`"xml"`** — walks a repeating XML element with optional folder navigation
  (Confirm opens a folder, Back goes up). No paging; the JSON `items` and
  grid/list presentation fields are ignored. The firmware knows no specific protocol: a WebDAV
  `PROPFIND` multistatus, an OPDS/Atom feed, or any XML list is all manifest
  config. XML field selectors are `"elem"` (child element text), `"elem@attr"`
  (child element attribute), or `"@attr"` (attribute on the item element).

For JSON catalogs, `browse.compatibility` maps optional catalog-entry fields to firmware checks. If a catalog entry omits compatibility metadata, it is accepted and displayed normally. When an entry declares restrictions, `firmware_family` and `firmware` are comma-separated allow-lists, `plugin_api_min` requires the device API to be at least that value, and every comma-separated `requires` capability must be advertised by the firmware. Any mismatch silently omits that entry.

JSON catalogs may also map `description`, `detail_author`, and `repository` for a generic item-details view, plus `details_url` and `details_format` for optional richer content. Catalog row subtitles may use up to three lines; the full Details view is still opened explicitly. Version-tracked catalogs automatically use Details before install/update and, for backward compatibility, recognize conventional item keys named `description`, `author`, `repository`, `details.url`, and `details.format` when explicit mappings are absent. Other JSON catalogs get the same Details-before-Fetch flow when a details field is mapped. The firmware re-streams the cached page only for the selected item, so long metadata is not retained for every row in memory. If `details_url` is present, firmware fetches that selected item only (bounded to 24 KiB); `details_format: "html"` uses the existing semantic HTML reader while an empty format or `text` is plain text. An unavailable URL or unsupported format falls back to the normal description/author/version/repository Details page and never blocks Install, Update, or Fetch.

```jsonc
{
  "title": "Service Name",                  // menu label; defaults to folder name

  // Relative paths are in the plugin's own folder; absolute paths work too.
  // The credential stores (wifi/opds/koreader .json) are refused.
  "token": {                                // omit for token-less catalogs
    "file": "token.json",                   // written by auth (either side)
    "path": "token",                        // dotted JSON path inside the file
    "persist": true                          // false = discard when this plugin catalog is left/reopened
  },

  "config": {                               // optional: flat JSON of {cfg.KEY} values,
    "file": "config.json"                   // e.g. a user-entered server URL + credentials
  },

  "ui": {                                   // optional native plugin shell; json catalogs only
    "default_tab": "library",
    "tabs": [                               // at most 4
      { "id": "library", "title": "Library",  "type": "browse" },
      { "id": "cache", "title": "Cache",       "type": "cache" },
      { "id": "settings", "title": "Settings", "type": "settings" }
    ]
  },
  "cache": {                                // optional bounded cache for re-fetchable plugin data
    "enabled": true,
    "scope": "shared",                         // shared (default) or token (auth-token cache identity)
    "refresh": "{cfg.cache_refresh}",          // always, 3d, 7d, 14d; default 7d
    "limit_mb": "{cfg.cache_limit_mb}",        // bounded by firmware to 8..128 MB; default 32
    "namespace": "catalog-v1",                 // optional key-compatibility namespace; bump after request semantics change
    "generation": "2026-10-17",               // optional snapshot id; mismatch lazily revalidates affected entries
    "stale_fallback": true,                    // default true: stale entries remain usable if refresh fails
    "manual_refresh": { "scope": "catalog" }, // current, catalog (default), or plugin
    "resources": { "details": "off" },        // off (default), ttl, or immutable; thumbnails are request-keyed automatically
    "seed": { "url": "https://.../thumbnail-cache.zip" } // optional user-triggered cache seed
  },

  "settings": [                             // optional native editor for config.file; at most 12
    { "key": "layout", "title": "Layout", "type": "choice", "default": "grid",
      "options": [                          // at most 6 per choice
        { "title": "Grid", "value": "grid" },
        { "title": "List", "value": "list" }
      ] },
    { "key": "list_thumbnails", "title": "Show thumbnails in list view",
      "type": "toggle", "default": true },
    { "key": "hide_pagination", "title": "Hide pagination buttons",
      "type": "toggle", "default": false,
      "visible_if": { "device_capability": "touch" } },
    { "key": "cache_refresh", "title": "Refresh cache", "type": "choice", "default": "7d",
      "options": [
        { "title": "Always", "value": "always" }, { "title": "3 days", "value": "3d" },
        { "title": "7 days", "value": "7d" }, { "title": "14 days", "value": "14d" }
      ] },
    { "key": "cache_limit_mb", "title": "Cache storage", "type": "choice", "default": "32",
      "options": [
        { "title": "8 MB", "value": "8" }, { "title": "16 MB", "value": "16" },
        { "title": "32 MB", "value": "32" }, { "title": "64 MB", "value": "64" },
        { "title": "128 MB", "value": "128" }
      ] }
  ],

  "browse": {                               // required
    "format": "json",                       // or "xml"
    "url": "https://.../search",             // required only for an openable catalog
    "method": "POST",                       // default GET
    "headers": { "Authorization": "Bearer {token}" },
    "body": "{\"page\":{page},\"per_page\":{limit}}",

    // --- json format ---
    "items": "",                            // dotted path to the item array; "" = response root
    "fields": {                             // json: dotted paths (numeric = array index)
      "title": "title",                     // required (items without one are dropped)
      "author": "authors.0.name",            // compact row subtitle
      "detail_author": "authors.0.name",     // optional Details author; defaults to author
      "id": "id",
      "url": "download_url",                // when the item carries a direct file URL
      "thumbnail": "thumbnail",             // optional preview URL; exposes {thumbnail}
      "version": "version",                 // catalog-of-plugins only: badges each row
                                            // Installed / Update by comparing this to the
                                            // installed plugin's manifest.json (found by id).
                                            // Both are MAJOR.MINOR.PATCH ("1.2.0"); only a
                                            // newer catalog version shows Update.
      "description": "description",         // optional: full paged text on Details
      "repository": "repository",             // optional: shown on Details
      "details_url": "details.url",            // optional: fetched only when Details opens
      "details_format": "details.format"       // optional: "html" or plain-text default
    },
    "compatibility": {                    // optional JSON-only catalog filtering
      "firmware_family": "compatibility.firmware_family",
      "firmware": "compatibility.firmware",
      "plugin_api_min": "compatibility.plugin_api_min",
      "requires": "compatibility.requires"
    },
    "grid_page_size": 6,                     // grid item count: 1..16
    "list_page_size": 10,                    // list item count: 1..16
    "view": "list",                         // "list" (default) or "grid"; json only
    "grid_columns": 4,                      // grid only: 1..4; default 4
    "grid_rows": 2,                         // visible grid rows: 1..4; default 2
    "list_thumbnail": false,                // list only: request thumbnail cards
    "selection_style": "theme",             // theme, light, dark, or outline
    "hide_pagination": "{cfg.hide_pagination}", // hide Previous/Next; swipe/hardware paging still works
    "refresh": true,                          // optional native Refresh footer action
    // Presentation values above may also be strings such as "{cfg.layout}"
    // or "{cfg.grid_rows}", binding the renderer to native plugin settings.
    "image": {                              // optional per-item preview request (json only)
      "url": "{thumbnail}",                // or any template using id/title/author/url/thumbnail
      "method": "GET",                      // default GET; body/headers work like browse
      "headers": { "Authorization": "Bearer {token}" },
      "format": "jpeg",                     // jpeg (default), png, bmp, or auto-detect
      "radius": 0                             // optional rounded preview corners, 0..16
    },

    "lists": [                              // optional named sub-catalogs (json only):
      { "title": "All Books" },             // a picker screen precedes browsing;
      { "title": "Favorites",               // each entry may override url and/or
        "body": "{\"page\":{page},\"per_page\":{limit},\"list\":\"favorites\"}",
        "notice": {                            // optional confirmation before opening
          "title": "Community content",        // defaults to the list title
          "message": "Review this notice before continuing.",
          "confirm": "Continue",               // defaults to translated Confirm
          "cancel": "Go Back"                  // defaults to translated Cancel
        } }
    ],                                      // omitted keys fall back to browse's
    "lists_url": "https://.../catalog-lists.json", // optional remote list index; replaces static lists when used

    // --- xml format (ignore the json fields above; fields become selectors) ---
    "item": "response",                     // local-name of the repeating element
    "container_element": "collection",      // presence marks a navigable folder (omit = flat)
    "skip_self": true,                      // drop the entry equal to the request URL
    "resolve_urls": true,                   // resolve url field against the request origin
    "extensions": [".epub", ".pdf"]         // allowed file extensions (omit = all)
  },

  "download": {
    "url": "https://.../books/{id}/download",
    "method": "POST",
    "headers": { "Authorization": "Bearer {token}" },  // file GET too, unless url_path is set
    "body": "{}",
    "url_path": "url",                      // response field with the file URL;
                                            // omit to treat "url" itself as the file URL
    "username": "{cfg.user}",              // optional HTTP Basic creds for the file GET
    "password": "{cfg.pass}",             // omit for token/header auth
    "dest_dir": "/ServiceName",             // created if missing; falls back to SD root
                                            // a "dest_dir" in the config file overrides it,
                                            // so the plugin's web card can offer the setting
    "filename": "{title}.epub",             // rendered filename is sanitized to 100 bytes;
    "preview_on_select": "{cfg.allow_image_preview}", // config toggle; true = full-screen Close/Save, false = direct save
                                            // a conventional extension is preserved
    "sidecar": {                            // optional per-book metadata file
      "path": "{dest}.meta.json",             // the book metadata sidecar convention (see plugin-events.md)
      "body": "{\"book_id\":{id}}"
    }
  },

  "auth": {                                 // optional on-device sign-in
    "type": "device_code",                  // or "password"

    // device_code (interactive: shows a code + QR, polls until authorized):
    "request": { "url": "...", "method": "POST", "headers": { ... }, "body": "..." },
    "poll":    { "url": "...", "method": "POST", "headers": { ... },
                 "body": "{...\"device_code\":\"{device_code}\"}" },
    // device_code response field paths, with their defaults:
    "code_path": "user_code", "verify_url_path": "verification_uri",
    "device_code_path": "device_code", "interval_path": "interval",
    "expires_path": "expires_in", "token_path": "access_token",
    "error_path": "error",

    // optional presentation-only hints for the device-code screen. These do
    // not alter the QR payload or polling flow:
    "display_url": "https://example.com",
    "qr_hint": "Scan QR to join this session",
    "manual_hint": "Or open the server, choose \"Join Session\", and enter the code above"

    // password (silent: mints a token from stored config credentials — for
    // OAuth2 password grants and login-returns-token APIs). Only "request" and
    // "token_path" apply; no user interaction:
    //   "type": "password",
    //   "request": { "url": "{cfg.url}/oauth/v2/token", "method": "POST",
    //                "headers": {...}, "body": "...{cfg.username}...{cfg.password}..." },
    //   "token_path": "access_token"
  }
}
```

### Behavior

- **Sign-in (`device_code`)**: when a token is needed, firmware establishes Wi-Fi
  before starting authorization. It then shows the verification URL (text + QR)
  and user code and polls until authorized, honoring `authorization_pending`,
  `slow_down`, `expired_token`, and `access_denied`. The token is written to
  `token.file` in the shape `token.path` expects, so the web plugin and the device
  share one sign-in. `token.persist` defaults to `true`; with `false`, firmware
  discards that token when the catalog is left or reopened, and a browse 401/403
  automatically starts fresh device authorization instead of retaining stale state.
  Once loaded, a non-persistent token remains valid in RAM for that open catalog
  session even if its backing file is also a firmware-owned browse scratch path that
  is moved or removed while caching the current response.
- **Sign-in (`password`)**: no on-device UI — the device silently POSTs the
  stored config credentials to the token endpoint before browsing, and re-mints
  a fresh token automatically after any 401 (these tokens are short-lived).
  Credentials come from a browser `plugin.js` writing the `config.file`. Set it
  up once on the web page; the reader is then standalone.
- Without an `auth` block the not-signed-in screen directs the user to the web
  plugin.
- **Stale tokens**: a 401/403 from browse returns to the sign-in screen rather
  than an error.
- **Pagination and refresh** (json): the browse request should return up to `{limit}` items.
  Grid and list layouts use `grid_page_size` and `list_page_size` independently;
  the extra item only determines whether another server page exists. Enhanced
  plugin UIs use FreeInkUI's native footer for Previous/Next pagination. Setting
  `browse.refresh: true` adds a separate Refresh action between them; Refresh forces
  a network refetch of the current page, bypassing a fresh catalog-page cache when
  one is configured. `browse.hide_pagination` may resolve to true on any device to
  omit only the Previous/Next footer actions; Refresh remains independent. Vertical
  browse swipes and supported hardware page/batch controls still cross server-page
  boundaries at the first/last viewport, so hidden pagination never disables paging.
  When pagination is shown, unavailable directions remain visible but disabled, and D-pad focus moves
  through the footer actions after the content items.
- **Progressive cache** (json): an optional top-level `cache` block keeps disposable,
  re-fetchable plugin data under that plugin's own `.cache` directory. Catalog pages
  remain the primary cached resource; converted thumbnails and opt-in rich item details
  use the same bounded request-key infrastructure. Persistent user downloads are never
  managed as cache entries. A fresh cached page opens directly from SD without requiring
  Wi-Fi. Browsing beyond the cached frontier fetches only the missing page and adds it to
  the existing cache; there is no prefetch or bulk catalog download. `refresh` controls
  TTL freshness (`always`, `3d`, `7d`, or `14d`; default `7d`). Cache scope
  defaults to shared. Token scope adds the currently loaded plugin token to cache
  identity and is inactive until a token is loaded; token-scoped resources remain
  reusable across normal navigation regardless of TTL age.

  Cache invalidation is policy-driven. `manual_refresh.scope` accepts `current`,
  `catalog` (default), or `plugin`. Settings **Refresh now** always fetches the
  currently displayed page immediately. `current` preserves the legacy one-page
  behavior. `catalog` advances a lightweight epoch for the active catalog, while
  `plugin` advances an epoch shared by all of that plugin's catalogs. Epoch invalidation
  is lazy: other cached pages/resources are marked stale logically and are re-fetched
  only when visited, so Refresh now never crawls the whole catalog. With
  `stale_fallback: true` (default), stale data remains available if the network fetch
  fails.

  `namespace` is an optional cache-key compatibility string; changing it intentionally
  creates a new cache namespace after a plugin changes request or pagination semantics.
  `generation` is an optional snapshot identifier. A cached page or metadata resource
  whose stored generation differs is treated as stale even if its TTL has not expired.
  A dynamic `browse.lists_url` response may publish a non-empty top-level
  `generation`; when observed it overrides the manifest generation for that hosted
  catalog snapshot. Generation invalidation takes effect when firmware next fetches
  the dynamic list index and sees the changed value, without forcing a plugin update.

  Request-keyed binary resources such as thumbnails remain reusable when their request
  identity is unchanged, so catalog refresh does not purge image data.
  `resources.details` may opt rich item-detail text/HTML into the generic resource
  cache as `ttl` or `immutable` (default `off`); both still obey manual epochs and
  generation changes, while `ttl` also obeys the configured refresh age. `limit_mb`
  is clamped to 8..128 MB (default 32 MB); when exceeded, old cached resources are
  evicted before old catalog pages while the page currently on screen and its thumbnails
  are protected. Cache-enabled plugins may expose these controls inside the native
  Settings tab: the top-level Settings list gets an expandable Cache section containing
  cache-bound settings, last refresh, page/resource counts, storage usage, seed controls,
  Refresh now, and Clear cache. The legacy `cache` tab remains supported for existing
  manifests. Search results are not cached.

  `browse.refresh: true` is separate from cache policy: it exposes a footer Refresh
  action that explicitly re-fetches only the current browse page.
- **Lists** (json): with `browse.lists`, opening the catalog shows a picker
  of the named entries first; each entry browses with its own url/body
  overrides (server-side categories, shelves, sort orders). A list with
  `notice.message` shows a confirmation before browsing; Back/cancel returns
  to the picker, while confirm continues into the selected list. `notice.title`
  defaults to the list title, and omitted button labels use the firmware's
  translated Cancel/Confirm defaults. Back from the book list returns to the
  picker.
- **Native tabs and settings** (json): `ui.tabs` exposes a bounded native tab
  shell using the same tab treatment as the main UI. Supported tab types are
  `browse` and `settings` (maximum 4 tabs). A settings tab edits the plugin's
  existing `config.file`; settings are declarative `choice` or `toggle` rows
  (maximum 8 settings, 6 choices each). Missing config keys use their declared
  defaults. Presentation properties may bind to those values with the existing
  `{cfg.KEY}` template syntax, so changing a plugin setting can switch layout
  without a firmware or plugin update.
- **Image layouts** (json): `browse.image.url` is a per-item request template
  and may use `{id}`, `{title}`, `{author}`, `{url}`, or `{thumbnail}`. `view: "grid"` uses
  `grid_columns` (1..4, default 4) by `grid_rows` (1..4, default 2) as the visible
  viewport. If `grid_page_size` exceeds that viewport, FreeInkUI scrolls the grid
  to keep the selected cover visible. `list_page_size` is independent.
  `list_thumbnail: true` uses thumbnail cards; false returns to the normal one-row
  list and skips preview fetching. `selection_style` controls list-row selection as
  `theme`, `light`, `dark`, or `outline`; grid items always use a cover frame so
  their preview is never obscured. The native footer uses its own FreeInkUI focus
  treatment. `image.radius` optionally masks preview corners to a bounded 0..16 px
  radius. JPEG and PNG sources are downsampled to 1-bit BMP on SD; `format: "bmp"`
  expects an already suitably sized BMP. `format: "auto"` detects BMP, JPEG, or PNG
  from the downloaded file signature before selecting the decoder. If `image` is
  omitted, the normal one-row
  list remains in use; an individual image fetch/convert failure renders that item
  with the standard empty preview placeholder.
- **XML navigation**: Confirm on a folder (an item carrying `container_element`)
  descends into it, Back climbs out (Back at the root leaves the screen).
  With `resolve_urls`, server-relative URLs resolve against the browse URL's
  origin. A `config.file` lets values like a server URL and credentials be
  user-entered (via a browser `plugin.js`) rather than baked into the manifest.
- **After download**: the book's layout cache is invalidated and the optional
  sidecar is written. Use the `{dest}.meta.json` convention so the fields ride
  KOSync progress uploads and event handlers (see `plugin-events.md`).
  Downloading a book file (`.epub`, `.txt`, `.md`, `.xtc`) also marks the
  Library index stale, so the book appears the next time the Library opens.
  Hidden files and folders (names that start with `.`) don't mark it, because
  the Library never lists them.

### Heap budget (ESP32-C3, ~380KB total)

- Manifest ≤ 8KB, token file ≤ 2KB, small API responses (auth, download-url
  hop) ≤ 48KB (hard caps).
- Browse responses (json and xml) stream to an SD temp file and are parsed
  from it, so the raw body never occupies DRAM — services that inline heavy
  per-item metadata (a page of BookFusion JSON runs 60+ KB) work unmodified.
  A 1MB cap bounds a misbehaving server.
- Catalog responses are parsed with a dynamically built ArduinoJson filter
  containing only the declared field paths, so a page parses into a few KB
  no matter the body size.
- JSON preview images are downloaded one at a time to SD with a 256KB source
  cap. JPEG/PNG previews are converted directly to small 1-bit BMP files; the
  catalog keeps only one file-backed bitmap reader in memory and at most 16
  ready-bit flags, matching the JSON page-size ceiling.
- One TLS session is kept alive across browse requests: repeated handshakes
  permanently fragment the heap.
- The book download streams to SD via `HttpDownloader`; file size is
  unconstrained.
- An XML list is parsed from the temp file in 2KB chunks; the engine stops
  after 200 entries per folder/feed. Split very large libraries into
  subfolders.

### Testing a new manifest

1. Copy the plugin folder to `/.crosspoint/plugins/<name>/` on the SD card.
2. Enable **Settings → System → Advanced → Plugin System**, then open **Home → Plugins → your title**. With no token and no `auth`
   block you should see the not-signed-in screen; with `auth`, the code/QR
   screen.
3. Watch serial (`[PCAT]` tag) for request/parse failures — the log includes
   HTTP status and truncation flags.
4. Token-less services (public JSON APIs) work by omitting the `token` block
   entirely; that is the quickest way to validate `items`/`fields` paths.

## Surface 4: firmware events and the book metadata sidecar

The firmware can record whitelisted moments (`reader.session`, sleep entry, a
catalog download finished) into a per-plugin queue
and later replay them through HTTP request templates declared in
`device.json` — including bounded WiFi bring-up at sleep entry for handlers
that opt in. A companion convention, the `<book path>.meta.json` sidecar,
lets plugins attach service fields (e.g. a service book id) to a book; those
fields ride along with KOSync progress uploads and are available to event
handlers as `{meta.*}` variables. See `plugin-events.md` for the whole
surface: the event whitelist, handler schema, delivery semantics, and limits.
The device's plugin list shows, under each plugin, the events it receives, so
the person using the reader can see what activity a plugin is sent.

## Ideas to build

The two planes decide *how* you build something, so pick by what the idea needs:

- **On-device (`device.json`)** — standalone on the reader, but only the
  declarative vocabulary above: an authenticated JSON or XML/OPDS-style catalog,
  browse, download to SD, optional device-code or password sign-in and per-book
  sidecar. No arbitrary logic.
- **Browser (`plugin.js`)** — anything at all (any protocol, auth, format), but
  it runs in a connected browser/app. Use it when the idea needs request
  signing, HTML→EPUB conversion, multi-file writes, or a real UI. The job queue
  (`/api/plugin-jobs` + `/plugins-run`) lets an external app trigger it.

Rule of thumb: if the service is "log in, list an authenticated JSON/XML
catalog, download files," it's on-device. If it needs computed request bodies
(OAuth 1.0a signing, canonicalized signatures) or format conversion, it's
browser-side.

### On-device (`device.json`) ideas

- **Self-hosted library presets** — Kavita, Komga, Audiobookshelf, Calibre-Web,
  COPS. Most expose OPDS (native OPDS browsing already covers those — a plugin
  adds value mainly for their JSON APIs or token auth). Build: `browse.format`
  `json` against the REST API, or `xml` against the OPDS feed; `download.url`
  from the entry; bearer/Basic/`{cfg.*}` auth. See `bookfusion` (JSON + token)
  and `webdav` (XML + config creds).
- **Dictionary store** — browse a JSON index of dictionary files, download to
  the SD dictionary dir. No native equivalent. Build: token-less `json` catalog,
  `download.dest_dir` = the dictionary folder, no sidecar. ~15 lines.
- **Theme store / font store** — same shape as the dictionary store, pointed at
  the theme or font directory. Asset catalogs OPDS can't express.
- **More read-later that exports EPUB** — anything with a token API and an
  EPUB export URL fits like `wallabag`: `auth.type` `password` (or a stored
  bearer), `browse` the entries JSON, `download` the export endpoint with a
  Bearer header.
- **A curated free-books catalog** — Standard Ebooks / Gutenberg via their OPDS
  feed (`xml` format, token-less). Overlaps native OPDS, but ships as a
  zero-config preset.

### Browser (`plugin.js`) ideas

- **Plugin store** — install other plugins from a hosted catalog. Build: fetch
  a catalog with `api.relay`, `/mkdir` the target folder, `api.writeFile` each
  file. See `plugin-store/`.
- **AO3 / fanfiction downloader** — the site serves per-work EPUB downloads.
  Build: take a work URL, `api.fetchToSd` (or `api.relay` + `api.writeFile`) the
  EPUB to SD. Browsing/search is HTML, so parse it in the browser.
- **RSS → EPUB digest** — build a "morning edition." Fetch feeds with
  `api.relay`, assemble an EPUB in the browser with the JSZip the File Manager
  page already loads, then `api.writeFile` it to SD.
- **Read-any-article** — paste a URL, run it through a readability service via
  `api.relay`, wrap the result as an EPUB, write to SD.
- **Metadata / cover editor** — a File Manager plugin: open an EPUB with JSZip,
  edit the OPF (title/author/cover), rewrite it in place. See
  `organize-by-author/` for the File-Manager-endpoints pattern.
- **Library tidy tools** — dedupe, bulk rename, move-by-metadata. Same
  same-origin endpoints (`/api/files`, `/mkdir`, `/move`, `/delete`), no device
  API needed.
- **Signed-API services** (Instapaper OAuth 1.0a, etc.) — must be browser-side:
  do the per-request signing in JS with `crypto.subtle`, relay the calls, and
  convert the article HTML to EPUB before writing to SD.

### Building blocks reference

| Need | Use |
|---|---|
| Outbound HTTP(S), any method (CORS-free) | `api.relay(method, url, headers, body)` |
| Download a URL straight to SD | `api.fetchToSd(url, dest, headers)` |
| Write a small file to SD | `api.writeFile(path, base64)` |
| The plugin's own folder (keep its data here) | `api.dir` |
| Crypto (hash, HMAC via SHA, AES, RSA) | `api.crypto(op, fields)` or browser `crypto.subtle` |
| Create / delete / move SD files | same-origin `/mkdir`, `/delete`, `/move` |
| On-device catalog/browse/download | `device.json` (this document) |
| External app triggers an action | `api.registerAction(name, fn)` + `/api/plugin-jobs` |

## Protected content and loan expiry

Books whose entries are content-protected open through the read path in
`lib/Epub/ContentProtection.cpp`. The core (SDK `ContentProtection`) only
understands the standard OCF `encryption.xml` and decrypts entries on demand,
streamed in small chunks; nothing decrypted is ever written to SD.

The reader gets each book's content key from `<book>.key`, written through
`POST /api/book-key` by the plugin that fulfilled the book: the plugin (or its
service) turns whatever the provider delivers into the key, and the device
stores it wrapped to itself. The reader carries no rights or account scheme.
A book with no `.key`, or one wrapped by another reader, does not open. A full
flash erase replaces the device secret, so a plugin should keep what it needs
to derive keys again.

When the key carries a due date, the reader enforces it against
`lib/TrustedTime`: a monotonic clock floor persisted in NVS (on-flash, not on
the removable card), restored into the system clock at boot, advanced at
every sleep entry and snapped to real time by SNTP on every Wi-Fi join. The
floor can lag real time while the device sits powered off, but it can never
be rolled back — staying offline delays a due date at most by the powered-off
gap, it does not suspend it. A book with a due date and no trustworthy clock
at all refuses to open and asks for one Wi-Fi connection.
