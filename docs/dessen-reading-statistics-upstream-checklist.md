# Dessen Reading Statistics: firmware and SDK upstream checklist

> Status: **working Razor-Crest integration**, verified with host-side tests; actual upstream CrossPoint compatibility is **not yet established**.
> Baseline integration snapshot: `thinkforge/dessen-runtime`, Rust app **0.5.7** (November 2026); later uncommitted Server Sync work is tracked separately below (app manifest **0.6.9**, November 23, 2026).
> Purpose: retain the concrete firmware/SDK requirements needed to run the Rust Reading Statistics app on a future upstream CrossPoint build, rather than assuming that upstream Dessen support alone provides all of them.

## Trial stage: standalone pairing and Overview fetch (0.6.8)

**Scope:** Before adding any Dessen-owned `sleep.enter` subscription, first verify that X3 can pair to Techy-Notes, perform an authenticated server Overview GET, and return to the same Dessen page after each silent reboot. Picco follows the identical two-stage reboot lifecycle. **No firmware was compiled and no hardware test has yet passed.**

**Standalone payload:** `experiments/reading-statistics-dessen-rust/{app.bin,manifest.json,device.json}`. Pairing uses the app's own `device.json` (auth/code, auth/token, catalog); the legacy `reading-statistics/device.json` and legacy plugin installation are **not required**. The firmware catalog's explicitly targeted pairing entry bypasses normal `app.bin`-first selection; regular plugin launches continue to prefer Dessen.

**Lifecycle:** Rust syscall 108 requests pairing or refresh. Firmware releases the 80-KiB guest and e-ink snapshot, stores only an allowlisted app path plus one-shot destination in RTC, and silently reboots **before** initializing Wi-Fi. Fresh-heap firmware pairs using Dessen's descriptor or downloads an authenticated Overview to `<dessen-app-dir>/server-overview.json` via atomic temporary-file promotion. After the operation, firmware silently reboots back to the same Dessen app, which consumes the saved one-byte page marker and cached server totals without initializing Wi-Fi. Cancellation and failed downloads still return to the app with local/previous snapshot data. All devices use the same mode; firmware declarative-SD-plugin primitives remain available.

**Hardware test plan:** (1) Flash updated firmware using user builder; (2) install **0.6.8** app.bin, manifest.json, and device.json (all three) on SD; (3) without relying on the original Reading Statistics plugin, start Dessen → Server Sync → Pair Reader; (4) confirm first silent reboot, QR/code approval and persistent token; (5) exit pairing, confirm server Overview downloaded and second silent reboot returns to Server Sync; (6) open Overall, verify SERVER totals and touch/widget refresh; (7) cancel/retry without boot loops; (8) check serial free/max heap, TLS and stack after each phase, on X3 first, then Picco. **Sleep synchronization deliberately deferred.**

## X3 serial-log (5): pairing exit before native registration

**Root cause and correction (November 23, 2026):** The X3 Dessen application actually runs as `/.crosspoint/plugins/reading-stats-dessen/app.bin`, but initial firmware handoff, RTC reboot allowlist, and pairing catalog were hard-coded to `reading-statistics-dessen-rust`. Pressing Open therefore triggered `leaveApp()` without any pairing request, without loading `device.json`, and without contacting Techy-Notes. The log showed no native handoff and no HTTP attempt. X3 concurrently logged preexisting 32-KiB ZIP inflate and 8-KiB BW snapshot OOM errors; these are independent rendering-memory problems.

**Fix:** Accept both exact first-party identities `reading-stats-dessen` (actual device installation) and `reading-statistics-dessen-rust` under the supported SD plugin roots; preserve the actual app path through both silent reboots; dynamically pass its directory name as the forced firmware catalog pairing target; trust either first-party Dessen app's own `device.json` for the reserved token only when its `app.bin` exists. Print diagnostic pairing acceptance/rejection and ensure token rotation invalidates both Dessen cache identities. Keep the standalone app completely independent of the legacy `reading-statistics` plugin. All host/source tests pass; **hardware pairing not yet verified**.

**Packaging requirement:** Copy all three files (`app.bin`, `manifest.json`, `device.json`) into the **same actual on-device application directory**. For the tested X3, this is `/.crosspoint/plugins/reading-stats-dessen/`, not the alternative directory. The staged package is `experiments/reading-statistics-dessen-rust/build/reading-stats-dessen/`. An SD card not mounted during a future packaging step cannot be described as updated.

## X3 progress sync stack overflow and Overall sync confirmation

**crash_report.txt (November 23):** FreeRTOS reports `ActivityManager` stack overflow after `[PHTP] TLS cache reclaim`; the render task is named `ActivityManagerRender` (truncated in FreeRTOS diagnostics). Earlier X3 logs reported render-task high water of only 592 and 80 free bytes. This is a render-task stack issue, not a verified TLS/authentication fault. Mitigation: **X3-only** `src/activities/ActivityManager.cpp` render task stack 8 KiB → **10 KiB**, leaving the Dessen 80-KiB guest reservation unchanged and preserving Picco/other device stack configuration. Hardware verification REQUIRED: ordinary KOReader progress push/pull, extension event sync, and Dessen launch contiguous allocation; if insufficient, reduce nested font/layout stack usage before increasing further. No firmware built by assistant.

**UI requirement (0.6.9):** Touching the paired Overall This Month card **only opens a confirmation**; cancel/back must not call network; confirm paints `Syncing...` instead of monthly hours before requesting the firmware-managed native handoff. The reboot helper preserves the Syncing frame for the first refresh reboot. Rust host tests verify confirm/cancel touch and physical actions and render-before-handoff; hardware validation pending. The last source path fix also accepts the original X3 `reading-stats-dessen` app directory and requires bundled `device.json`.

## X3 pairing completion and Overall totals (serial-log 6)

**Observed:** X3 accepted the Dessen pairing handoff and opened the bundled authentication UI. After authorization it entered the normal PluginCatalog browsing state instead of finishing. The parent Dessen callback (which downloads `/api/device/overview` to the app's `server-overview.json`) was never triggered. A later manual launch saw the one-byte `resume-page` left at 6 and reopened Server Sync rather than Overall; the missing server snapshot caused Overall to show local hours.

**Fix:** `PluginCatalogActivity::pollAuth` now calls `finish()` directly after successfully saving the device-code token **only for the Dessen forced pairing mode**. Existing SD PluginCatalog remains unchanged. Forced pairing Back and Home both pop to the suspended parent, which attempts the server Overview GET and always silently reboots back to Dessen. Network/cache failures log distinct reasons without exposing tokens. A normal app launch clears any stale `resume-page` marker; the firmware passes `resumeAfterNetwork=true` only for a one-shot RTC Dessen return. Fresh pairing and explicit Server Sync refresh store page **0 (Overall)**, not page 6. Rust 0.6.10 and the updated firmware are required together; test refresh/pairing on X3 hardware before declaring completion. No firmware build performed.

## Standalone Dessen reset-pairing action (0.6.11)

A Dessen app must be able to unpair from the device itself, including X3
(which cannot normally expose its SD card over USB). **Do not relax**
`ProtectedPaths.h`, the WebUI file-manager delete restriction, or WebDAV.

**App:** Native Server Sync displays **Disconnect reader** (touch) or
**Previous: Disconnect reader** (physical X3). It asks for confirmation.
Cancel/Back makes no network call and does not modify storage. Confirm calls
existing Dessen syscall 108 `native_stats(2)` and writes the one-shot Overall
resume marker. Local reading data remains intact.

**SDK/firmware:** Native syscall action 2 maps to the restricted
`NativeStatsUnpair` host launch. First-party Dessen path and bundled
descriptor validation run before VM unload. RTC silent target 7 boots into
native operation 3; firmware removes the reserved credential through
`pluginhttp::removeTokenFile` and thereby invalidates Overview caches in
all supported plugin roots (including if token was already absent). No
guest file-manager permission is expanded. A subsequent silent reboot
returns to Overall in unpaired mode.

**Verification:** Dessen host and size-optimized Rust app fixtures plus
dedicated X3 keys/Picco landscape/portrait confirm/cancel tests; firmware
static source contracts and clang-format; hardware unpair + re-pair still
required. No firmware compilation by assistant.

## Source of truth and commits

| Layer | Current local commit | Relevant source |
| --- | --- | --- |
| Razor-Crest firmware integration | `dd99ea59` | Root firmware worktree; Dessen host, plugin launcher, native-services adapter, font wiring, reader/library integration |
| Rust Reading Statistics app | `1675d633` | `experiments/reading-statistics-dessen-rust/`; package version 0.5.7 |
| Earlier C proof of concept (reference only) | `6b89a8ac` | `experiments/reading-statistics-dessen/`; **not** the Rust app's runtime dependency |
| Dessen engine / ABI | `2c8d944` | `dessen/` gitlink in firmware |
| FreeInk SDK extensions | `b81de94` | `freeink-sdk/` gitlink in firmware |

**Important:** Dessen and FreeInk SDK modifications are **required parts of this working integration**. The parent commit pins both local submodule commits. The commits are local until pushed to suitable remotes; their hashes alone do not make them fetchable by another checkout or CI. Ensure commits are published/reachable *before* using them on upstream/CI, but do **not** silently move submodule pins to different upstream revisions. Check remotes and licenses before porting. This document is an inventory, not upstream acceptance.

## Active change log and evidence (keep updated for every firmware change)

This ledger records **why** a change was made, **where** it lives, what was
actually verified, and what still needs on-device validation. A change being
tracked here does **not** mean upstream has adopted it. Preserve this ledger
when creating an upstream PR or comparing a new CrossPoint firmware version.

| ID | Layer / change | Reason and specific failure | Evidence so far | Status / upstream requirement |
| --- | --- | --- | --- | --- |
| M01 | **Rust app**, `experiments/reading-statistics-dessen-rust/Makefile`: Dessen guest memory increased **64 KiB → 96 KiB** using `-Wl,--defsym,APP_MEM=98304` | Server Sync code and temporary buffers use the same Dessen guest allocation as the dashboard's code/data/stack. The linker script's default is 64 KiB; its guest stack descends from the end of that block. | **November 22, 2026 controlled comparison**: the *same compiled Rust static library* linked at 64 KiB built but **failed** `test_sync.py` (no credential persisted); **72 KiB and 80 KiB passed both `test_sync.py` and `test_fixture.py`**. The normal 96 KiB build also passed both. | **Historical pairing-only version; superseded by M02 (80 KiB).** Do not describe it as a required firmware heap increase. Measure real guest stack high-water and memory on X3/Picco before reducing, and rerun both suites for every change. Dessen's existing 4 MiB app memory cap admits 96 KiB. |
| M02 | **Rust app and X3 memory**, `experiments/reading-statistics-dessen-rust/Makefile`: **96 KiB to 80 KiB** app allocation | X3 `serial-log` repeated `OOM: 98304 bytes of app memory` with ~86 KiB maximum contiguous allocation. | **November 23, 2026** current 0.6.4 Rust library: 72 KiB compiled but **corrupted dashboard text** in fixtures; 80 KiB passed both dashboard and pairing suites, including Web PIN and remote Overview. This is not a guest stack high-water proof or hardware validation. | **Keep 80 KiB for X3 launch testing; monitor memory and repeated TLS on X3/Picco.** Do not call this a firmware global heap increase. Related firmware reclaim N07 is mandatory for X3 networking. |
| N01 | **Firmware**, `src/activities/plugins/CrosspointServices.cpp`: guard Dessen's `Op::WifiStart` against an already-configured/connecting ESP32 station | On Picco, generating a pairing code logged `wifi:sta is connecting, cannot set config` and `ESP_ERR_WIFI_STATE (0x3006)` at `STA.cpp:417`. A duplicate `WiFi.begin()` rewrites station configuration during an ongoing association. | ESP32 Arduino STA source confirms `WiFi.begin()` calls `esp_wifi_set_config()` and can fail while connecting. The initial N01-only Picco test still timed out at 20 seconds, so N02 refined the state handling. The combined N01/N02 implementation subsequently connected and reached HTTPS 200 on Picco. | **Superseded/refined by N02; further hardware testing required.** Reuse an existing join attempt, preserve the **20 s** timeout, and verify cold-start Wi-Fi, already-connected Wi-Fi, joining Wi-Fi, failure/retry and configured-but-disconnected cases. CrossPoint port must preserve equivalent behavior, not necessarily this exact code. |
| N02 | **Firmware**, `src/activities/plugins/CrosspointServices.cpp`: distinguish configured-but-idle station from active Wi-Fi join and log Dessen Wi-Fi/HTTP outcomes without sensitive data | Picco `serial-log (2).txt` after **three Generate Code attempts** (November 22, 2026) contained only refresh/memory lines: no pairing code, no Wi-Fi or HTTP outcome. The previous N01 guard assumed a configured `WL_DISCONNECTED` station was already connecting and could wait 20 seconds without actually initiating a join. | Code now asks `esp_wifi_connect()` to join using existing station config; `ESP_ERR_WIFI_STATE` means another connection is already underway. `WL_IDLE_STATUS` waits for IP; unconfigured stations use saved credentials. Logs Wi-Fi state and join/timeout, and HTTP method/status/size or transport/heap errors. No URL, payload, SSID, pairing secret or token is logged. **Picco hardware evidence (November 23): Wi-Fi connected in 1,654 ms and /api/device/code returned HTTP 200, 206 bytes.** Approval checks subsequently failed the TLS heap preflight (N03), not Wi-Fi. | **Required for troubleshooting / upstream behavior parity:** verify three device-code requests, already-connected, disconnected-with-config, auto-reconnect, no saved credentials and 20-second timeout on Picco. Confirm live HTTPS response and displayed code; rerun device tests before declaring pairing operational. |
| N03 | **Firmware**, `src/activities/plugins/CrosspointServices.cpp`: Dessen HTTP preflight uses the firmware's PSRAM-aware TLS block threshold | Picco generated a code with HTTP 200, then refused subsequent `/api/device/token` requests because the largest internal allocation was **31,732 B**, 1,036 B below Dessen's 32 KiB check, despite **~4.75 MiB PSRAM** free. | Uses `HalMemory::getPsramHeap().totalBytes` to choose `MIN_TLS_MAX_ALLOC_PSRAM=20,000` on PSRAM boards, otherwise the existing 32 KiB floor; still enforces the 48 KiB free-internal-heap floor. Mirrors `CatalogActivity.cpp` safeguards rather than disabling TLS checks. **Hardware retest pending.** | **Required for server pairing on Picco and other PSRAM devices.** Confirm repeated HTTPS requests, memory low-water, failed-handshake behavior, and no new OOM/reset. |
| N04 | **Firmware**, `CrosspointServices.{h,cpp}` and `DessenAppActivity.cpp`: Wi-Fi picker fallback/resume | User shouldn't have to connect through firmware UI before launching Reading Statistics. Previous Dessen Wi-Fi start could timeout after 20 s and never show available networks. | New `Op::WifiSelecting` parks the VM, launches existing `WifiSelectionActivity` after no credentials/join failure/20 s timeout, and reports success/cancellation to the original pending Wi-Fi syscall without restarting the app. **Not yet hardware-tested.** | **Required for self-contained app networking.** Verify Picco touch and X3 buttons, saved-network failure, multiple SSIDs, cancellation, return-to-app, then code generation; check optional auto-restart behavior of native Wi-Fi UI. |
| N05 | **Firmware + Dessen SDK**, `CrosspointServices::qrBitmap`, `dessen/abi/freeink.api` syscall **106**, `DessenServices.h` and generated `sdk/dessen.*`/`dessen_dispatch.inc`, Rust `qr.rs` | Version 0.6.2 only showed the PIN and link text, not the user-requested scannable code. Implementing QR Reed-Solomon inside RV32 guest caused the desktop pairing test to stall; native generation avoids the emulator cost. | Firmware's existing MIT QRCode dependency encodes a bounded 4-L matrix as row-padded 1bpp `{u16 width, u16 height, bytes}`; Rust renders it with the existing bitmap syscall. `make test` for Rust and Dessen host/ABI passes. **Picco's initial pairing QR scanned and paired successfully on 0.6.3; new web-login QR and X3 remain hardware-pending.** | **New ABI capability for this UI.** Preserve syscall number and response format when upstreaming or replace with compatible guest-side encoder; test 29/33-module scan, QR quiet zone, portal prefill and no secret exposure. |
| N06 | **Firmware + Dessen host**, `DessenAppActivity.{h,cpp}`, `dessen/freeink/DessenFreeInk.{h,cpp}`, `sdk/dessen_abi.h` (`DSN_EV_TIMER=11`), Rust `readstats.rs`/`sync.rs` | Server accepted the PIN, but the user needed to press Check Pairing repeatedly; later attempts were blocked by N03's TLS guard. | A synthetic non-touch five-second WakeTick is delivered only to the Reading Statistics app while the VM is waiting. Rust polls only in its Code phase, stops on pairing/expiry, and leaves polling when navigating away. Background checks update e-ink only on visible state changes, not every five seconds. Added timer-key desktop fixture and FreeInk host test; full Dessen and Rust suites pass. **Device hardware pending.** | **Required for automatic approval checks.** Verify no taps/side keys are fabricated, 10-minute expiry, Back cancellation, repeated checks and no excessive e-paper refreshes. |
| N07 | **Firmware**, `DessenAppActivity.cpp`, `CrosspointServices.h`: free reusable glyph caches before X3 guest allocation; discard saved e-ink frame before network work | X3 maximum alloc ~86 KiB and free internal heap ~111 KiB could not hold a 96 KiB guest. Even an 80 KiB guest leaves too little contiguous heap for Wi-Fi/TLS if a duplicate saved framebuffer is retained. | Added conditional `FontCacheManager::releaseSdFontCaches()` before app load on non-PSRAM boards, and a `needsNetworkHeap()` gate to release `renderer.storeBwBuffer()` snapshot just before Wi-Fi/HTTPS (not while opening keyboard). Logs free/max blocks. **Not hardware-tested.** | **Required for X3.** Verify no overlay/frame corruption, app launch, TLS 48 KiB free-heap and 32 KiB block thresholds, repeated QR/login/Overview requests, and return-to-app. Never weaken TLS preflight to compensate. |
| N08 | **Firmware**, `CrosspointServices.cpp`: offline HTTP fail-fast guard | Automatic server Overview checks must never open the network picker or block an offline statistics view indefinitely. Only explicit pairing or Web PIN requests may invoke the picker. | Ordinary disconnected HTTP returns `DSN_ERR_UNAVAILABLE` immediately. **N10 supersedes the Overview-specific case** with a bounded, quiet 10-second saved-network attempt (no picker); Rust `ServerOverall` keeps local metrics when unavailable. **Hardware pending.** | **Required for offline UX** on both X3 and Picco. Verify bounded offline Overview and manual Web PIN opens Wi-Fi picker. |
| S02 | **Techy-Notes Sync Server**, `src/routes/device-pairing.ts` and `web.ts`: fresh single-use web PIN and read-only account-wide overview | After pairing the reader had no way to open private mobile statistics; Overall only displayed a local preview. Existing `/api/device/catalog` already issued reader login PINs but Dessen lacked the action. Browser-only `/api/device/dashboard` cannot safely accept upload tokens. | Added `POST /api/device/login-code` (fresh code invalidates earlier) and `GET /api/device/overview` (strict scoped reader auth, account-wide native/session totals without double counting). `/signin?code=...` prefills the PIN but **never automatically signs in**. Server TypeScript build and 14 focused tests passed; deployment not performed. | **Server companion dependency, not firmware source.** Non-destructively staged tested source to `X:\\techy-notes_sync-server-code` (hash-verified, no build/deploy). Deploy and validate endpoints before hardware test; check revocation, account isolation, timezone, empty/guest account, URL safety. |
| A14 | **Rust app**, `server.rs`, `sync.rs`, `qr.rs`, `preview.rs`, `readstats.rs`: server-backed Overall and portal login code with enlarged QR | Picco pairing succeeded but no QR/PIN after pairing, and Overall remained a local preview. | Version 0.6.4: token stays in SD root, authenticated Web PIN and compact scoped Overview requests; larger QR with prefilled `/signin?code=...` portal link; automatic refresh once per app entry and manual tap on month heading; local fallback while offline. Rust pairing/dashboard fixtures passed at **80 KiB**. | **Requires deployed S02 server endpoints** and firmware N07/N08 on X3; Picco/X3 visual QR, HTTPS, touch/button, offline and refresh tests pending. |
| N09 | **Firmware**, `CrosspointServices::qrBitmap`: heap-allocate bounded native QR scratch | X3 serial log reported only **420 bytes remaining** in the UI render task stack. The earlier native QR encoder kept a 512-byte scratch array on that task stack, risking overflow during code generation. | Replaced the fixed render-stack array with `makeUniqueNoThrow<uint8_t[]>(qrcode_getBufferSize(4))`, bounded to 512 bytes, and retain graceful allocation failure; `CrosspointServices.cpp` formatting and source tests reviewed. **Hardware measurement pending.** | **Required for X3/Picco stack safety.** Verify render-task stack high-water during both pairing and Web PIN QR display and handle allocation failure gracefully. |
| N10 | **Firmware**, `CrosspointServices.{h,cpp}`: quiet saved-network join for the paired Overall GET | Picco 0.6.4 displayed **LOCAL FALLBACK** even though Web PIN generation succeeded over HTTPS (POST 200). `ServerOverall` deliberately skips the interactive `dsn_wifi_connect`, and the previous `Op::Net` rejected disconnected Wi-Fi instantly; server logs did not show a device `GET /api/device/overview`. | For precisely `GET https://sync.techy-notes.com/api/device/overview`, Wi-Fi can join a stored SSID without opening the native picker. `NetConnecting` checks connectivity asynchronously with a **10-second bound**, then resumes the original HTTP GET; missing credentials/failed join returns offline fallback, never retries indefinitely. No SSID, credentials or token logging. Production anonymous endpoint probes returned 401 (routes exist), but authenticated device hardware flow needs verification. | **Required for server-backed Overall on cold start.** Verify quiet Wi-Fi start, network timeout, no picker, TLS 200 on Picco/X3, and LOCAL FALLBACK while offline. |
| N11 | **Firmware**, `DessenAppActivity.{h,cpp}`: suppress redundant e-paper restoration after synthetic polling tick | Picco log showed e-paper refreshes approximately every five seconds with no corresponding network request or visible state change, while the Dessen timer wakes the guest every five seconds. `DessenAppActivity::render` previously restored and displayed the saved frame after every no-draw timer wake. | Track synthetic WakeTick as a no-draw event; do not run `restoreBwBuffer/displayBuffer` for that timer frame if the app did not request a refresh. Overlay restoration for real activity invalidations remains unchanged. Rust still refreshes for pairing success, errors, or expiry. **Device testing pending.** | **Required to eliminate periodic unnecessary e-ink flashes.** Verify quiet five-second polling, overlays (frontlight), no lost frame, Picco/X3. |
| N12 | **Rust app**, `readstats.rs`: retry server Overall on page return | Initial remote fetch can fail offline before a paired reader requests a Web PIN; the Web PIN then enables Wi-Fi, but the existing `server.attempted=true` prevented refresh when user returned to Overall. | When entering Overall from another page while paired, reset `attempted` once so the account-wide totals are fetched again; tapped heading remains manual refresh. **Rust dashboard and pairing fixture suites pass, including an explicit Server Sync return/retry assertion.** | **App-only change;** verify page return, failed network fallback, reconnect without busy-loop or unnecessary picker. |
| N13 | **Firmware**, `CrosspointServices.cpp`: refuse unsafe cold Wi-Fi start on non-PSRAM boards | X3 `serial-log (3).txt` shows **80-KiB guest loads** but leaves ~29 KiB free with only ~16 KiB largest block; CPRO EPUB extraction then brings free heap under 18 KiB, Wi-Fi initialization fails repeatedly with `ESP_ERR_NO_MEM`, and ActivityManager crashes with **stack protection fault**. | Before native Wi-Fi/picker initialization on non-PSRAM boards, require at least 80,000 bytes free internal heap and `MIN_TLS_MAX_ALLOC` contiguous block, log actual free/max and return `DSN_ERR_UNAVAILABLE`; preconnected requests still obey the existing TLS guard. No changes to PSRAM Picco. **Safety mitigation only, not X3 network support.** | **Required before further X3 hardware tests**: avoid Wi-Fi task creation/OOM/stack panic. X3 offline statistics and renderer still need more headroom for ZIP decompression and BW frame buffers. To support server sync on X3, develop memory architecture that can coexist with Wi-Fi/TLS rather than bypassing safeguards. |
| A17 | **Rust app**, `preview.rs`/`readstats.rs`: compact refresh icon in paired Overall header | Picco clipped `THIS MONTH - SERVER (TAP TO REFRESH)` to `(TAP TO R...` and user requested a sync icon. | Replaced instruction text with `THIS MONTH - SERVER`, drew 12 short lines forming e-ink sync arrows, and moved tap detection to the icon's 38×34-pixel hitbox; portrait server overview fixture checks no clipped instruction. No firmware ABI changes. | **App-only UI**: verify icon is visible, tappable and does not overlap month metrics on Picco; no impact on X3 physical-key UI. |
| A18 | **Rust app**, `server.rs` and `readstats.rs`: bounded quiet Overview retries | Picco required multiple manual refresh taps before `LOCAL FALLBACK` transitioned to server totals when Wi-Fi started slowly. | Paired Overall makes up to three attempts (initial + two five-second timer retries), stops on success, and redraws only when data becomes available. Returning from Server Sync or tapping sync icon resets the attempt budget. Uses existing firmware timer; no new ABI or firmware changes. **Device verification pending.** | **App-only reliability change**; confirm cold-start connection settles without extra taps and avoid repetitive requests when offline. |
| N14 | **Firmware + Dessen ABI**, `dessen/abi/freeink.api` **107/108**, generated SDK/dispatcher, `CrosspointServices::nativeStatsMode`, `DessenAppActivity` | An 80-KiB RV32 Dessen guest left only ~30 KiB internal RAM on X3; Wi-Fi/task creation, EPUB inflate and BW buffers failed while the VM stayed resident. Existing native SD Reading Statistics pairing works on X3. | `native_stats_mode()` returns 1 only on non-PSRAM firmware; `native_stats(0|1)` requests native pairing/manual cache refresh. `DessenAppActivity` validates the first-party app path, calls `unloadGuestForNativeStats` (services, guest and saved BW snapshot freed), opens existing `PluginCatalogActivity` for `reading-statistics` or runs one native server GET, then stops Wi-Fi and reloads guest from SD. Rust saves/restores page via a tiny file under its actual app directory. Desktop host native handoff fixture PASSES; **real X3 memory/stack and ActivityManager lifecycle still pending**. | **Critical X3 memory architecture:** do not start Wi-Fi/TLS while Dessen VM is loaded. Preserve ABI syscall IDs and anti-cross-app path guard; check free/max block before and after guest unload/reload, reset on cancellation, and behavior if standard SD plugin is missing. No firmware compilation performed. |
| N15 | **Firmware plugin events + unified SD plugin**, `PluginEvents.cpp` and standard plugin `device.json` version **0.1.13** | Need account-wide server stats on X3 without waking an 80-KiB Dessen VM and without depending on KOReader sync settings. `sleep.enter` is already emitted and drained after sleep-screen render; it reuses bounded saved-network join with KOReader sync. | Unified plugin subscribes to `sleep.enter` with authenticated GET to `/api/device/overview`; the standard plugin event drain streams response atomically into `server-overview.json` under the installed plugin folder (supports all 3 plugin roots). Relative destination names are restricted to safe basenames without path traversal. Unpaired readers do not queue sleep downloads or request a Wi-Fi join. Network/download failures keep the previous snapshot. All 12 standard plugin tests pass; **sleep hardware test pending**. | **Firmware + standard plugin dependency:** must update both; test standalone sleep sync even with KOSync unset, one network join only, interrupted download recovery, low battery/weak Wi-Fi and no sleep display disturbance. |
| N16 | **Firmware**, `PluginHttp.cpp`: invalidate cached account metrics on pairing token change | Re-pairing can link the reader to a different user; a previous user's cached totals must never show as current account data. | For the protected unified token path, native token-save compares previous and next values and clears cached `server-overview.json`, `.part`, `.bak` under all supported plugin roots when changed. Token deletion clears those files too. Rust only displays cached totals while a persistent token exists. | **Private data isolation:** verify cancellation retains existing valid token/cache, new token removes stale cache before reading, and no credentials leak to logs. |
| N17 | **Techy-Notes server**, `GET /api/device/overview` response adds `synced_at` | Need last successful sync time to persist across app restarts and control when refresh icon appears. | Server returns authoritative Unix seconds `synced_at` in authenticated snapshot. Dessen Rust parses it and shows sync-arrow icon after explicit widget touch or when 24h has elapsed. Invalid/missing timestamp counts as not recently synced; no token or private server field is stored in UI text. **Deploy updated server source before hardware test.** | **Server deployment dependency** on top of existing S02 read-only endpoint; verify timestamp, account isolation and cache freshness after sleep. |
| A19 | **Rust app v0.6.7**, `readstats.rs`, `server.rs`, `sync.rs`, `preview.rs` | X3 must use offline cache and native pairing/manual refresh, while Picco retains live QR and HTTPS. Overall touch previously advanced to next page and showed an unwanted LOCAL FALLBACK label. | On X3, native mode reads `server-overview.json` from any installed plugin root, never issues guest HTTPS; unpaired Server Sync launches native pairing and paired Server Sync manual native cache refresh. The whole This Month card refreshes without page navigation, while the sync icon shows only after widget touch or 24h since `synced_at`. No 'LOCAL FALLBACK' label. Both Rust fixtures and desktop host native mode tests pass. | **Requires N14–N17** for X3. Picco live behavior must be regression-tested; X3 native interaction, 80-KiB guest allocator, book/ZIP/BW OOM and sleeping network session need physical validation. |
| B01 | **Local firmware builder**, `C:\\XP-FW\\sync-source.ps1`: seed pinned FreeInk and Dessen gitlinks from their **local source repositories**, rather than running a recursive remote submodule update | Picco build aborted with `fatal: remote error: upload-pack: not our ref 2c8d944...` because the Dessen integration revision (and custom FreeInk revision) is committed locally but unavailable from the remote listed in `.gitmodules`. The builder already had logic to mirror Dessen's tracked edits, but that logic ran after the remote update failure. | **November 22, 2026**: `sync-source.ps1` completed using worktree `thinkforge/dessen-runtime`; build mirror revisions match Dessen `2c8d944` and FreeInk `b81de94`, and both submodule patch hashes match. The firmware Wi-Fi patch's **normalized source lines** also match the mirror (raw hashes differ due to CRLF/LF). **No firmware compilation performed.** | **Builder-only, outside the firmware repo; not an upstream firmware code dependency.** Preserve the builder fix locally, and publish the pinned commits or provide an equivalent local-source sync mechanism before attempting reproducible CI/upstream builds. Never substitute a different SDK revision silently. |
| S01 | **Firmware + Dessen ABI**, baseline Dessen host/SDK/launcher/statistics/font/touch/book-data extensions (IDs F01–F06, A01–A16 below) | The Rust app needs native reading-statistics files, stable fonts/layout, accurate touch/button events, recent-book metadata, book launch and Dessen app loading. | Baseline app previously exercised on X3/Picco; host and Rust tests exist. Version, platform and test caveats detailed below. | **Required capability audit for every firmware integration/upstream sync**. Compare with upstream, port only missing contracts, keep local uncommitted work and submodule pins intact. |

**Memory reservation interpretation:** `dessen/sdk/app.ld` defaults to `APP_MEM=64K`.
The Rust app's current linker flag overrides that to `81920`, writes the requested
size into the Dessen app header, and changes the VM's **guest allocation**.
`DessenAppActivity::loadApp()` allocates that block (typically PSRAM on S3)
and enforces `MAX_APP_MEMORY=4 MiB`; no firmware heap limit was raised.
The app binary alone occupies roughly 52 KiB of text/rodata in the measured
build, and local Rust frames/HTTP buffers must also fit. The 64 KiB pairing
failure is reproducible, but the failure does not by itself prove a precise
stack-overflow threshold or that the earlier 96 KiB was optimal. Instrument guest stack
usage and test with a populated on-device library before declaring a minimum.

**Change-log rule:** Add a row here for **every new firmware/SDK change**
needed to support Reading Statistics (whether in the host, FreeInk, Dessen,
native stats producer, Wi-Fi or plugin launcher). Record the original symptom,
file/function, rationale, measured test result, hardware status, and whether
the change must be ported upstream. Keep app-only changes separate from firmware
changes, and never mark an untested behavior as hardware-verified.

## 1. Mandatory foundations

| ID | Required capability | Implementation / source | What breaks if missing |
| --- | --- | --- | --- |
| F01 | Firmware enables and runs Dessen RV32IM apps from SD | `platformio.ini` (`[dessen_apps]`, `CROSSPOINT_DESSEN_APPS`), `.gitmodules`, `src/activities/plugins/DessenAppActivity.{h,cpp}`, `dessen/` | `app.bin` cannot be loaded or executed |
| F02 | A way to discover/launch an installed `app.bin` | `src/activities/plugins/PluginCatalogActivity.{h,cpp}` and its plugin scanning / launch branch; `src/activities/ActivityManager.cpp`, `src/activities/settings/SettingsActivity.cpp` for other entry points | App exists on SD but is inaccessible from firmware UI |
| F03 | Dessen syscall adapter and file reads of native stats | `src/activities/plugins/CrosspointServices.{h,cpp}`; `dessen/sdk/dessen.rs`, `dessen/sdk/dessen_sysnums.h`, `dessen/freeink/DessenFreeInk.cpp` | App cannot read its native stats records, book metadata or clock |
| F04 | Native Reading Statistics producer and on-SD record formats | `src/util/ReadingStatsStore.*`, `src/activities/reader/ReaderActivity.cpp` (record/opened book), `src/activities/reader/EpubReaderActivity.cpp`, and existing supporting stats code | Dashboard appears empty despite Dessen being installed |
| F05 | Native global, book and time-bucket files remain compatible | Rust parser in `experiments/reading-statistics-dessen-rust/readstats.rs`, `pages.rs`, `calendar.rs`; native writer in `src/util/ReadingStatsStore.*` | Wrong totals, zero days, missing sessions/books, checksum failures |
| F06 | Local calendar/time, battery, safe drawing and input | `CrosspointServices::clock`, `CrosspointServices::battery`, `DessenAppActivity`, FreeInk UI host/syscalls | Wrong daily grouping, missing indicators, unusable input |

**Build flag warning:** the checked-in `[dessen_apps]` block sets `-DCROSSPOINT_DESSEN_APPS=0` by default. The *actual firmware build* must override it with `-DCROSSPOINT_DESSEN_APPS=1` and correctly resolve the `Dessen=symlink://dessen` library. Confirm the target build configuration; do not infer enablement merely from a present submodule.

**Stats data contract (current Rust reader):**

- `/.crosspoint/reading-stats/global_v2.bin` and backup
- `/.crosspoint/reading-stats/books_v1.bin`
- `/.crosspoint/reading-stats/time_global_v1.bin`
- Per-book `book_<hash>_v2.bin` (and backup) and `time_<hash>_v1.bin`
- Binary magic/version and FNV-1a checksum checks are in `readstats.rs`; reading uses the Dessen FS read/read-at/list syscalls. These files must be **generated** by the native collector. Import/sync is not implemented in the Rust app.

Do not mistake a read-only dashboard for a replacement of the native statistics collection code. Before porting upstream, compare the *current* upstream collector and formats; a future native stats API could replace file-format coupling but does not exist in this snapshot.

## 2. Concrete API enhancements used by the Rust dashboard

| ID | Requirement / present behavior | Files to inspect and port | Degradation if omitted |
| --- | --- | --- | --- |
| A01 | Stable, explicit Noto Sans 12/14/18 slots (4/5/6) and correct text measure/line height | `freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIGfxRenderer.h`; `dessen/sdk/dessen_abi.h`, `dessen/sdk/dessen.rs`, `dessen/freeink/DessenFreeInk.cpp`; binding in `src/activities/plugins/DessenAppActivity.cpp` | Text/hero cards truncate or wrap differently, incorrect responsive spacing |
| A02 | Font fallback on `OMIT_FONTS` targets | `DessenAppActivity.cpp` (`NOTOSANS_14_FONT_ID` fallback); FreeInk font slot mapping | Missing font on constrained boards |
| A03 | Recent shelf: paged library rows with path, title, author, progress placeholder, optional **fifth formatted series/index** | `CrosspointServices::recentShelf` / `writeShelfRow`; `lib/LibraryIndex/LibraryIndexFile.{h,cpp}` (identity matching, index metadata / series); `dessen/sdk/dessen.rs` `dsn_recent_shelf` | Continue Reading loses series, titles/author or recently-opened book data |
| A04 | Per-book progress from firmware | `CrosspointServices::bookProgress`, `dsn_book_progress`; firmware's `loadBookProgress` | No meaningful unfinished-book filter or percentage |
| A05 | Book cover data in supported bitmap format, bounded by app buffer; placeholder where necessary | `CrosspointServices::bookCover`, `dsn_book_cover`, Dessen bitmap syscall, `src/activities/plugins/DessenAppActivity.cpp` | No portrait cover in Current Book/Continue Reading |
| A06 | Touch swipe **start coordinates** in safe-area units, with low 8-bit event type unchanged | `src/activities/plugins/DessenAppActivity.cpp` (`mappedInput.wasSwipe(&x,&y)`), `dessen/freeink/DessenFreeInk.cpp`, `dessen/sdk/dessen_abi.h` | Cannot distinguish swipe inside Continue Reading from page-navigation swipe |
| A07 | Source marker `DSN_EV_SIDE_BUTTON` (bit 8) for physical side page keys, retaining original Prev/Next event types | `DessenAppActivity.cpp` (front Left/Right vs PageBack/PageForward), `dessen/freeink/DessenFreeInk.cpp`, `dessen/sdk/dessen_abi.h` | X3/X4 side and front buttons cannot have separate page/carousel behavior |
| A08 | Device information: `hasTouch` / `hasButtons` and accurate safe-area/screen dimensions | `dsn_device`, `DessenFreeInk.cpp`, `readstats.rs` `Ui` capabilities | X3 four-label footer vs Picco touch UI cannot be selected properly |
| A09 | Open the selected real book from the app | `CrosspointServices`, Dessen `OPEN_BOOK` syscall, `DessenAppActivity::loop` launch dispatch | Confirm/tapping a book cannot resume it |
| A10 | Wi-Fi picker fallback that resumes a parked Dessen network syscall | `CrosspointServices` WifiSelecting state, `DessenAppActivity` nested `WifiSelectionActivity` | Cold-start/offline users cannot connect from inside Reading Statistics |
| A11 | HTTPS request preflight uses board-appropriate contiguous-block reserve | `CrosspointServices`, `HalMemory`, `HttpDownloader::MIN_TLS_MAX_ALLOC_PSRAM` | Repeated pairing checks fail despite a usable PSRAM board |
| A12 | Firmware-native QR bitmap from bounded link data | `dessen/abi/freeink.api` syscall 106; `CrosspointServices::qrBitmap`, Dessen service/dispatcher, Rust `qr.rs` | No scannable prefilled-code URL on the device |
| A13 | Periodic non-input wake event for pairing approval | `DSN_EV_TIMER=11`, Dessen host `WakeTick`, `DessenAppActivity`, Rust `readstats.rs` | No automatic approval checking while app waits for input |
| A14 | Reduced guest memory + headroom recovery for non-PSRAM X3 | App Makefile `APP_MEM=81920`; firmware `DessenAppActivity` font and saved-frame reclaim | X3 fails to open app or loses Wi-Fi/TLS memory after opening |
| A15 | Non-interactive remote Overall fetch and authenticated web PIN | Firmware `CrosspointServices` Wi-Fi state/HTTP, Rust `server.rs`/`sync.rs`/`qr.rs`, server scoped `/api/device/overview` and `/api/device/login-code` | Overall remains local preview, or offline reads trigger network picker; paired reader cannot open web portal |
| A16 | Native QR scratch must not overflow the X3 render task | `CrosspointServices::qrBitmap` heap-backed scratch bounded to QR version 4 requirements | X3 UI render task minimum reported only 420 bytes remaining while old implementation used 512 stack bytes |

**Precise limitations:**

- In A03, the optional series field is **not** required for bare statistics totals, but is required to reproduce the tested dashboard typography/content. When the library index is unavailable or metadata is absent, no series appears.
- A05 **requires PSRAM for current firmware's cover decode/scale path**. `bookCover()` returns `DSN_ERR_UNAVAILABLE` without `BOARD_HAS_PSRAM`; that is an intentional capability limit, not an app parsing failure. The UI should still show a placeholder.
- A06 encodes the swipe's *start*, not its end or full drag path. Synthetic/old events may lack a coordinate (direction-only). This enables simple widget hit-testing without a widget-registration system.
- A07 has a one-book rule: on Overall, front Previous/Next cycles books **only if two or more exist**; with zero/one it switches pages. Side keys **always** switch pages. The Rust UI draws the X3 footer, not the firmware's standard footer.
- A01–A16 describe what the **current tested app** uses; not all are prerequisites for Dessen itself or for a future, different statistics UI.

## 3. Related firmware integration — review rather than automatically upstreaming

Commit `dd99ea59` touches more than the minimal Reading Statistics runtime. These are supporting integration areas; upstream may already have equivalents or may choose different entry points.

| Area | Files / purpose | Porting decision |
| --- | --- | --- |
| Home/launcher | `src/util/HomeApps.*`, `src/activities/ActivityManager.*`, `src/activities/settings/SettingsActivity.*`, `src/CrossPointSettings.*` | Optional for opening Reading Statistics directly from home; not needed if Plugin Hub launches the app |
| Firmware app callbacks/events | `src/util/PluginEvents.*` | Optional for other plugin-to-app workflows; not necessary for viewing statistics |
| Library-index helpers | `lib/LibraryIndex/LibraryIndexFile.*`, `test/library_index_file/LibraryIndexFileTest.cpp` | Needed for the current efficient series lookup; can be replaced by another reliable metadata API |
| App font/renderer integration | `src/SdCardFontSystem.*`, FreeInk font files, `DessenAppActivity.cpp` | Preserve correct font slot wiring; broader SD theme/font changes need separate assessment |
| Network, crypto, keyboard, credentials | `CrosspointServices.*`, `src/network/HttpDownloader.*`, firmware plugin auth/settings; Rust app `sync.rs` | Server Sync pairing now needs the Dessen Wi-Fi and bounded HTTPS syscalls. Preserve the Wi-Fi join-in-progress guard (`CrosspointServices::serviceAsyncWork`, `Op::WifiStart`) to avoid `ESP_ERR_WIFI_STATE` when the station is already associating. Pairing alone does **not** implement session upload or remote totals |
| English text repair | `lib/I18n/translations/english.yaml` | Fixes malformed shared labels; not a strict Rust statistics dependency |
| Miscellaneous changes in integration commit | `platformio.ini`, `src/main.cpp`, `src/activities/reader/*`, `lib/hal/HalHaptics.h` | Review against upstream rather than cherry-picking indiscriminately |

Do **not** assume a future upstream Dessen merge contains our native stats collector, the same series shelf, fixed fonts, or touch/button source extensions. Compare implementations by contract; replay only missing pieces.

## 4. CrossPoint upstream port checklist

- [ ] Obtain a source revision of CrossPoint **with Dessen accepted and buildable**; record its commit and ABI revision. If no upstream Dessen exists, upstreaming the dashboard alone is premature.
- [ ] Inspect upstream feature flags and plugin SD install/launcher behavior; ensure `app.bin` can run, and verify the actual device build enables it.
- [ ] Compare upstream `dessen` and `freeink-sdk` with pinned local commits `2c8d944` and `b81de94` before applying patches. Avoid duplicating already-merged SDK features.
- [ ] Verify a compatible native stats collector actually writes all required records; preserve users' existing `/.crosspoint/reading-stats/` data.
- [ ] Verify stats-file read permissions and bounded `FS_READ`/`FS_READ_AT` calls.
- [ ] Check clock local-day semantics and counts against native on-device stats. Test empty/corrupt/backup data.
- [ ] Test recent shelf ordering, optional series formatting and long UTF-8 strings; confirm progress percentages and library index identity matching.
- [ ] Verify font ID mappings, line-height/measurement, two-line title and series, hero progress clearance on X3 and Picco.
- [ ] Verify firmware-native QR code can be scanned on X3/Picco with a mobile camera and opens the prefilled production pairing portal; check bitmap scaling, quiet zone and screen layout.
- [ ] Verify PSRAM-aware repeated HTTPS code/token requests stay within memory limits and do not reboot; exercise manual network picker, cancel, and app resumption.
- [ ] Verify automatic five-second approval checks, expiry, Back cancellation, and that no simulated touch input affects other Dessen apps.
- [ ] **X3:** verify 80 KiB app loads, heap/max-block before and after stored-frame reclaim, Wi-Fi and repeated TLS, and no render stack overflow.
- [ ] **Picco:** scan larger paired Web PIN QR, confirm browser PIN prefill and explicit submit, then regenerate; old PIN must fail.
- [ ] **Both:** Overall shows server account totals once deployed; disconnect Wi-Fi to verify immediate LOCAL FALLBACK and manually refresh while online.
- [ ] **Techy-Notes:** deploy tested server routes before app regression; verify account isolation, revocation, and guest reader statistics.
- [ ] Verify covers on PSRAM boards and expected placeholders on non-PSRAM boards.
- [ ] Verify touch swipe origin and X3/X4 side-key source flag semantics; check zero/one/multiple-book carousel behavior.
- [ ] Confirm app launches/resumes books, exits, and doesn't break normal page navigation.
- [ ] Run Dessen host/ABI suite and Rust app regression suite; user builds firmware and conducts X3/Picco device tests.
- [ ] Validate server device-code pairing on Picco/X3 (guest and existing account), including a Wi-Fi join already in progress; preserve the firmware's Dessen Wi-Fi connection guard and 20-second timeout.
- [ ] Keep **session upload, import, and remote Overall** explicitly out of scope until implemented and validated. Device-code pairing is implemented in the Rust app, but the current Overall page is a local preview, not all-device totals.
- [ ] Only after upstream parity and hardware tests: plan commits/PRs. Do not publish or push without approval.

## 5. Reproduction / verification

From the firmware worktree:

```sh
git status --short
git submodule status
git log -3 --oneline
git -C dessen log -1 --oneline
git -C freeink-sdk log -1 --oneline
```

Dessen host/ABI test suite, from `dessen/` in WSL:

```sh
make test
```

Rust Reading Statistics build/fixtures, from `experiments/reading-statistics-dessen-rust/` in WSL:

```sh
source ~/.cargo/env
make test
```

Install the resulting **`build/app.bin` plus `manifest.json`** to the device SD path:

```text
/.crosspoint/plugins/reading-statistics-dessen-rust/
```

The current local firmware worktree is `thinkforge/dessen-runtime`; it has the linked SDK/engine commits. Do **not** build or flash firmware as part of maintaining this document. Hardware verification belongs to the user.

## 6. Future additions to track

The initial **read-only account-wide Overall** and **paired-reader Web PIN** are implemented locally as S02/A15, but **not deployed or hardware-verified**. The existing local statistics remain the offline fallback. Future full-device sync work still needs verified session upload, auth/linking, token storage outside removable app folders, write endpoints, book identity, session de-duplication, device attribution, optional offline server caches, cover transport and schema/version negotiation. A working Dessen networking syscall alone does not establish any of those capabilities.
