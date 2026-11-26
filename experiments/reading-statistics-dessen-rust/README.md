# Reading Statistics 0.1.16

Full Rust Dessen application: Overall, Device Statistics, Current Book, Your Week,
Reading Calendar, All Books, Server Sync and Settings.

The application uses an 85 KiB guest allocation (4 KiB above 0.1.14) to retain
stack headroom. It probes the optional native reading-history API rather than
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

The Rust application owns device-code pairing, Web PIN requests and persistence,
server-overview validation and caching, manual refresh confirmation, optional
daily refresh and disconnect confirmation. Disconnect clears the shared reader
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

Standalone build and all five regression suites, from this directory in WSL:

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

The test package includes the app and its manifest/device metadata. The authoritative source is this worktree. Earlier versioned folders remain unchanged.

Firmware compilation and hardware verification are performed by the user.
