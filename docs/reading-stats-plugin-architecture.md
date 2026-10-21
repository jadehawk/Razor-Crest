# Razor-Crest: plugin-first reading statistics

## Status (2026-10-20)

The six-page on-device dashboard, book details, native BIN data and
CrossInk/vCodex importer were exercised on physical X3 and X4Pro devices.
The import review screen supports touch and button selection, uses wrapped
confirmation/status text and refuses reimporting historical data twice.

CrossInk v1/v2/v3 global totals and versioned EPUB cache records are
supported; vCodex format v6 is streamed book-by-book with dated reading-day
history. Originals are not modified. Recovery/replay, import deduplication,
and native BIN store tests pass on the host. Importing a second legacy
firmware source into the same BIN database is intentionally blocked to
avoid potentially overlapping history.

This integration intentionally excludes the unfinished BookOrbit pairing
API, Web Stats access code and page-event upload journal. The reduced
native-only integration branch has host-tested changes; the user-tested
firmware originated from the larger development worktree.

## Architecture and compatibility

Firmware owns the canonical data and reusable dashboard rendering primitives;
plugins own page order, labels, fields, and visual layout. Reading data is
collected even with the Plugin System disabled; Wi-Fi is never started to
collect or display local stats.

The existing plugin picker remains standard for catalogs, browser plugins,
events, and README-only plugins. A small adapter recognizes a standalone
`device.json` with `dashboard.source = "reading-stats.v1"` and opens a
offline dashboard activity with an opt-in, confirmed legacy-data import.

Razor-Crest advertises `stats.native.v1` and `ui.dashboard.v1`
through the firmware identity contract, additive to `plugin_api=1`.
A future Plugin Hub catalog entry should require these two capabilities.
Stock CrossPoint cannot run the native dashboard and falls back to the
ordinary plugin description. No browser statistics gateway is shipped
with this native-only integration.

## Storage and tracking

Razor-Crest reuses the active-dwell accounting of its existing
`ReaderSession` tracker. A successful forward page turn after a displayed
page dwelled for 2..1800 seconds contributes time and one qualified turn.
Reverse turns, failed turns, and page skips are excluded. At exit/sleep a
session of at least 10 active seconds is recorded locally; sessions reaching
60 seconds increment the session count.

The existing `reader.session` plugin-event protocol remains unchanged:
only time measured with a trustworthy clock is exported. Untrusted-clock
reading still contributes to local all-time totals, but cannot be assigned
a historical date.

The SD store is `/.crosspoint/reading-stats/`, with versioned, CRC-like
FNV-1a-checked binary snapshots:

- `global_v2.bin`: all-time totals and rolling 366-day history.
- `book_<path_hash>_v2.bin`: matching per-book data.
- `pending_v1.bin`: recoverable transaction between per-book and global updates.
- `import_pending_v1.bin`: recoverable one-record legacy import transaction.
- `import_receipts_v1.bin`: checksummed, append-only per-source/book receipts
  preventing repeated imports, even after reboot.
- `books_v1.bin`: a fixed-size, checksummed, positional book index (title,
  author, path, hash). It supports bounded lists without loading all books.
- `.bak` companions preserve the last good snapshot if the primary is
  incomplete or damaged.

Each snapshot is 1500 bytes. Session recording writes a durable pending
marker, applies the transaction to both files with monotonically increasing
sequences, then removes the marker. A pending transaction is replayed
idempotently on the next save/load. A damaged pending marker is logged
rather than silently discarded; writing stops until repaired. All updates
are bounded and the renderer uses a cached snapshot instead of reading SD
on every redraw.

The first release uses an SD book-path hash as its identity; moving a book
creates a distinct native record. Legacy data is **never** imported
automatically. A confirmed import merges its historical counters into the
existing native BIN files and leaves the original sources untouched. CrossInk
has global counters and per-book cache counters but no exact dated daily
history; vCodex has dated per-book history but no qualified-turn counter.
No missing field is fabricated. One-time receipts prevent repeated imports
of the same source/book, even if its legacy file is later updated.

## Generic declarative widgets

A dashboard plugin supplies up to six pages and six widgets per page,
with a manifest limited to 8 KiB. `legacy_import` previews legacy
sources and opens a separate two-step confirmation activity; opening the
preview alone never writes either legacy or native files. The native host accepts:

- `metric`: `total_seconds`, `sessions`, `qualified_turns`,
  `seconds_per_turn`, `latest_day_seconds`, `last_7_seconds`,
  `active_days`.
- `week_bars`: seven saved daily buckets.
- `heatmap_28`: a 28-day e-ink activity grid.
- `book_list`: opens a paginated on-device list of tracked books. Selecting a
  book opens the same readable metric-card dashboard as Current Book,
  including title, author and available EPUB series metadata.
- `legacy_import`: source discovery and bounded preview of CrossInk global
  totals or vCodex v6 per-book totals; the review activity requires choosing
  the same source a second time before any conversion starts.
- Widget `scope`: `global` (default) or `current-book` (the most
  recently opened book).

Manifest labels, page sequence, and widget choices are plugin-defined.
The firmware contains no particular dashboard design, cloud service,
or plugin identity. Invalid manifest widget types/fields are rejected
rather than executed.

The standalone installable dashboard lives in the sibling repository
`../reading-statistics.crosspoint-plugin/reading-statistics.crosspoint-plugin/`.
It includes `manifest.json`, `device.json`, `README.md` and `DETAILS.md`.
Only the plugin repository controls the displayed page/metric layout.

## How to test the firmware

1. Build from the current integration branch using
   `C:\XP-FW\build-fw.ps1`; the assistant does not run firmware builds.
2. Install the payload from sibling repository
   `../reading-statistics.crosspoint-plugin/reading-statistics.crosspoint-plugin/`
   into `/.crosspoint/plugins/reading-statistics/` on the SD card.
   Keep `manifest.json`, `device.json`, `README.md` and `DETAILS.md` together.
3. Enable Plugin System in Settings > System > Advanced.
4. Open Home > Plugins > Reading Statistics. Confirm six pages display,
   including All Books and Import Statistics. On All Books, Select opens the
   paginated book list. On Import Statistics, Select opens source review;
   selecting the same valid source a second time explicitly starts import.
   Back cancels before confirmation. Navigation otherwise stays unchanged.
5. Start an EPUB or XTC, allow two or more forward page turns after
   qualifying reading dwells totaling at least 10 seconds, then exit to
   Home (or sleep and wake). Reopen the plugin: the total and turn counters
   should increase. A 60+ second session increments the session count.
6. If the clock is unsynchronized, charts can remain blank even when
   all-time counts advance. To test the Books list and chart presentation
   immediately, run `python test/reading_stats/make_fixture.py` on the PC.
   The fixture now contains `global_v2.bin`, `books_v1.bin` and two synthetic
   per-book snapshots. Copy them to the test SD card as a single fixture set;
   do not mix them with genuine counters.
7. Test restart persistence and remove/reinstall the plugin. Statistics
   must remain while the plugin is disabled or uninstalled.
8. Confirm existing OPDS/Plugin Hub catalogs, KOReader synchronization,
   fixed screens, and timed wallpaper refresh still behave normally.

## Disposable hardware test fixtures

Use a spare/test SD card **only**. The generated fixtures contain synthetic
books and statistics; do not copy them over personal native BIN data.

```powershell
python test/reading_stats/make_legacy_fixture.py --output "C:\Temp\RazorCrest-Stats-Test"
```

Two distinct roots are produced: `crossink_sd` and `vcodex_sd`. Copy only
one root's **contents** onto the test SD card at a time, install the sample
dashboard, and confirm Import Statistics > Select > choose source > select the
same source again. Confirm all-time totals and available book records,
then repeat the action and reboot: the counters must not increase twice.
`vcodex_sd` includes existing native BIN readings for one book to validate
additive merging rather than replacing history. Never merge both test roots
onto the same card: cross-firmware data may overlap. An interrupted import
leaves a journal for replay on the next opening of statistics.

Host tests:
`LegacyStatsImportTest.cpp` (including interrupted-write recovery),
`LegacyStatsPreviewTest.cpp`, `LegacyStatsFormatsTest.cpp`,
`LegacyStatsMergeTest.cpp`, `ReadingStatsDataTest.cpp` and
`ReaderSessionStatsTest.cpp`. The original UI and importer were tested
on X3 and X4Pro; the statistics-only split should be rechecked after a
future device build.

## Next enhancement stages

- Expand CrossInk book discovery beyond the preserved recent-books and native
  book index for older, orphaned cache folders; a filename hash alone cannot
  reconstruct a trustworthy book path.
- Consider a future incremental source-high-water-mark importer. The current
  one-time receipt policy intentionally skips changed source files after the
  first import; it does not silently double-count an updated legacy source.
- Add a reconciled, opt-in strategy for overlapping histories from two legacy
  firmwares. The first migration intentionally refuses the second source.
- Add optional Sync Day for clockless date buckets; track explicit
  completion, per-book timeline, and longer session history.
- Extend generic dashboard widgets for CPR-vCodex's reading profile,
  achievements, goals, richer heatmaps, and navigable per-book/day drilldowns.
- Reconsider a future browser integration without requiring manual
  per-boot Web Stats access codes. BookOrbit remains deferred.
- Add generic cached plugin sleep-screen compositions without triggering
  repeated timed wakes. Keep Razor-Crest timed wallpaper rotation untouched.

## Licensing

The first implementation's data structures and plugin host are newly written
for Razor-Crest. The design references CPR-vCodex's rich analytics and CrossInk's
reading-pace/session concepts, both MIT-licensed. If their substantial source
or visual layouts are copied in later enhancements, preserve author attributions
and the relevant MIT notices in `docs/attribution/`.

Local integration of the native dashboard and sample plugin does not
require a browser access code or network connection.
