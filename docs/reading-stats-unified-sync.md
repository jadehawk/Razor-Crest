# Unified Reading Statistics — basic and full modes

The SD installable Reading Statistics plugin now provides two modes from a
single `device.json`:

- **Baseline CrossPoint:** the stock plugin catalog, QR/PIN sign-in and
  `reader.session` background POST to the Techy-Notes beta server. No native
  reading-history widget support or KOSync configuration is required.
- **Razor-Crest with the updated renderer:** the six existing offline dashboard
  pages plus **Server Sync** as page seven. Confirm or tap opens the *existing*
  `PluginCatalogActivity` directly for the installed `reading-statistics`
  plugin, reusing the same device-code, saved token and catalog network logic.

The renderer's page limit is now seven. No new firmware-specific authentication
protocol, browser, or server endpoint is introduced. No firmware image is built
by this change.

## Persistent token and migration

The unified manifest stores the device credential in the absolute SD path
`/.crosspoint/reading-statistics-token.json`. This is outside the installed
`/.crosspoint/plugins/reading-statistics` directory, which the Plugin Hub may
delete on uninstall. It is also outside the protected *native* BIN directory
`/.crosspoint/reading-stats/`: stock CrossPoint's generic plugin HTTP
functions cannot write inside that protected directory.

Razor-Crest protects the persistent credential against generic file API access
via `ProtectedPaths.h`. Interactive plugin auth and background event drains
allow this reserved path only when invoked by the installed
`reading-statistics` plugin; general plugin settings are explicitly
prevented from using it.

On opening the updated Razor-Crest dashboard, migration attempts to **copy**
the old `reading-stats-sync/token.json` from each standard plugin root only
if the new token does not exist. It does **not** replace an existing guest
identity. An existing legacy `events.jsonl` queue is copied to the new
plugin folder only when (a) its destination is empty and (b) the source and
destination tokens represent the **same** pairing. Old files are never
removed, renamed or modified.

On unmodified stock CrossPoint there is no native migration hook: users must
copy the old JSON token to the new absolute path themselves before removing
the companion, and allow pending events to flush. The firmware is still able
to use that absolute file path through the existing plugin HTTP interface.

**Do not uninstall an existing companion before checking that the unified
plugin still has a paired browser identity and that all queued sessions have
been accepted.** The two plugins should not remain subscribed concurrently
after a successful migration.

## Testing and release compatibility

The unified plugin version is 0.1.11, and its manifest no longer globally
requires `stats.native.v1` or `ui.dashboard.v2`; those capabilities only
control whether the firmware can present the native dashboard. The combined
JSON remains under 8 KiB.

Previous Razor-Crest builds with a strict six-page dashboard must not install
this plugin version until the matching firmware source has been built and
flashed. Baseline CrossPoint continues using the existing catalog rendering
contract, ignoring the dashboard definition.

Tested by source-level Python contracts; these do **not** substitute for
physical-device verification of e-ink layout, pairing persistence or sleep
uploads. Firmware compilation/flashing is left to the user.
