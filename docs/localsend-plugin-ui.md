# LocalSend native SD-plugin UI (Razor-Crest)

The receiver is enabled in regular Razor-Crest firmware builds; its File
Transfer entry still requires the separately installed LocalSend SD plugin.
It is not part of the generic on-device JavaScript runtime or ELF loader.

## Contract

Firmware built with `CROSSPOINT_LOCALSEND=1` advertises
`transfer.localsend.v1` in `pluginCapabilities`. This capability means
the firmware can host bounded multi-file EPUB/PNG/JPG/JPEG/BMP receives
and the protected HTTP/discovery/storage implementation.

The Local Send entry under **File Transfer** appears only if:

- the firmware has this compiled capability;
- the global Plugin System switch is on;
- a discoverable, non-installing plugin named `localsend` has a valid
  `manifest.json` with matching `name`, plus a `device.json` containing
  `native.type = "localsend.v1"` and
  `native.capability = "transfer.localsend.v1"`;
- `native.folders` declares 1–6 distinct, bounded and validated destination
  presets, with display labels.

The host reads the plugin's `title` and folder choices, supplies the
reader's native list and confirmation widgets, and runs the existing
LocalSend protocol implementation only while that screen is active. The
plugin does not execute on-device JavaScript and cannot add new protocol
routes or override storage protections.

The six provided presets are **SD Root** (`/`), **Books** (`/Books`),
**LocalSend** (`/LocalSend`), **Books/LocalSend** (`/Books/LocalSend`),
**.sleep** (`/.sleep`) and **.sleep-overlay** (`/.sleep-overlay`).
Paths under `/Books` are supported, while the additional destinations
are explicit exceptions; arbitrary hidden
or system directories, traversal, malformed paths and overlong paths are
rejected. The selected destination is remembered as `destination` in the
`/.crosspoint/localsend/localsend.json`. Only choices in its installed manifest
are shown. No arbitrary-directory browser is exposed. The saved state
is independent of the installed plugin and survives its removal.

The native UI has three tabs: **Receive** shows the listening address,
a compact dividing line, and up to 16 successful files from the current
session; **History** browses SD-backed received-file records (up to the
UI toolkit's 32,767-row maximum), using plain list, thumbnail cards or a cover grid; **Settings** controls the
destination folder, Device ID, History Layout, List layout thumbnails and grid
columns/rows plus **Clear History**. Clear History asks for confirmation and
only removes the SD-backed history index; it leaves every received file
and the current Receive tab's filenames intact. New installations default
to list without thumbnails, and a
2-column / 3-row grid. Filenames wrap to three lines. Tapping a
filename shows a dialog with the filename as a title, and the complete
saved directory on its own line, followed by actions. In **Receive** the options are
**Open**, **Delete**, **Close**. In **History** they are
**Open**, **Remove from History**, **Delete**, **Close**.
Opening uses the firmware's existing reader/image dispatch; deleting
uses the standard EPUB cache/clipping cleanup (plain storage removal
for images) and requires confirmation. **Remove from History** changes
only the plugin's history, never the actual saved file.
The Receive tab is rendered before the listener is started, so the device
does not advertise itself before its screen is displayed. The listener stays
active on the LocalSend tabs, pausing while the shared keyboard is open
for editing the Device ID. An empty Device ID uses the
runtime board model (for example `Razor-Crest(X3)` or `Razor-Crest(X4Pro)`);
a custom ASCII identifier such as `User_Custom_ID` is advertised as
`Razor-Crest(User_Custom_ID)` in both HTTP info and multicast discovery.
Opening an EPUB replaces the plugin with the reader and releases Wi-Fi.
Opening an image pushes the existing viewer on top of LocalSend; closing it
returns to the plugin, which restarts the listener if Wi-Fi remains connected. On the X4Pro, this path shuts down Wi-Fi in place instead
of triggering the normal File Transfer exit restart; the reader or image
viewer opens without returning to Home.

Destination, Device ID, and display settings are stored in the bounded
`/.crosspoint/localsend/localsend.json` file. The same state file stores
one randomly generated receiver fingerprint so discovery and Favorites keep the
same identity across restarts. The receiver will not advertise a new fingerprint
if its initial state write fails. The received-file index
is independent: `/.crosspoint/localsend/history.bin` holds fixed-size
SD records (folder and filename), newest-first when browsed. Only the
small visible window is loaded into RAM. Remove/Delete rewrites this
index through a temporary file and backup with interrupted-edit recovery;
the actual books are never modified by Remove from History. The only
history ceiling is 32,767 entries from FreeInkUI's signed row indexes.
This unreleased-format change intentionally does not migrate older JSON
`names` records or plugin-local `settings.json`/`recent.json`.
Native display settings mirror the Send2Ereader grid/list options and
use the shared cover-grid and book-card widgets. Thumbnails are drawn
only if the firmware already cached an EPUB cover; no new thumbnail
decoder runs during uploads. Only successfully published files appear
in either list. A temporary settings file and backup protect preferences
during writes. Existing files remain protected by
the same staging, checksum, exact-size, close-before-publish and duplicate
rejection rules.
A duplicate filename is rejected at Prepare with HTTP 403, the on-device
log identifies the existing destination path and the sender receives
`File already exists in destination folder`. A duplicate created during
the consent window is also rejected without overwriting it. The potentially
slow SD free-space scan starts only after Accept, with the consent dialog
replaced by a non-interactive "Checking SD card space..." notice while the
scan runs. The notice clears after the check, with no fixed duration promised;
Decline does not scan the card. An accepted offer that cannot fit is rejected
with HTTP 507 before upload credentials are issued.

Leave with Back or Home to stop TCP/UDP listening, discard any pending
offer, and close the network activity. Ordinary File Transfer / hotspot
and Calibre do not start the LocalSend listener. Incoming traffic is
HTTP (not TLS); use a trusted LAN.

## Diagnosing a Windows LocalSend TCP reset

A sender error such as `os error 10054` means the TCP socket closed
unexpectedly; it is **not** an HTTP 403 or 422 response. Keep the X4Pro
serial monitor active during the same attempt and capture the lines from
`Waiting for transfer acceptance` through the subsequent upload failure,
including any ESP-ROM restart marker. The receiver logs:

- `Upload start`: the authenticated upload reached its body handler;
- `Chunk header failed`, `Chunk size invalid`, or `Upload exceeded chunk limit`:
  rejected or interrupted HTTP chunk framing;
- `Upload read stopped`: transferred bytes, expected bytes, whether TCP
  still reported connected, and elapsed upload time;
- `File signature mismatch` or `SHA-256 mismatch`: the body arrived but
  failed final validation;
- `Could not write HTTP ... response`: the peer did not receive a complete
  HTTP response.

A previous failed transfer consumes its token. A subsequent 403 for that
token is expected; use a new offer after diagnosing the first failure.
This test receiver retains the existing safety limits and does not yet
claim to handle all desktop sender/network failure modes.

## EPUB rejected during Prepare

If a JPG transfers and opens correctly but a subsequent EPUB fails with
`Prepare rejected: HTTP 400`, the EPUB bytes have not been uploaded.
Prepare validates 1–16 files per offer, allowed basenames up to 128 bytes,
4-byte to 1-GiB file sizes, SD free-space availability and optional SHA-256 metadata. The
receiver now records the specific failed validation in its `[LSEND]`
serial message and returns a corresponding JSON `message` to the sender.
Do not assume the previous JPG or receiver session state caused this error.
The host suite verifies that a valid EPUB still transfers successfully
after a successful JPG and an unrelated invalid EPUB offer. For a real
device failure, capture the specific Prepare rejection line.

## Local X4Pro test (human build/flash)

1. Run `tests\\localsend\\build-x4pro.cmd` in the **localsend-receiver**
   worktree with the existing `C:\\XP-FW` builder, then flash manually.
2. Copy the contents of the standalone repository's
   `localsend.crosspoint-plugin/` payload into the SD card folder
   `/plugins/localsend/`. Enable
   **Settings → Connectivity → Plugins**.
3. Open **File Transfer**. Check **Local Send** appears immediately below
   **Join Network**, followed by Calibre, Hotspot and (where available)
   USB Drive. With the Plugin System disabled or the plugin directory
   absent, Local Send must be hidden and the original menu order retained.
   On X4Pro, confirm the USB Drive confirmation still targets USB Drive.
4. Select **Local Send**, join the same Wi-Fi as the official app, and
   check the **Receive** tab shows `Listening for files`, the IP/port,
   current-session files, with a separate **History** tab.
5. Send one new EPUB using the official LocalSend app, then attempt a
   700 MiB graphic-novel EPUB on a card with sufficient free space.
   Accept it on-device and verify the exact bytes in
   `/Books/LocalSend/`, then open the book. The filename must appear
   under Receive and the list on History.
   Verify three-line filenames and tap to inspect the full name and saved folder.
6. Re-send that filename: the receiver must refuse it and preserve the
   existing file, without adding an entry. Decline another offer;
   do not create or log it as saved. Check sender cancellation, consent
   timeout and Back/Home while awaiting acceptance.
7. Verify all six folder presets. Save separate named EPUB, PNG, JPG
   and BMP files to their intended folders; check `/`, `/Books`,
   `/LocalSend`, `/Books/LocalSend`, `/.sleep`, and `/.sleep-overlay`.
   Verify their signatures and bytes, duplicate refusal and failure cleanup.
   Exit and re-enter: selected destination and History persist, while
   the Receive list resets. Details for older files still
   show their original saved folder.
8. Tap an EPUB and choose **Open**: it should open in the reader without
   a software restart. The serial log should show
   `Opening received file:` followed by the reader or image-viewer entry,
   with no intervening ESP-ROM restart.
   Tap PNG, JPG and BMP files: each must open in the existing image viewer;
    press Back and confirm the LocalSend screen returns (not File Browser),
    and that the listener resumes. Verify X3 memory while Wi-Fi remains connected.
   From the history tab choose **Remove from History** and verify the
   saved file remains; leave and re-enter and confirm the history entry
   stays gone. Test **Delete → Cancel** (file intact) and **Delete → Delete** (file removed from disk and both lists). Verify a
   destination settings in `/.crosspoint/localsend/localsend.json` and
    SD-backed history in `/.crosspoint/localsend/history.bin`
   after uninstalling and reinstalling the plugin. Temporary `.part`
   files share the directory and are removed after normal transfers.
   Capture `LSEND` lines to diagnose any rejected transfers.
9. Exit the screen and try sending from the official app: the reader
   must no longer be discoverable as an active receiver or accept
   uploads. Reopen the screen and confirm listening resumes.
10. Exercise Receive/History switching by touch, Confirm on the tab bar
   and navigation buttons; verify Back closes the screen without leaving
   the listener active. Exercise remapped buttons, portrait, inverted,
   and both landscape orientations,
   a long transfer interrupted by Back/Home, and record heap/free block
   and flash-size measurements. No other File Transfer modes should
   advertise LocalSend.

## Multi-file and low-memory validation

The receiver accepts one approval for **1–16 files**, up to **1 GiB per
file**. Batch metadata uses 64-bit total size accounting and SD free-space
preflight. Each file has its own random token. Uploads are
processed serially on the embedded listener, though the official app may
start parallel TCP requests. Successfully published files remain safe if a
later upload fails; subsequent pending tokens are invalidated on failure.
Duplicate filenames (case insensitive) within a batch, or files already
present at the destination, are rejected before consent. Folder mode
uses slash-separated relative paths and is explicitly rejected until safe
nested directory creation and history navigation are designed.

The same streaming receiver is available on the X3 and original X4 (ESP32-C3
without PSRAM). Regular Razor-Crest builds enable the receiver in
`platformio.ini`. The user confirmed an X3 hardware test receiving
16 files in one batch successfully. Large (~700 MiB)
files, interruption and X4 hardware remain unverified. The X4Pro is ESP32-S3
with PSRAM. Use
`tests/localsend/build-c3.cmd X3` or `build-c3.cmd X4` with the
`C:\XP-FW` builder for C3 tests; only the user builds/flashes firmware.
Test a small three-file batch on each device, then a 16-file batch,
cancel mid-batch, retry a completed token, reject a duplicate, and confirm
memory/free-block readings remain stable. Also confirm individual-file
uploads, destination persistence and opening received files still work.
On X3/X4, observe whether parallel sender requests time out behind the
serial receiver before attempting larger batches.
