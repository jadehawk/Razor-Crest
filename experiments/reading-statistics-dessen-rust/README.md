# Reading Statistics — Dessen Rust dashboard (offline pages milestone)

Standalone experimental **Rust/Dessen application** with six offline pages:
the five existing Reading Statistics views plus a new Overall Statistics layout
preview. It does not replace the original plugin, use network access, or build
firmware. The new Overall page currently shows **device-local** totals and real
recent unfinished books, *not* combined server statistics.

## Pages

1. **Overall Statistics (local preview):** This Month and device totals
   temporarily stand in for future server aggregates. The four detail cards
   show sessions, page turns, the best reading day, and average time per active
   day. Best Day and Daily Avg. use only retained local daily history (up to
   371 days), not all-time totals; missing history shows `--`. The Continue Reading
   carousel holds up to five genuine recently opened unfinished books, shows
   one at a time using the same portrait-cover, two-line title/series, author,
   and right-column progress layout as Current Book, and loops in either
   direction. The optional formatted series/index comes from the firmware's
   recent shelf record. The `1/N` count sits at the right of the Continue
   Reading heading; no separate carousel footer is required. Absent books
   are never invented.
2. **Device Statistics:** the previous Overall Statistics dashboard, unchanged:
   Noto Sans 18 monthly reading time, streak, active days, books, sessions,
   qualified page turns, time-of-day profile, and last-seven-days chart.
3. **Current Book:** recent book cover/title/author, optional formatted series
   and index, progress, native per-book sessions/turns/active days/seconds per
   turn and four time-of-day buckets. Unavailable metrics show `--`.
4. **Your Week:** last-seven-days reading chart, time-of-day distribution for
   the recent seven-day native bucket record, and the most recent active day.
5. **Reading Calendar:** matches the firmware's Monday-first numbered grid,
   four activity intensity levels (none, <15m, 15–44m, 45m+), outlines every
   date, and adds a thicker outline for today. A four-level legend sits below
   the grid. Insights are Best Day, Daily Average per active day, and Reading
   Consistency with a percentage bar and days-read count. Tap **Prev/Next** in
   the calendar heading to browse up to eleven previous months.
6. **All Books:** a five-row paginated list of books with valid native stats,
   their titles, authors, and reading time. Previous/Next navigation browses
   the list; no library-sized array is allocated.

As of v0.5.7, all three Time of Day panels share the same font-measured
label/bar layout, with taller cards adding row spacing rather than moving the
bar column. Calendar navigation reserves text-measured width for `Next >`,
Current Book metrics use a narrower icon column, and series metadata may wrap
over two lines when space permits. The All Books footer is shown only when
the library spans multiple pages.

On touch devices, Continue Reading uses the same book-cover/title/author/
progress layout as Current Book. Swipe horizontally inside Continue Reading
to loop through books; outside swipes change statistics pages. Only a centered
position indicator (for example 1/2) appears when multiple books are available.

On non-touch X3/X4 devices, four labels are drawn in a separate bottom
hardware-key footer with widths allocated from measured text so the longer
Prev Book and Next Book captions fit. The touch-only header back arrow is
hidden on hardware-button devices. The shared book hero reserves two title
lines and two series/index lines, with automatic compact-font fallback
rather than dropping a title line. The progress strip is anchored at the
bottom of the metadata column; Current Book reserves enough hero height so
two-line titles and series do not compete with the progress row. The front Left/Right keys loop through carousel books
only if two or more are available. If there is one (or none), they switch
statistics pages instead. The configured side page-turn buttons **always**
switch statistics pages regardless of carousel count. Confirm opens the
selected book and Back exits. Other pages use front keys for page navigation.
The SDK retains the original PREV/NEXT event types, adding a side-source flag
for configured side buttons. Existing apps reading only the event type remain
compatible. A firmware rebuild is needed to enable that source distinction
and the gesture-origin support; the app itself requires only an SD update.

The local preview keeps at most five book paths and metadata records in memory,
with one cover decoded at a time; it doesn't cache library-sized data.
Back returns to the new Overall preview, or exits from Overall. Tapping the Current Book hero
or confirming on that page resumes reading.

## Storage and compatibility

Reads validated native data **read-only**:
`/.crosspoint/reading-stats/global_v2.bin`, `books_v1.bin`,
`time_global_v1.bin`, `book_<hash>_v2.bin` and `time_<hash>_v1.bin`.
Backups are supported. Historical time-of-day data that was not recorded is
never invented.

The dashboard uses Dessen's fixed `DSN_FONT_NOTOSANS_12`,
`DSN_FONT_NOTOSANS_14`, and `DSN_FONT_NOTOSANS_18` font slots to match the native
Reading Statistics typography independently of the current UI theme. The old
`DSN_FONT_LARGE` slot remains compatible. Constrained `OMIT_FONTS` builds fall
back to Noto Sans 14 for the unavailable 12 and 18 fonts. A firmware build from
the updated Dessen and FreeInk SDK worktrees is required; updating only the Rust
app will not enable these new font slots.

The Rust Current Book screen accepts an optional series/index field on the
recent-shelf record, with the existing four-field format supported as a
fallback. The firmware-side metadata capability remains a separate integration
task; do not expect series to appear on every firmware build yet.

**Not yet ported:** the original plugin's **Import Statistics** and
**Server Sync** pages (including pairing, authentication, and bidirectional
session synchronization). These must be implemented with real operations and
appropriate error states rather than placeholder controls.

## Standalone build and tests

In WSL, in this directory:

```sh
source ~/.cargo/env
make test
```

Install `build/app.bin` and `manifest.json` under
`/.crosspoint/plugins/reading-statistics-dessen-rust/` on the SD card.
Inspect on X3 and Picco before changing the existing plugin.
