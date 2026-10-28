# Single-card carousel primitives

These capabilities are firmware features, available to any plugin that detects the
advertised capability; they are not requirements tied to a firmware name.

## Home: `home.book-carousel.v1`

**Setting:** Display > Enable Continue Reading Carousel (enabled by default, persisted under continueReadingCarousel). The setting enables cyclic navigation inside one
full-size native SD-theme `book-card` of variant `cover`, where the theme
declares a Home scene. Existing cover-grid layouts are unaffected.

- One book card remains visible; never adjacent covers or a cover-strip.
- A single Home focus slot represents the entire card, regardless of the number
  of books. The front Left/Right buttons navigate between Home items.
- When focused and more than one eligible book exists, side Up/Down cycle the
  displayed book on button-only readers. Confirm opens that displayed book.
- Touch devices swipe left/right beginning inside the card; swipe gestures
  beginning at the left-edge Back zone are reserved for system navigation.
  Tap opens the displayed book.
- Cycling wraps around. The remembered selection is retained while Home is
  revisited in the same firmware session; missing/finished books are skipped.
- One eligible book keeps the original single-card presentation and normal
  side-button navigation. The Back/Resume shortcut opens the displayed book.
- The source is the Recent Books store, not a full-library scan.
  - Without PSRAM, the Recent Books storage ceiling and carousel maximum stay at 10.
  - With PSRAM, the storage ceiling is 50. The Library pinned overlay stays at 10.
- Display > Continue Reading Book Limit offers Auto (10 or 50 by hardware), 5,
  10, and on PSRAM devices 20, 30, 40, or 50. This limit controls recent
  candidates, not the number of decoded cover images. The stored
  continueReadingBookLimit value is zero for Auto and is clamped at use.
- Completion and series metadata are checked lazily for the displayed book.
  Completed candidates are skipped when reached; unknown progress stays
  eligible. A candidate not yet inspected is not yet known to be completed.
- Full-size card dimensions, title, author, series/index and progress remain
  firmware-owned. No book imagery is provided by an SD theme package.
- No new partial/neighbor cards, no network calls, no additional persistent
  book database. The current cover snapshot is invalidated on change, and
  only the displayed book's thumbnail is generated on demand.
- The optional theme scene style `book_carousel_rail_px` is an integer 4..12
  (default 8) controlling each side's selected-card rail width. When that
  option is declared, the theme package should require
  `home.book-carousel.v1`. The rails appear only for a focused card when
  more than one eligible book exists.

## Reusable single-card widget: `ui.carousel.v1`

Firmware provides a generic stateless C++ pager in
`src/util/SingleCardPager.h` and a bounded declarative plugin widget. This
is a **single complete content card switcher**, not a grid of book covers.

A native `device.json` plugin may use a dashboard with source `cards.v1`;
its one page contains a single widget of type `card_carousel`. The same
widget is supported in a normal `reading-stats.v1` dashboard page, at most
one carousel widget per dashboard. The plugin declares its own static
information and may regenerate its manifest to change card content; it does
not execute plugin code in the renderer.

Example:

```json
{
  "title": "Sample Carousel",
  "dashboard": {
    "source": "cards.v1",
    "pages": [{
      "title": "Items",
      "widgets": [{
        "type": "card_carousel",
        "border": "rounded",
        "corner_radius": 8,
        "cards": [
          {"title": "First item", "subtitle": "Example plugin data",
           "value": "Ready", "progress": 60},
          {"title": "Second item", "subtitle": "Full card remains visible",
           "value": "Review next", "progress": 20}
        ]
      }]
    }]
  }
}
```

Each card accepts title (required, up to 70 characters), subtitle (up to 80),
value (up to 48), and optional progress (0..100; omit to suppress). Maximum
eight cards per carousel; total plugin `device.json` remains bounded by the
8 KiB dashboard manifest limit.

Side Up/Down cycle the cards on the active dashboard page; Left/Right retain
dashboard page navigation. A touch swipe left/right beginning inside card
content cycles, while edge-origin Back gestures remain reserved. Confirm
advances to the next static card. For one card no cycling rails are drawn.

Plugin manifests requiring the widget should advertise the capability
requirement `ui.carousel.v1`. Detect the capability rather than
hard-coding Razor-Crest. No backwards-compatibility implementation is needed.

## Testing and memory discipline

Host-test the index wrap/disable cases, X3 input focus isolation and plugin
widget parser bounds. Test actual X3 firmware before X4 Pro. The plugin
declaration is static and capped; it must not cache an array of decoded
book-cover bitmaps. Real e-ink refresh cadence, swipe conflict checks and
memory headroom require hardware validation.
