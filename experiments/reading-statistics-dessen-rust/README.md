# Reading Statistics 0.1.14

Full Rust Dessen application: Overall, Device Statistics, Current Book, Your Week,
Reading Calendar, All Books, Server Sync and Settings.

The application uses an 83 KiB guest allocation (84,992 bytes) with a tested minimum 1 KiB stack margin. The shared shelf and 2,304-byte cover/network workspace reduce memory demand; larger history requests continue in smaller resumable batches. It probes the optional native reading-history API rather than
checking firmware identity. Unsupported hosts use the streaming JSONL parser.

Install the `reading-statistics` directory at:

```
/.crosspoint/plugins/reading-statistics/
```

Keep app.bin, manifest.json and device.json together. Native aggregation requires
the matching firmware changes. Use the same package on both firmware families.
The top-level optional_permissions.device_identity grant is ignored by older
hosts. Supporting hosts attach hardware identity only to authorized HTTPS
origins; no physical ID is exposed by the app-scoped device ID API.
The existing runtime and service requirements for pairing and refresh still apply.
Configure a saved Wi-Fi network in firmware before pairing or refreshing.

Installing the plugin authorizes its validated manifest scopes without extra
launch prompts. The manifest declares access to the application's directory,
reading-statistics records and session history, library metadata/covers/progress,
the shared session-sync token and the exact HTTPS origin. A one-use continuation
retains these scopes across networking reboots only if the manifest digest matches.

After successful pairing or a completed history upload and overview refresh, the app opens Server Statistics. Failed or incomplete operations retain their origin page. The Rust application owns device-code pairing, Web PIN requests and persistence,
server-overview validation and caching, manual refresh confirmation, optional
daily refresh and disconnect confirmation. When the optional native source API
is available and the reader is paired, Settings offers Upload & refresh as
the default non-touch selection on every entry. Up/Down follows the displayed
row order, skipping unsupported options; side keys still change pages.
The confirmed action uploads pending history, releases the native query and
refreshes account statistics using the existing Wi-Fi connection. Upload and
refresh results are independent; even partial or already-uploaded checkpoints
refresh the dashboard. Refresh only remains available. Explicit source ownership
confirmation is required. The app owns the Techy-Notes protocol and its checksummed
per-account cursor; firmware provides generic local data only. Never upload
copied reader history as your own. Disconnect clears the shared reader
token and cached server data locally. Local reading records remain.

When session history exists, the application uses the native provider when
available, otherwise it streams JSONL locally. All Books reads indexed metadata
without scanning the complete log on every page. After networking, a released
snapshot is queried again on demand. Undated sessions contribute to lifetime
statistics without inventing dates. No BIN snapshot fallback remains. Native-provider failures are not silently
reinterpreted as stock JSONL. A migrated Razor-Crest baseline needs the native
provider; older hosts can read ordinary stock and v2 session rows. Current Book excludes
recent books whose reported progress is 100% and books with unknown progress. Device Statistics and Current Book use reading duration and average session length rather than local page-turn counts. Overall Statistics retains server-reported page turns. Book titles, authors, series, progress,
covers and navigation retain the existing full application layout.

Front Left/Right select widgets and actions; configured side buttons change
pages. Confirm activates, Back exits from any normal page and cancels dialogs.
Pairing-code generation and enabling Auto Sync require a Wi-Fi notice and
confirmation. A newly paired reader fetches its initial account statistics once,
regardless of the Auto Sync preference. Automatic refresh checks at launch and
when Auto Sync is enabled; it runs only if the cache is missing or at least
24 hours old. Navigation never refreshes it. Without a reliable clock, existing
cached totals are retained. Disconnect works offline.
Touch gestures use the
same coordinate and event contract on both firmwares. The app opts into the
generic five-second timer for pairing checks.

Networking on non-PSRAM devices parks and encrypts the full guest and pending
request, releases its allocation and services, then restarts before transport.
A second clean reboot resumes the exact continuation with the saved response.
PSRAM devices keep the parked guest loaded.
A held Back cancels the worker and closes the app. A socket operation may take
its timeout to acknowledge cancellation; no new guest executes concurrently.

Standalone build and all seven regression suites, from this directory in WSL:

```sh
source ~/.cargo/env
make test
```

Build the SDK host first if needed:

```sh
cd ../../dessen
source ~/.cargo/env
make test RUST=no
make rust
```

Paired calendar snapshots are cached with the current pairing scope; local
history is merged only when ownership and source identity are compatible.
Otherwise the label explicitly says server only. Physical front keys navigate
months, Confirm switches source, and side keys navigate pages. All Books uses
the detailed native record if supported, with cached covers, saved progress
and a historical fallback. Old hosts retain the original compact list.

The size-optimized RV32IM build has been verified with Rust 1.100.0.
It uses call relaxation without GP relaxation, machine outlining and omits
unused panic source-location strings. App-local compact SDK wrappers and RV32IM save/restore helpers reduce code size while preserving the SDK ABI. These Rust compiler switches are
unstable; re-run the full stack-gated suite after toolchain upgrades.
The test package includes the app and its manifest/device metadata. The authoritative source is this worktree. Earlier versioned folders remain unchanged.

Firmware compilation and hardware verification are performed by the user.

## Sleep screen

Select Custom in the firmware sleep-screen settings, with no active Razor-Crest
wallpaper pack. In the app Settings, choose Sleep Screen and a statistics page.
The app captures that page without status or navigation chrome using the generic
screenshot service, stages it in the app directory, and publishes /sleep.bmp.
An original root image is saved as /sleep-backup.bmp. Off restores it when the
active image is still app-owned. Manual replacement and backup conflicts are
reported instead of silently overwriting the user's files. After interruption,
reopen the app to reconcile the journal. Older hosts without screenshot support
hide the selector; the former plugin-local capture mode is not enabled automatically.

Turn Sleep Screen off before uninstalling. Manual removal or external credential
changes while the app is not running cannot trigger app cleanup; external
account changes are reconciled on the next launch. If necessary, restore the
original backup manually. A selected Razor-Crest wallpaper pack retains priority.
No new server service is needed.

## Book list preferences

Open a book row to open it (with confirmation), delete it from the list until
new reading activity or changed live progress is detected, or hide it until
restored through Settings > Manage Hidden Books. These are device-local
preferences, not changes to the EPUB, reading history or server totals.
The bounded preference file supports up to 96 exclusions; the hidden list
shows excluded books still present in the native history index.
Sleep selection uses full titles in the picker and short names in Settings.
Books Local is not a sleep page; an obsolete selection is disabled through
the same wallpaper restoration procedure as Off.