# Reading Statistics — Dessen Rust dashboard and server pairing

Standalone experimental **Rust/Dessen application** with six offline
statistics pages, Server Sync and Settings pages. It operates without the
older `reading-statistics` SD plugin: the installable app folder contains
`app.bin`, `manifest.json`, and its own `device.json` carrying the
firmware-readable pairing grant. Server totals are cached in
`server-overview.json` in this *same app folder*, and Continue Reading uses
the reader's local book metadata.

For this trial, the firmware takes the application completely out of memory,
silently restarts into the standalone device-code pairing UI (including QR),
saves the token at `/.crosspoint/reading-statistics-token.json`, fetches a
read-only account Overview snapshot, and silently reboots back into Dessen.
The existing firmware device-code and HTTPS primitives are reused; no
legacy SD plugin is required. Dessen itself does not open Wi-Fi or TLS.
The first on-device X3/Picco pairing and reconnect tests are **pending**.

**Standalone pairing/fetch trial (0.6.8):** Every supported device,
including Picco and X3, uses the same reboot-isolated firmware networking.
The Dessen guest writes its resume-page marker and requests a native
operation; firmware releases the guest and silently reboots into a fresh
networking phase. The firmware catalog pairing screen reads **this Dessen
application's own `device.json`**, not the legacy Reading Statistics
plugin. The descriptor contains the production device-code grant, account
catalog, and persistent credential location. After pairing, firmware uses
the protected token to GET `/api/device/overview` into the application
folder's `server-overview.json` atomically. It then silently reboots
again, reopens the same Dessen app, and restores its page. Paired readers
can trigger another download by touching This Month or choosing Refresh
Stats. The guest never holds Wi-Fi/TLS alongside its 80-KiB allocation.
The old SD Reading Statistics plugin is **not required**.
The server's `synced_at` timestamp supports 24-hour refresh-icon age;
failures leave the last successful snapshot intact.

**Disconnect / Reset Pairing (0.6.11):** On the native Server Sync page,
a paired X3 uses **Previous → Disconnect this reader? → Open/Confirm**;
Picco and other touch readers have a separate **Disconnect reader** button
above Refresh Stats. Cancelling returns without changes. Confirming requests
the restricted Dessen native action 2; firmware unloads the VM, silently
reboots to a fresh native operation, calls
`pluginhttp::removeTokenFile("/.crosspoint/reading-statistics-token.json")`
and invalidates saved server overviews for both first-party app names and
the legacy plugin, then silently reboots into Dessen's **unpaired Overall**.
The protected credential remains invisible and undeletable through generic
WebUI/WebDAV. Local reading history is not touched. App build uses
`rustc -C opt-level=z -C codegen-units=1` to preserve sufficient guest
runtime stack headroom on X3; the larger size-optimized-for-speed build
showed corrupted on-screen labels in host tests.

**Automatic server refresh (0.6.12):** Settings is available on all devices.
Auto-fetch is off by default and persists in the app's stats-settings file.
On a normal launch, a paired reader with an absent or at least 24-hour-old
snapshot uses the native GET-only refresh handoff once. A returned app does
not repeat the attempt, even after failure. Settings also offers confirmed
manual refresh, independent of cache age. Side keys/swipes change pages;
front keys on Settings select the toggle or refresh action. Firmware allows
one 10-second saved Wi-Fi join and a 4-second HTTP timeout; invalid or failed
responses leave the old cache intact. No additional SD plugin is required.

**Picco pairing and dashboard corrections (0.6.13):** Server Statistics shows
month and all-time totals. Paired touch readers refresh via the hero or Settings,
not a redundant Server Sync button. Non-touch readers retain Open to refresh
with confirmation. The unpaired Settings action opens Server Sync. Firmware
retries empty or failed Wi-Fi scans twice and keeps the existing Wi-Fi connection
through the native pairing callback, fetching Overview before resuming the app.

**Server Statistics and Web Login (0.6.14):** Unpaired readers start on
Device Statistics. Both navigation directions skip Server Statistics until the
reader is paired. Server Statistics shows `N/A` until an account snapshot is
available and keeps the previous snapshot if refresh fails.

Pairing and every refresh fetch account totals and a new single-use web login
over one Wi-Fi connection after the firmware unloads the guest. The app reads
the cached PIN and renders its QR until the server-issued expiry, up to 24
hours. Generating another login replaces the preceding code. The X3/X4 front
buttons select actions on Server Sync, and the side buttons change pages.
On Picco, use the Generate Web Login and Disconnect Reader touch actions.
The month card and Settings provide manual refresh. Confirmed disconnect
revokes the reader credential and outstanding login, clears the caches, and
returns to Device Statistics. A failed network disconnect leaves pairing
available for retry.

**Server Sync hardware follow-up (0.6.15):** Touch actions have larger
targets and more space between them. Generate Web Login asks before connecting
to Wi-Fi and shows progress before the native handoff. Firmware saves the login
lifetime and last successful statistics fetch against the reader clock. Auto-fetch
runs only when enabled, paired, and the last successful fetch is at least 24 hours
old. Pairing and manual refresh can populate an empty cache.

**Deliberately deferred:** Dessen-owned `sleep.enter` subscriptions.
First verify interactive pairing, cached server totals, native refresh,
cancel, and both silent resume paths on X3 and Picco. The standard declarative
SD plugin's existing firmware primitives remain supported separately.
Firmware still owns any reading-session uploads, not the Dessen guest.

## Pages

1. **Server Statistics:** Paired readers load account-wide Techy-Notes
   statistics (including combined native snapshots and reader sessions).
   The hero splits **THIS MONTH** and **ALL TIME**, retaining minutes even for
   totals above 99 hours. Unavailable server metrics display `N/A`.
   On touch devices the month card opens a refresh confirmation. The refresh
   icon appears after a tap or 24 hours after the last successful server
   update. The server's `synced_at` timestamp survives app restarts.
   Firmware downloads the account snapshot while the guest is unloaded.
   Best Day uses uploaded account history, and Daily Avg. uses the current
   month's active days. The Continue Reading
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
Back returns to Overall, or exits from Overall. Tapping the Current Book hero
or confirming on that page resumes reading.

The app requests **80 KiB of Dessen guest memory** using
`-Wl,--defsym,APP_MEM=81920` in the Makefile. This is an app-header guest
allocation, **not** a raised firmware heap limit. The RV32 code/data and
descending stack share this one block.

**Why 80 KiB?** On X3, repeated attempts to load the earlier 96 KiB app
failed with `[ERR] [DSN] OOM: 98304 bytes of app memory`, while the
largest available contiguous block was approximately 86 KiB. Earlier
pairing-only versions passed host fixtures at 72 KiB, but after adding
server overview and web login, the current build **fails at 72 KiB**
(corrupted text during rendering). The same current Rust static library
**passes both `test_fixture.py` and `test_sync.py` at 80 KiB**.
An even earlier variant failed at 68 KiB. This gives useful headroom for
the tested app, not a proof of stack safety under all real-device loads.

On non-PSRAM boards, the firmware releases rebuildable SD glyph caches before
guest allocation and discards the stored e-ink frame before Wi-Fi/TLS work.
Both changes are required because **opening the app and having enough
remaining heap for HTTPS are different tests**. These memory reclamation
paths still require X3 hardware verification for offline rendering and
the native unload/pair/reload sequence. The VM must be **absent** while
native Wi-Fi/TLS work happens; no attempt is made to lower TLS safety floors.
Picco pairing was previously verified at 96 KiB; the updated 80-KiB app
needs a Picco regression test as well.

See `docs/dessen-reading-statistics-upstream-checklist.md` in the
firmware tree for this decision and every firmware-side integration requirement.

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

**Not yet ported:** the original native plugin's **Import Statistics**.
Server Sync pairing, web sign-in, and read-only aggregate retrieval are
implemented as described above. Native firmware continues to own reading
session uploads and background events, rather than reconstructing them
from offline totals. Additional bidirectional synchronization features
must use explicit tested API operations and
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
