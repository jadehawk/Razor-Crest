# Reading Statistics 0.6.31

Full Rust Dessen application: Overall, Device Statistics, Current Book, Your Week,
Reading Calendar, All Books, Server Sync and Settings.

The application keeps its 80 KiB guest allocation. It uses the same generated
Dessen ABI on CrossPoint and Razor-Crest. There are no firmware identity checks,
native statistics launch calls or application resume-page markers.

Install the `reading-statistics` directory at:

```
/.crosspoint/plugins/reading-statistics/
```

Keep app.bin, manifest.json and device.json together. This revision requires the
matching Phase 3 firmware changes; earlier experimental firmware is incompatible.
Configure a saved Wi-Fi network in firmware before pairing or refreshing.

Installing the plugin authorizes its validated manifest scopes without extra
launch prompts. The manifest declares access to the application's directory,
reading-statistics records and session history, library metadata/covers/progress,
the shared session-sync token and the exact HTTPS origin. A one-use continuation
retains these scopes across networking reboots only if the manifest digest matches.

The Rust application owns device-code pairing, Web PIN requests and persistence,
server-overview validation and caching, manual refresh confirmation, optional
daily refresh and disconnect confirmation. Disconnect revokes the server login
before removing the shared token and app caches. Local reading records remain.

When session history exists it supplies local statistics, including undated
sessions and devices without an RTC. Otherwise the application reads validated
native binary snapshots and backups. Device Statistics and Current Book use reading duration and average session length rather than local page-turn counts. Overall Statistics retains server-reported page turns. Book titles, authors, series, progress,
covers and navigation retain the existing full application layout.

Front Left/Right select widgets and actions; configured side buttons change
pages. Confirm activates, Back exits from any normal page and cancels dialogs.
Pairing-code generation and enabling Auto Sync require a Wi-Fi notice and
confirmation. A newly paired reader fetches its initial account statistics once,
regardless of the Auto Sync preference. Automatic refresh checks at launch and
when Auto Sync is enabled; it runs only if the cache is missing or at least
24 hours old. Navigation never refreshes it. Without a reliable clock, existing
cached totals are retained. Disconnect paints a waiting screen before
networking. Touch gestures use the
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

The versioned package includes the application and a matching source tree under
`source/experiments/reading-statistics-dessen-rust`, together with its SDK and
the FreeInkUI headers needed for desktop tests. Earlier versioned folders remain
unchanged.

Firmware compilation and hardware verification are performed by the user.
