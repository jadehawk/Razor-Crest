# LocalSend receiver experiment

This downstream Razor-Crest experiment is based on 2325388721cf57d155498d54610fe8489b3e0a34,
branch feat/localsend-receiver. It contains no native ELF experiment changes.
The native-elf-poc worktree and original razor-crest checkout are preserved.
Pinned SDK: 5e94fe7480f019c89fbf691aef87523eb1b46704;
lucide: c81680e066f45b640743ca78ae36cdedda3f0318. Gitlinks are unchanged.

## Current stage: plugin-owned UI

For the **current** X4Pro build/flash and plugin installation instructions,
see [LocalSend native plugin UI](../../docs/localsend-plugin-ui.md).
The original no-plugin single-EPUB test steps below are retained for
historical reference; use the linked native plugin guide for current tests.

## First local build and transfer (historical receiver test)

From this worktree, run tests\localsend\build-x4pro.cmd. The wrapper invokes
the existing C:\XP-FW builder for X4Pro with this exact source path.
CROSSPOINT_LOCALSEND=1 is enabled in the shared Razor-Crest platformio.ini,
so no one-off environment overrides are needed. It does not build or flash
until the human runs it. The canonical
builder performs its normal clean target build and copies outputs into
C:\XP-FW\out\latest\X4Pro\. No toolchain infrastructure is modified.

Worktree:
C:\Users\Admin\Documents\Github Working Repos\crosspoint-reader-(Razor-Crest)\.thinkforge-worktrees\localsend-receiver

1. Build with that wrapper and flash the resulting X4Pro firmware using the normal process.
2. On the reader, open File Transfer and Join a Network.
3. Put the sender on the same local Wi-Fi network. Keep File Transfer open.
4. Open LocalSend on the sender. The reader announces itself as Razor-Crest.
5. Send ONE EPUB, at most 32 MiB, with a new filename. Accept it on the reader.
6. Find it at /Books/LocalSend/<filename> on the SD card. Open it in the reader.
7. Repeat with another filename. Sending an existing name must fail and preserve
   the existing file. Back/Home exits File Transfer and stops the receiver.

That initial receiver-only test predates the final SD plugin integration.
Current Razor-Crest builds include the native receiver by default, but the
Local Send menu appears only when the compatible SD plugin is installed and
enabled. It does not use or grow the native ELF API.

The first receiver deliberately advertises HTTP, not HTTPS. Transfers are
unencrypted and require acceptance on the reader while File Transfer is open.
Use a trusted test LAN. PIN support and HTTPS are not implemented.
No new settings, activities, tasks, radio drivers or dependencies are added.

If multicast discovery is blocked, use this direct HTTP test from PowerShell:

```powershell
C:\Python314\python.exe .\tests\localsend\send-epub.py 192.168.1.123 "C:\path\to\book.epub" --name localsend-test.epub
```

Replace the IP with the address shown in File Transfer. The script supplies the
file SHA-256, streams the file, and requires HTTP 200 after verification.
The agent has not run this command against a device. Its PASS indicates a server
acknowledgement; compare the received file independently for the device test.

Serial logs use LSEND:
- HTTP receiver ready at <ip>:53317
- Saved /Books/LocalSend/<filename> (<bytes> bytes)

If the phone app does not discover it but the script succeeds, collect the sender
OS/app version and whether the LAN permits multicast UDP 53317. No discovery
or live-client interoperability has been claimed before device confirmation.

## Protocol and boundaries

Protocol reference: https://github.com/localsend/protocol/blob/main/README.md
(inspected LocalSend v2.2 specification). The receiver implements the v2 info,
register, prepare-upload, upload and cancel endpoints. UDP discovery uses
224.0.0.167:53317, with HTTP on TCP 53317. It sends periodic announcements and
bounded multicast replies; direct IP can bypass multicast.

One accepted offer can contain 1–16 files (EPUB/PNG/JPG/JPEG/BMP), up to
1 GiB each, with 64-bit combined-size and SD-space checks. The receiver
uses a 128-bit random session ID and
independent 128-bit file tokens bound to the sender IP. Individual files are
processed sequentially and streamed in 1024-byte buffers, without requiring
PSRAM. A completed file token cannot replay; the session stays open for other
files and expires after two idle minutes. Failure aborts outstanding tokens,
but does not delete successfully published files.

Only safe basenames are accepted: no directory paths, control characters,
leading dots, reserved names or URL-encoded path syntax. Names and file IDs
are capped at 128 bytes. Folder uploads with relative paths are currently
rejected explicitly. EPUBs must begin with ZIP local-file magic; this is not
full EPUB validation. Images must have their corresponding magic signatures.

Metadata uses explicit Content-Length. Uploads accept either Content-Length or
Transfer-Encoding: chunked. Duplicate/conflicting framing headers, encoding
chains, malformed tokens, oversize metadata and unsupported methods/routes
are rejected. Expect: 100-continue is supported after upload authorization.
The parser accepts HTTP/1.0 and HTTP/1.1 and closes every connection after reply.

The private staging file is /.crosspoint/localsend/<file-token>.part,
exclusively created. /.crosspoint/localsend/localsend.json holds preferences,
while /.crosspoint/localsend/history.bin is an SD-backed random-access
history of up to 32,767 records. Only a 16-entry window is materialized for
the grid/list UI. Neither is inside the uninstallable plugin. Exact length, full SD
writes, SHA-256 when offered, and file close must succeed before a
non-overwriting rename publishes each file. Existing books are
preserved. Failure closes/removes this attempt's staging file. Card errors may
prevent cleanup; a power loss may leave a private .part file. FAT rename is not
claimed power-loss atomic. Tokens and filenames are never used as paths without
validation. Session tokens are not logged.

The listener belongs to LocalSendPluginActivity, which paints the Receive tab
before starting the receiver, polls it while active, and uses its Back/Home
cancellation check. Images open the existing image viewer as a child activity
and return to LocalSend when closed; EPUBs retain the direct reader navigation.
The plugin offers a model-based advertised alias, optional validated custom
Device ID, per-tab History selection/scrolling, and the latest 16 session filenames. The custom-ID keyboard
temporarily pauses reception and uses the existing keyboard activity.
No receiver thread remains after exit. LocalSend stops before Wi-Fi shutdown.
Join Network is the initial supported mode; hotspot reception is not enabled.

## X3/X4 build handoff (no firmware built)

Use `tests/localsend/build-c3.cmd X3` or
`tests/localsend/build-c3.cmd X4` from this worktree. Both use the existing
`C:\XP-FW\build-fw.ps1` and inherit the default-enabled receiver flag
from `platformio.ini` (without adding duplicate command-line defines). The original X3 and X4 share the ESP32-C3 firmware family
with no PSRAM; X4Pro is ESP32-S3 with 8 MiB PSRAM. The user confirmed
16 files received in one batch on X3. Large-file transfers, interruption,
X4 transfer tests, navigation changes and memory peaks remain unverified.

## Resource budget and evidence

No firmware compilation, flash, size measurement, commit, merge or push was run
by the agent. Flash cost and physical internal/PSRAM peaks remain unmeasured.
Do not reuse the 67 KiB ELF diagnostic delta as a LocalSend size estimate.

The bounded 16-file host session is 5936 bytes and request state is 208
bytes. Firmware alignment may differ. There is no whole-book buffer. A
2049-byte header allocation is freed before metadata is read (maximum
8193 bytes). Uploads use a 1024-byte stack buffer plus SHA-256 state.
JSON documents have a checked 16384-byte allocation budget, including
allocator bookkeeping; the larger acceptance response is heap-allocated
to avoid consuming stack space. The SD history index uses fixed records,
not a variable-sized in-memory list, and only 16 records are cached
for viewing. Additional heap and lwIP socket buffers are not measured.

Header/metadata work has a ten-second total deadline. Body reads use
per-read timeouts (with a longer idle allowance for upload chunks), rather
than a ten-minute deadline across a multi-gigabyte transfer; cancellation is
checked while waiting and between chunks. Existing network response writes
can add socket latency. These are cooperative bounds; physical SD calls
are not preempted. No measured power or stack high-water claim is made.

Host verification:
- 2167 session/framing checks, including all truncated offers, malformed framing,
  token/IP binding, expiry and clock wrap, filename/size limits, bounded JSON
  exhaustion, and 2000 mutated metadata inputs.
- 184 receiver checks compile the actual receiver source against fake network
  and HAL storage. They cover successful exact saves, wrong tokens before any
  write, replay, busy sessions, conflicts, sender cancellation, expired offers,
  partial writes, close/rename failure, bad digests, disconnect and Back/Home.
- The crypto substitute is explicitly a digest sentinel; these host tests do
  not validate wolfSSL SHA-256, TLS, real networking, multicast or physical SD.
  Production code uses the already installed wolfSSL SHA-256 API.
- Sender syntax checked; live sender run remains a human device step.
- No sanitizer pass is claimed.

Run host suites with tests\localsend\run-host-tests.ps1. Its ArduinoJson
parameter can point to another existing checkout; no dependencies are installed.
The test stubs are confined to tests/localsend/stubs and never enter firmware.

## Streaming sender interoperability correction

The first phone-app test returned HTTP 400. Source inspection found a concrete
compatibility gap: the official Rust sender wraps file streams without a known
HTTP body length, while the initial receiver rejected chunked transfer encoding.
This is a likely explanation, pending the corrected device run. Source reviewed:
https://github.com/localsend/localsend/blob/92e133aa43869e94d394fd04fa70c50a0f796a64/packages/core/src/http/client/mod.rs
(upload_body), and packages/core/src/http/client/v2.rs (upload).

The receiver now decodes upload chunks with its existing 1024-byte buffer.
It verifies the decoded size against the accepted offer and consumes the zero
chunk and bounded trailers before publishing. Chunk-size lines are capped at
128 bytes, chunk count at 65536 including the terminal chunk, and trailers at
seven nonempty lines plus a blank terminator within 1024 bytes. Large declared
chunks are streamed; no chunk-sized allocation is made. The offer's size limit,
authorization-before-write, digest checks, cancellation and deadlines remain.
Empty cancel requests may omit Content-Length, matching the official sender.

New tests cover legal extensions/trailers, split ZIP magic, an oversized decoded
body, early/missing terminators, bad chunk syntax, truncated bodies, wrong tokens,
duplicate transfer encodings and conflicting Content-Length. Error responses now
include a message; LSEND logs identify framing, prepare and upload failures
without printing raw headers, query tokens or offer bodies. No device success
is claimed for this correction until the human rebuilds and retries.

## Per-transfer acceptance

Each valid, non-conflicting offer now opens the shared OptionPopup on the File
Transfer screen. It displays the EPUB filename, rounded KiB size and sender IP.
The sender IP identifies the connection; it is not an authenticated identity.
Decline is selected initially. Accept returns session credentials and permits
that one upload. Decline, Back and tapping outside the popup reject the offer;
Home exits File Transfer and closes its connections. The initial Confirm hold
and its release are ignored for button selection; independent touch remains
available. No new activity, worker or framebuffer is allocated.

The pending prepare connection is retained for at most 60 seconds. No file data
is written before acceptance. Timeout, Wi-Fi loss or sender disconnect rejects
and removes the prompt on the next UI loop. The already-read metadata buffer is
released before waiting. One existing NetworkClient, session state and popup
state remain in the File Transfer activity. Normal loop delay applies while the
popup waits. Other request handling pauses during the prompt; cancelling in the
sender closes that pending connection. A new request is required after rejection.

Shared UI theme metrics and touch routing own popup geometry. Device checks:
- Accept an EPUB, open it, then verify duplicate protection still rejects it.
- Decline and press Back: neither creates a book or staging file; File Transfer
  stays open. Retry with another offer and accept it.
- Cancel on the sender while the prompt is visible; it should disappear.
- Leave the prompt untouched for 60 seconds; the sender should get rejection.
- Hold Confirm before an offer arrives: releasing it must not select an option.
- Test touch and remapped buttons in portrait, inverted, landscape CW/CCW.
- Home while waiting must exit the network flow without retaining the request.

Host checks pass: 2167 session/framing checks and 184 receiver checks. New cases
exercise deferred replies, no writes while waiting, explicit acceptance/decline,
timeout, disconnect, late acceptance and stop cleanup. Host tests do not render
or exercise physical buttons/touch. Translation generation passed with English
fallback labels. Firmware compilation and new device confirmation remain pending.

## Device evidence and measured prior build size

The user confirmed real LocalSend app transfer after the chunked correction.
The serial log records a 791347-byte EPUB saved with HTTP 200 and chunked=1.
The user confirmed HTTP 403 on sending the same filename was expected duplicate
protection. During the longer run, internal free memory recovered from 91864 to
94240 bytes before exit; PSRAM stayed at 8278804 bytes. Exiting File Transfer
performed the existing software restart. These observations do not establish
complete in-boot heap cleanup or long-duration leak freedom.

Before adding this acceptance UI, the locally built firmware.bin measured
5730512 bytes against the supplied baseline's 5658784 bytes: +71728 bytes,
70.05 KiB. This is the difference between those two binaries, not a controlled
same-settings feature-on/off comparison. It contains no native ELF loader.
The application slot remains 6553600 bytes. The new acceptance code has not
been built as firmware by the agent, so its updated size is not yet measured.
