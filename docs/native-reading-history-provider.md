# Native reading history provider

This is the read-only, stock-JSONL implementation stage of the native statistics proposal.
The Dessen adapter and Rust Reading Statistics UI are integrated. The SD script adapter remains separate.

## Interface and ownership

- `src/util/ReadingHistory.h/.cpp`: host-testable incremental query.
- `src/util/ReadingHistoryProvider.h/.cpp`: one-task HAL reader. `begin(document, anchorDay)`
  opens the standard history and captures its length. Call `step()` cooperatively; each
  step consumes at most 4096 bytes, and `cancel()` releases resources. A completed query
  exposes its summary, calendar buckets and sorted document IDs until reset or cancellation.
- Pass the same 32-hex document identifier used by the session recorder to select a book.
  Omit the ID for an overall-only query. The anchor is a local Unix day number (not the
  legacy BIN ordinal); zero selects the latest recorded valid local date. The caller must
  distinguish that inferred date from a trusted current date.
- Lifetime totals and date-known counts are 64-bit; invalid or missing timestamps/offsets
  contribute to lifetime totals but not dated metrics. Average session duration is derived
  from duration/session count. The calendar provides 371 days; active-day counts refer
  to this window, not lifetime. A separate seven-day ring retains weekly time-of-day
  totals. No timezone or RTC value is invented.
- The index retains the smallest 1024 distinct binary document IDs and explicitly marks
  overflow. Metadata pages use indexed latest-session offsets, without rescanning. Offsets beyond 4 GiB have unavailable metadata.
- Complete newline-terminated JSON objects only, maximum 511 payload bytes plus optional CR.
  Skip invalid lines, count rejected records, and flag an unfinished tail. Non-session
  record types are ignored except the explicitly versioned Razor-Crest historical
  checkpoint described below. That checkpoint is an aggregate, never a measured session.
- The source JSONL is read-only: no cursor changes, sync calls or legacy BIN reads.
  The derived checkpoint is published separately. The source is assumed
  append-only during an active query. Appended bytes beyond the captured length are
  included on the next query; early EOF or read errors invalidate results. Reopening
  verifies the entire cached source prefix, including same-size replacements.
  Concurrent in-place source modification and USB mass-storage coordination during
  an active query remain unsupported.
- Allocation is lazy and fallible. A document block is 1280 bytes (64 IDs and offsets), up to
  sixteen blocks; a separate 371-entry day ring, 512-byte line, 4096-byte read buffer,
  and ArduinoJson's filtered parser allocations coexist while scanning. Parser and read
  buffers, day ring and all index blocks are released before reporting Ready.
  Result pages are then read from the verified SD checkpoint. A bounded wire page
  uses up to 1024 transient bytes; metadata pages have their existing bounded scratch.
  Cache publication and restoration of at most 36,832 bytes are synchronous; the
  time they hold the activity lock still needs hardware measurement.
  The Dessen VM is parked while the activity services the scan on its loop task,
  in slices of at most four chunks with a 12 ms soft time budget. A single SD operation
  can exceed that budget. While the query is pending, the activity prevents idle CPU
  throttling and auto-sleep, skips the usual UI-loop delay, and yields one RTOS tick
  between slices. Long queries display a native progress bar after about one second
  and refresh it no more than once per five seconds. Hold Back to exit; activity
  teardown cancels the query and closes its file.
  The service logs start, progress every two seconds, ready, failure and cancellation
  with byte/session counts, elapsed time, CPU MHz and heap where applicable.
  The 1,024-book index remains unchanged.

## Dessen API

The two calls are appended as syscall numbers 108 and 109 in the Dessen ABI.
`dsn_reading_stats_read(0, 0, empty)` probes availability (version 1).
Use the existing read permission for `/.crosspoint/reading_sessions.jsonl`;
no new manifest key is required. A selected book can be a 32-hex document ID
or an authorized library path resolved using the configured sync match method.
Changing that method can split history into different document IDs.

See `dessen/docs/writing-apps.md` for wire format and error codes. The result
is owned by the requesting service; it is released explicitly, on another query,
before networking, or when the app exits. The API does not expose any account
or network data. The checkpoint follows normal filesystem permissions; no additional protected path is reserved.
The existing v0.1.15 application ABI and package do not change.

## Verification

Host test (from this worktree, WSL; uses the pinned ArduinoJson dependency already present
in the local firmware build environment):

```sh
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -O2 \
  -I/mnt/c/XP-FW/work/src/.pio/libdeps/gh_release/ArduinoJson/src \
  -Itest/reading_stats/fake_hal -Ilib/Memory -Ilib/Utf8 -Isrc/util -Isrc -Idessen \
  test/reading_stats/ReadingHistoryTest.cpp src/util/ReadingHistory.cpp \
  src/util/ReadingHistoryProvider.cpp lib/Utf8/Utf8.cpp -o /tmp/rc-reading-history-test
/tmp/rc-reading-history-test
```

Optionally pass the four synthetic fixture paths in this order:
1,000 sessions/10 books, 10,000/10, 100,000/10, 100,000/1,000.
The verified fixture copies and hardware evidence are in
`C:/Users/Admin/Downloads/CrossPoint-native-stats-evidence-20261126/fixtures/`.

Host tests verify date/timezone calculations, selected book, out-of-order dates,
case-insensitive IDs, unknown time, malformed and oversized lines, incomplete tails,
ignored baseline records, bounded distinct IDs, empty/missing file, cancellation,
append boundary, early EOF, successful retry, and bounded little-endian wire
serialization. All four fixtures pass cold build, unchanged reuse (zero parsed bytes),
and append-only parsing, with identical wire summaries, day buckets and book offsets
against a full parse. Tests also cover key changes, same-size replacement, corrupt
or truncated caches, partial JSONL tails, cancellation, failed publication and retry.
Every ready provider has zero retained aggregation buffer bytes. The Dessen `make test` suite exercises
the unsupported-host path and a parked native statistics query with a mock host.

## Integrated X4 observations (2026-11-26)

Original v0.1.15 integration, before the power/scheduling fix:

| Log | Sessions | Books | Native elapsed | Outcome |
|---|---:|---:|---:|---|
| 12 | 28 | 2 | 34 ms | Ready |
| 13 | 1,000 | 10 | 751 ms | Ready |
| 14 | 10,000 | 10 | 7,313 ms | Ready |
| 15 | Not confirmed | Not confirmed | >108 s observed | No ready message |

Log 15 showed stable free heap 24,392 bytes and largest block 15,348 bytes. The
application was still blank after more than two minutes according to the user.
The logs do not record CPU frequency or processed byte counts, so they cannot
establish the cause of the delay. Source inspection found the activity did not
prevent idle power saving: non-PSRAM CPU speed can drop to 10 MHz after 3 seconds
of inactivity. The revised activity keeps normal speed during native queries and
replaces the ordinary loop delay with a bounded RTOS yield. Repeat the identical
fixtures and compare start/progress/ready timings, CPU speed, minimum heap and
actual dashboard readiness. No new hardware result is claimed yet.

After the power/scheduling fix (logs 16 and 17), both 100,000-session files
completed at 160 MHz with zero rejected records. Ten books: 60,883 ms, free
33,208 B / largest block 24,564 B at Ready, zero font errors. One thousand books:
61,076 ms, free 12,980 B / largest block 4,340 B at Ready, 1,848 font allocation
errors (5,334–7,738 B requested). The retained index and calendar compete with
rendering memory. The derived checkpoint now separates those lifetimes.

## Derived checkpoint (RHISTC04)

`/.crosspoint/reading_sessions-history.cache` is disposable; no statistics are lost
if it is removed. The only authoritative source remains reading_sessions.jsonl.
The cache is keyed by selected document and requested date anchor. A changed key,
truncation, invalid header/checksum, or modified source prefix rebuilds from JSONL.
The entire previously processed prefix is read in bounded chunks and checked with
FNV-1a 64-bit before reuse (noncryptographic integrity, not authentication). Warm
launches still read those bytes; they do not parse them. Matching complete-prefix
checkpoints resume aggregation only on newly appended bytes. A partial final line
requires a cold rebuild if the file grows. Unchanged queries do not rewrite the
cache. The cache is written to a temp file, closed, replaced through the HAL and
reopened with length/checksum validation. Interrupted publication never makes the
source dependent on the cache. Old temp/backup files are not accepted as results.
One cache slot is retained: changed book/anchor, including a different explicit
anchor after networking, can require a cold scan. SD write failure returns an I/O
error, leaving source data intact. Hardware power-loss tests are still pending.

Logs report parsed_bytes, phase (verify or parse), and scratch_bytes at Ready.
The latter counts the aggregation arrays and input buffers, not all service or
HAL allocations. Log 18 confirmed the 100,000-session/1,000-book cold scan in 64,628 ms and warm
verification in 14,151 ms, with 43,428 B free and 24,564 B largest block at cold
Ready, zero retained aggregation buffers and zero font allocation errors.

## Razor-Crest historical migration rehearsal

The stock history parser and cache do not depend on the migration. The following
Razor-Crest-only extension belongs in a separate downstream patch when preparing
the generic native-provider PR.

`scripts/rehearse_reading_history_migration.py SOURCE NEW_OUTPUT --match filename`
operates on a copied native RST2 directory and original JSONL. It verifies
snapshot/catalog checksums, path hashes, source sequence, known document mapping,
time profiles, complete input boundaries and absence of pending transactions.
It refuses ambiguous filename IDs, orphan snapshots and corrupt/missing-primary
records requiring recovery. It never imports another firmware's files or deletes
the source. Binary matching is not yet implemented by this rehearsal tool.

The output copies the old JSONL prefix byte-for-byte, then appends a versioned
aggregate cutover block:

- `rc-baseline-begin`: record count; local pre-boundary aggregation is reset.
- `rc-baseline-total`: one global total, then one per catalog document; seconds,
  sessions, qualified turns, latest known Unix day, lifetime periods, and optional
  book metadata including last observed progress.
- `rc-baseline-day`: global or document day and seconds.
- `rc-baseline-week`: global or document day and four period totals.
- `rc-baseline-end`: FNV-1a 32-bit checksum of every intervening UTF-8 record
  including its LF terminator.

All records have v=1 and are at most 511 bytes before LF. The global records omit
doc; per-book records use the normal 32-hex document ID. The native query refuses
partial, corrupt or duplicate cutover blocks and publishes no partial results.
The baseline replaces, rather than adds to, local aggregation of the earlier
session prefix. It is an absolute historical checkpoint of the native BIN store,
not a fabricated set of individual events. Existing global totals can differ
from the sum of per-book totals (for example, when book-specific history was
removed). Preserve both as stored. Known dates are retained; no historical
start times or count of dated sessions are invented. Page turns and per-book weekly buckets are preserved in JSONL. Kind 6 of the
native read syscall returns a 24-byte optional turns record: u32 version 1,
u32 completeness flags (overall bit 0, selected book bit 1), u64 overall turns,
u64 selected-book turns. The original 192-byte summary remains unchanged. Stock sync skips the baseline rows and keeps old session IDs and
byte offsets. A source cursor is copied unchanged if present; the copied Picco
history had no cursor.

The real Picco backup rehearsal (2026-11-26) contains 94 global sessions,
90,914 seconds, 3,587 qualified turns and four catalog books. Its existing JSONL
contains 28 measured events and 24,664 seconds. Both source and converted output
remain available. Host tests compare every global/selected-book total, day and
period against the source-derived report, check metadata, warm cache and appended
sessions, reject incomplete/corrupt/duplicate baselines, and verify the stock
upload excludes aggregate rows.

## Automatic device cutover

The boot migration recovers pending native transactions, validates source
snapshots and document identities, and streams a candidate containing the exact
old JSONL prefix followed by the baseline. It reopens and parses the candidate,
checks the historical totals and checksummed baseline, then publishes a receipt
with old/new byte lengths and hashes. Only the verified candidate may replace the
canonical log. The original upload cursor is untouched. After verifying the
published prefix, it moves existing cover previews where possible and removes
the Razor-Crest legacy directory. An interrupted rename or cleanup is recovered
on boot. A missing or damaged candidate can restore the verified original when
the canonical file is absent. Unresolved identities and corrupt inputs block
conversion. The old recorder is retained only on a failed pre-publication
migration so existing data is not lost; an unresolved publication blocks opening
the reader. No BIN reader or writer participates after successful cutover.

New v2 rows store tracked_secs (total local active time), turns, author, series
and series_index where available, retaining stock secs and start fields. Undated
sessions use start 0 and are never uploaded to the stock endpoint. Local totals
include them; dated charts and stock uploads use dated seconds only. The native
SD dashboard and sync comparison use the same provider. The legacy import and
per-book BIN removal actions are disabled; append-only history deletion requires
a separate design. Dessen 0.1.16 has no BIN fallback. Ordinary stock logs are
supported on older runtimes; migrated aggregate histories require the native
provider to prevent silently undercounting.

Log 19 confirmed the converted 94-session/4-book history on X4: 122 ms cold,
25 ms warm, 43,428 B free after cold aggregation, zero font errors. Host tests
cover real Picco automatic migration, transaction recovery, extended durations
and turns, and cache consistency. Automatic cutover itself still requires
testing on the copied source SD data before deploying to a live reader.

## Remaining integration

Real-device automatic migration, power-loss and append tests, extended server
synchronization, and upstream separation of the Razor-Crest migration extension.
The 1024 distinct-book display limit remains accepted; larger histories still
contribute to lifetime totals.
