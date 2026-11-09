# Local Notes native plugin host (experimental Razor-Crest)

## Objective

Open a small, offline, firmware-rendered list browser for local vocabulary and
highlight exports from an installed SD plugin. No plugin-side JavaScript runs
inside the e-reader. This host is optional; stock CrossPoint uses the normal
README fallback. This is a pre-build experimental implementation until
physical X3 and touch-device verification.

## Contract

An installed plugin supplies a device.json with the source marker
"local_library": {"source": "vocabulary-highlights.v1"}.
The firmware reads the two fixed SD-root exports /Vocabulary.txt and
/My Clippings.txt. Manifest-supplied paths are deliberately ignored.
A companion manifest.json may add a home_shortcut with target "dashboard".

Razor-Crest advertises ui.local-notes.v1 in pluginCapabilities.
Stock CrossPoint continues to show the plugin README on-device, and the
ordinary browser WebUI remains functional on both firmware families.

## Current native behavior

- Home shortcut and Home > Plugins entry open an offline e-ink activity
- Choose Vocabulary or Highlights, select a book, select a saved entry
- Reuses UiListActivity for touch and button navigation
- Reuses DictionaryDefinitionActivity to display full saved details
- Long-press Select or long-press a touch row for sorting options
- Sorting: books A-Z/recent; words A-Z/reading order/recent;
  highlights reading order/recent
- Reads exports in 256-byte chunks with 6 KiB maximum record buffers;
  incomplete JSONL tails and clipping blocks are ignored
- The first-stage limits are 64 books/192 entries per book on X3 and
  128 books/512 entries per book on other boards. Limits are visibly
  reported in the on-device UI.

## Before broad rollout

- Native editing of New/Learning/Mastered and delete/restore is not yet
  implemented. These remain in the WebUI. This first-stage viewer does not
  apply hidden-entry flags either, so items hidden in the WebUI still appear
  in the native read-only list. Future native state writes must
  use the shared versioned state-v1.json and must not rewrite source exports.
- Larger libraries need an SD-backed paginated index instead of retaining
  all record keys in RAM. Current caps are not suitable for unlimited libraries.
- Highlights use saved page/chapter, vocabulary uses spine/page/text offset.
- Validate physically on no-PSRAM X3, non-touch X4, and Picco/X4Pro.
- No firmware build, push or release is implied by this implementation.

## Paths

Firmware worktree:
crosspoint-reader-(Razor-Crest)/.thinkforge-worktrees/vocabulary-highlights-native

Plugin repository:
vocabulary&highlights.crosspoint-plugin/vocabulary-highlights
