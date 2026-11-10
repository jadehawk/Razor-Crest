#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Firmware event hooks for SD plugins.
//
// The firmware emits a small whitelisted set of events; a plugin's device.json
// subscribes by declaring an "events" section whose handlers are the same
// declarative request objects the catalog vocabulary uses:
//
//   "events": {
//     "reader.exit": {
//       "request": { "method": "POST", "url": "https://...", "headers": {...},
//                    "body": "{\"book\":\"{event.book}\",\"pct\":{event.percent}}" },
//       "toast": "Synced {event.book}"
//     },
//     "sleep.enter": {
//       "download": { "url": "https://.../daily.bmp", "dest": "/sleep.bmp" }
//     }
//   }
//
// A handler is either a "request" (response discarded; an acknowledgement) or
// a "download" (response streamed to `dest` on SD, e.g. a sleep image).
// "require_meta": "<key>" queues an event for that handler only when the
// event's book ("book", or "path" for book.downloaded) has a sidecar with a
// non-empty value for the key.
//
// Execution is deferred: plugin handlers need the network, and events mostly
// fire with WiFi down (leaving the reader, entering sleep). emit() appends one
// JSON line to the plugin's SD outbox (<plugin dir>/events.jsonl) and costs
// nothing beyond that; drain() later replays queued events through the
// declared requests whenever the device is already online. Substitution in
// url/body/headers/toast: {token}, {cfg.KEY}, and the event's {event.*} vars.
//
// sleep.enter exists to act before the chip powers down (a fresh sleep image,
// a pre-sleep progress push), so subscribing implies "connect": true: the
// sleep path may bring WiFi up to deliver it rather than waiting for the next
// online session. Delivery is split around painting the sleep screen:
// downloads that write a sleep-screen source run first so the image can appear
// on this sleep; requests and other downloads run after the screen is on the
// panel with no renderer, so plugin UI can never become the retained frame.
// Delivery stays at-least-once — a failed sleep-time drain leaves the events
// queued for the next drain.
//
// Whitelist only: unknown names in a manifest are ignored with a log line, so
// manifests written against newer firmware degrade gracefully. Event names are
// a compatibility promise (semantic, not activity class names).
namespace pluginevents {

enum class Event : uint8_t {
  ReaderOpen,      // "reader.open"      vars: book
  ReaderExit,      // "reader.exit"      vars: book, percent, end_percent
  ReaderSession,   // "reader.session"   vars: book, document, active-session summary
  BookDownloaded,  // "book.downloaded"  vars: path, title, plugin
  SleepEnter,      // "sleep.enter"      vars: book, percent, end_percent when sleeping from a reader, else none
  COUNT
};

struct Var {
  const char* key;    // name inside {event.*}, e.g. "book"
  const char* value;  // NUL-terminated UTF-8
};

// Re-reads every installed plugin's device.json "events" section into the
// static subscription table. Cheap relative to its callers (a handful of small
// SD reads); call at boot and whenever plugins may have changed (plugin list
// opened, web-server session ended).
void refreshSubscriptions();

// True when at least one plugin subscribes to `e` (a few string compares).
bool anySubscriber(Event e);

// Bit per Event the named plugin subscribes to (0 = none), for disclosing
// what leaves the device. Reflects the last refreshSubscriptions().
uint8_t subscriptionMask(const char* plugin);

// Which handlers a drain may run. The sleep path splits its work so plugin UI
// cannot become the retained sleep frame: sleep-image downloads run before the
// screen paints, everything else after it.
enum class DrainKind : uint8_t {
  All,         // every queued handler (ordinary online sessions)
  SleepImage,  // only downloads that write a sleep-screen source
  Deferred,    // requests and downloads that do not feed the sleep screen
};

// True when a queued event belongs to a handler of `kind` marked
// "connect": true (sleep.enter subscriptions imply it; see above): the sleep
// path may then bring WiFi up, bounded, so delivery happens before the chip
// powers down instead of in the next online session.
bool wantsConnect(DrainKind kind);

// Appends the event to each subscribing plugin's outbox. No-op without
// subscribers. An outbox over the size cap is dropped wholesale first (the
// newest events carry the current state; stale ones are worthless by then).
void emit(Event e, const Var* vars, size_t varCount);

// Replays queued events through their manifest handlers. Call only while WiFi
// is connected; requests run synchronously on the calling task. At most
// `maxEvents` handlers run per call (each is a bounded HTTPS request) so a
// backlog cannot stall an interactive session. Successfully delivered events
// leave the outbox; failures stay for the next drain. `kind` limits the run to
// one half of the sleep-entry split; skipped handlers stay queued untouched.
// When `renderer` is non-null, a handler's "toast" template is shown via the
// standard popup after its request succeeds; the sleep path passes nullptr so
// no plugin UI can overwrite the retained sleep frame.
void drain(GfxRenderer* renderer, DrainKind kind = DrainKind::All, size_t maxEvents = 4);

}  // namespace pluginevents
