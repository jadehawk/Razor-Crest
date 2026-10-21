# Reading Statistics — Razor-Crest

This is an optional **on-device** statistics dashboard. It is declarative;
no code runs from the SD card and no server or Wi-Fi connection is needed.

Install by copying the `reading-stats-dashboard` directory (including
`device.json`) into `/.crosspoint/plugins/` on the SD card. In firmware,
enable **Settings > System > Advanced > Plugin System**, then open
**Home > Plugins > Reading Statistics**.

Requirements: Razor-Crest with `stats.native.v1` and `ui.dashboard.v1`.
Basic CrossPoint firmware cannot render this dashboard; installing the
plugin there should display its README instead. When publishing in Plugin
Hub, mark the listing with `compatibility.requires` containing both
capabilities, so unsupported firmwares do not offer installation.

`device.json` controls page titles, metric order, labels, and which
generic widgets are displayed. Firmware supplies only the local records
and widget renderer. This host supports `metric`, `week_bars`, `heatmap_28`, `book_list`,
and `legacy_import`. The book list opens paginated native book details.
The sixth **Import Statistics** page previews CrossInk `global_stats.bin`
and vCodex v6 `reading_stats.json`. Select opens the import review screen;
choose CrossInk or vCodex, then **select that same source again** to confirm
the conversion. Back cancels before the second selection. The two source rows
show their complete names and summaries, and the status/confirmation message
wraps below them instead of occupying a truncated third list row. The operation
writes Razor-Crest BIN snapshots only, never legacy source files.

CrossInk imports global counts and per-book EPUB cache counts for books whose
original paths are discoverable through recent books or the native index.
vCodex imports per-book sessions, totals and dated reading days; it has no
qualified page-turn counts, so none are invented. Import is **additive** to
existing native data, with one-time receipts to prevent re-importing the same
source/book even if its file changes later. To prevent possible overlap, only
one legacy firmware source can be imported into a native statistics database.
Use a spare SD card for synthetic fixtures or risky migration tests.

Back exits. Previous/Next buttons or directional swipes change pages.
Confirm refreshes the local snapshot on metric pages. On **All Books**,
Confirm (or a tap) opens the book list; selecting a title opens the **same
metric-card dashboard** used for Current Book, with that book's title, author,
and available EPUB series/series index. Back returns to the Books list.
Current Book also identifies its title and author. On Import Statistics,
Confirm opens the review/confirmation screen. The original five dashboard
pages remain visually unchanged. Books appear after a qualifying session
or confirmed import has been recorded. New reading time becomes visible
once the reader finishes a counted session (exit to Home or sleep).

Notes: Daily charts need a trusted date; otherwise all-time totals
continue accumulating but chart buckets remain blank. Stats tracking
counts qualifying forward turns, not browser/catalog navigation.
