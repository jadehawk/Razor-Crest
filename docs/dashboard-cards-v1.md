# Dashboard cards V1 — experimental on-device contract

This is the single pre-release dashboard contract. The old `ui.dashboard.v2`
capability and generic `dashboard.v2` source are retired; do not create
backward-compatibility branches for development builds.

## Manifest and capability

- Dashboard manifest source: `"reading-stats.v1"`
- Required firmware capabilities: `stats.native.v1`, `ui.dashboard.v1`
- Maximum six pages, six widgets per page, 8 KiB `device.json`
- The firmware owns the persisted native reading data, book thumbnails, progress
  and rendering; SD plugin manifests declaratively specify page order and widgets.

## Available widgets

- `metric`: native fields `total_seconds`, `sessions`,
  `qualified_turns`, `seconds_per_turn`, `latest_day_seconds`,
  `last_7_seconds`, and `active_days`.
- `reading_snapshot`: 195-pixel Overall Statistics featured card, independent
  of any currently open book. Displays local calendar-month reading time,
  consecutive-day streak (kept alive by yesterday's reading), active days
  this month, and tracked book count. Uses the existing 366-day global history
  and stored book index, with no persisted format changes. When the device
  date is unknown, date-derived values show -- but the book count remains.
- `hero`: 195-pixel Current Book featured card. A larger real cover is at left,
  with title, author and series/index aligned at right. The progress bar and
  vertically centered percentage appear below the cover/text columns. When
  there is no active reader path, use the most recently opened book if known.
- `week_bars`: latest seven recorded day buckets.
- `heatmap_28`: four complete Monday–Sunday rows of activity.
- `book_list`: directly renders a compact, vertically paginated list of
  book-cover thumbnails alongside title, author, recorded reading time and
  percentage completed. Select/tap opens the same Current Book detail view.
  Hold Select for 900 ms or long-press a touch row to show a cancel-first
  confirmation before removing only that book's native statistics BIN and
  index entry. The EPUB and independent global totals remain untouched.
- `legacy_import`: bounded CrossInk/vCodex preview and confirmed import.

### Cover fidelity and persistent snapshots

The firmware captures distinct SD thumbnails for the 142-pixel hero and
68-pixel All Books row, under
`/.crosspoint/reading-stats/covers/<book-hash>.hero_bw_fill.bmp` and
`<book-hash>.list_bw_fill.bmp`. EPUB, TXT and Markdown JPEG/PNG covers are
converted directly from the source artwork to a size-matched 1-bit BMP
rather than upscaling a small pre-dithered thumbnail.
The source artwork is rendered with a single Atkinson 1-bit pass at
the final thumbnail size. The former four-level/Bayer experiment
produced visible dot grids and was reverted; the shared UI bitmap
renderer is unchanged. Old cached covers remain a fallback if a
book has been deleted, and new cache names trigger regeneration
without manual SD cleanup. The full-cover sleep screen retains its
separate native grayscale display passes. Generation uses the existing
streaming converters to stay within X3 memory limits. Older captured
previews remain available if the source book is missing; removing the book's
history cleans both size-specific caches and any earlier saved preview.

Book metadata fields in `scope: "current-book"` refer to the selected book,
active reader path or most recently opened book. The All Books row uses the
recorded book-index path hash, not
a title, as its identity. Book covers (`.bmp`) and progress (`.pct`) are saved
under `/.crosspoint/reading-stats/covers/` by the firmware when accessed and
survive removal of the source EPUB. For moved/renamed books, a unique recent
title/author match can recover the existing Home cover, missing metadata and
reading percentage; ambiguous titles never borrow other books' previews.
The renderer fits full cached thumbnails into both cover slots even if the
original Home thumbnail is larger. Missing artwork gets a generic placeholder
and unknown progress appears as `--`.

## Card styling and icons

Widget fields `label`, `field`, `scope`, `show_value`, `icon`,
`icon_size`, `border`, and `corner_radius` configure the host renderer.
Valid borders are `square`, `rounded`, `chamfered`, and radius is 0–16.
The `icon` name must match `[a-z0-9_-]{1,32}` and is loaded from the
plugin's `icons/<name>.pbm`. New icons must be binary PBM P4 at 80 × 80,
with the exact nine-byte header `P4\\n80 80\\n` followed by 800 packed
bytes (809 bytes total). Maximum eight distinct icons in one dashboard.

The same first-page renderer is also available as a **live sleep-screen
source** in Custom Wallpaper Collection when the plugin is installed and
enabled. It refreshes from saved native statistics at each normal sleep entry,
without a bitmap screenshot, navigation header, footer controls or network access.
The centered Overall Statistics title remains at the top. With Custom
Timed Sleep-Screen Refresh enabled, each timer wake reloads and repaints the
same first page before returning to deep sleep, even if statistics did not
change. This is a live scheduled display source, not a static bitmap pool.

Neither the cover renderer nor the progress provider requires a network
connection. Each page now shows one centered page title beneath the persistent
Reading Statistics header, with no numeric `(n/6)` counter. Page one is
Overall Statistics (global totals); page two is Current Book (book totals).
The book hero card has no redundant heading; the new Reading Snapshot is
a separate type with its own monthly label. Small-device geometry and controls
must be tested on X3 before validating X4Pro.
