# Dessen application capabilities and networking

Phase 3 uses one capability and event contract in both firmware worktrees.
Application identity is the actual installed plugin directory, not a manifest
claim or an application-specific firmware allowlist.

## Manifest

```json
{
  "id": "example",
  "version": "1.0.0",
  "permissions": {
    "filesystem": {
      "app_directory": ["read", "write", "delete", "list"],
      "read": ["/Books/**"]
    },
    "library": true,
    "credentials": {
      "shared": ["/.crosspoint/example-session.json"]
    },
    "network": {
      "origins": ["https://example.com", "http://192.168.1.2:8080"],
      "handoff": true
    }
  }
}
```

The installed directory must be a direct child of a supported plugin root.
Installing a plugin authorizes its validated manifest scopes without additional
user confirmation at launch. A networking continuation carries the original
manifest digest; a changed manifest cannot reuse it.

Parsing accepts at most 4096 bytes with nesting limited to six and a capped
12 KiB ArduinoJson allocator. There are at most sixteen filesystem rules and
eight origins. A rule is an exact canonical path or a directory ending in
`/**`; arbitrary glob syntax is not supported. Rule paths are limited to
190 bytes.

Read, write, delete, list and book-key scopes are distinct. An app may not
modify its executable, manifest or device descriptor through these services.
FAT comparisons are case-insensitive; traversal, empty path components,
ambiguous spaces/dots, control characters, invalid FAT characters and short
aliases are rejected before authorization.

An application's own directory is isolated from other applications. Private
credentials can use its authorized files. Shared credentials are exact
root-level JSON-store paths under `/.crosspoint`, including explicitly granted
temporary siblings when needed; ordinary filesystem rules cannot read them.
Firmware Wi-Fi, OPDS, KOReader, LCP, device-secret and runtime stores remain
unavailable even when requested in a manifest.

All application writes stage privately and replace the approved destination
only after a complete write and successful close. Native consumers receive a
generic file-change notification; their own cache invalidation stays outside
the Dessen runtime.

The library capability permits shelf metadata, supported book covers and
progress without granting raw whole-card reads. Opening a book is checked too.

## Transport and ownership

Network permissions match the entire scheme, hostname and port. HTTP and HTTPS
are separate origins; the default ports normalize to 80 and 443. Credentials,
wildcard hosts, fragments and malformed authorities are rejected in URLs.
Restricted requests never follow redirects. Downloads require write and delete
access to both the destination and its `.part` file. The private runtime marker
permits cleanup of an interrupted partial download on the next boot.

The guest chooses HTTP method, destination, headers, body, result interpretation,
pairing, account policy, retry behavior and caches. The host supplies transport
using the existing firmware TLS policy. Request headers remain bounded by the
SDK; bodies are capped at 8 KiB and response buffers at 32 KiB. Restricted file
downloads are capped at 16 MiB. A transport operation has a 60-second overall
deadline in addition to socket deadlines.

Networking uses one owned 8192-byte worker stack while the guest is parked.
The loop remains available to request cancellation. Rendering cannot poll or
execute the guest while that worker owns service state. Exit waits for the
worker to finish before destroying its services or buffers.

On devices without PSRAM:
1. Validate the already-authorized operation and guest ranges.
2. Save a pointer-free CPU continuation and an authenticated encrypted snapshot.
3. Stage a bounded encrypted request in nonce domain 2, release the guest
   allocation and destroy the old service objects.
4. Arm a one-use network ticket and restart before Wi-Fi initialization.
5. Consume the ticket, revalidate the manifest and request, and execute
   transport without allocating guest memory.
6. Stage an encrypted response in nonce domain 1 and arm a separate resume ticket.
7. Restart again, consume the ticket, validate the manifest and guest ranges,
   allocate on a clean heap and resume with the original syscall result.
8. Erase keys and remove operation files.

The RTC ticket uses raw uninitialized storage so C++ initialization cannot erase
it during a software reset. A cold boot, invalid ticket, failed restore, changed
manifest, cancellation or abandoned operation cannot arm a resume loop.
Cancellation after unloading returns through a clean Home reboot; deep sleep
supersedes that reboot. PSRAM devices retain their guest and use direct result
delivery without a networking reboot.

## Input ABI

PREV/NEXT from front Left/Right are unmarked. Configured side PageBack/PageForward
add `DSN_EV_SIDE_BUTTON` (bit 8). Swipes may carry a safe-area origin. Confirm and
Back keep their usual meanings. `timer_set(0)` disables timer events; intervals
from 1000 to 3600000 milliseconds opt into `DSN_EV_TIMER`. Timer wakes do not
restore an unchanged overlay frame.

## Verification

Executable policy tests are in `test/test_dessen_permissions.cpp`. The SDK
host tests exercise real guest continuation, invalid restore state, network
failure results, pointer detachment and input encoding. Python ownership tests
cover the firmware boundaries. The full application has five offline fixture
suites, including cache freshness, pairing, unpairing and missing-RTC behavior.

Firmware compilation, target stack margins, OTA size and device behavior remain
for manual builds and hardware tests. The upstream XPUI examples additionally
depend on an unavailable `xpui-backends` branch named `dessen-support`; core
SDK C/Rust tests and the full application do not require those examples.
