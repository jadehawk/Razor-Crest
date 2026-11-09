# Native Vocabulary & Highlights (experimental Razor-Crest)

## Milestone 2 — validated

The initial Razor-Crest native reader has been tested on device. It renders the installed plugin's
versioned `device.json` marker `local_library.source = "vocabulary-highlights.v1"`
through existing firmware activities, without executing plugin JavaScript.
Stock CrossPoint continues to use the normal on-device README and browser WebUI.

The on-device viewer offers offline Vocabulary/Highlights sections, book and entry
lists, sorting, and full saved definitions/passages. It reads only the SD-root
`/Vocabulary.txt` and `/My Clippings.txt`, regardless of manifest path values.

## Milestone 3 — device-tested native progress editing

The device-tested implementation adds a native `VocabularyHighlightsState` interface
to the same saved-state contract as the browser:

- `/.crosspoint/vocabulary-highlights/state-v1.json`
- Falls back to `/.crosspoint/vocabulary-builder/state-v1.json` if the new
  state is absent, retaining the legacy file as a backup
- Version 1 JSON with `statuses` containing `learning`/`mastered` per
  `vocab:<32-hex-id>` and `hidden` containing true-valued tombstones
- Long-press a vocabulary row to change status, hide the entry, restore it,
  or open entry sorting
- Long-press a book row to sort or toggle **Show deleted** for vocabulary
- Normal vocabulary lists omit entries hidden in either native firmware or WebUI
- A confirmation warns that hiding does not delete firmware export records
- Saved files are read and validated fresh before every edit; updates go to
  a temporary file, close, and replace the prior state file
- Unrelated statuses, clipping tombstones and other JSON metadata are preserved

**Protection:** No source export file is rewritten. Corrupt, unsupported or
oversized state disables native editing rather than resetting progress.
The bounded native state-reader cap is **8 KiB on X3** and **24 KiB on other
boards**, deliberately lower than the WebUI's maximum of 240 KiB.
Large progress files remain editable in the browser WebUI.

**Not implemented:** Native highlight hide/restore editing. Browser highlights
have content-derived IDs with Unicode-sensitive hashing and occurrence numbers;
those must be reproduced exactly before a native editing operation can safely
target them. Highlights continue to be read-only on-device, including highlights
hidden in the browser.

## Resource limits and validation

- Records scan in 256-byte chunks with a 6 KiB record cap.
- X3: 64 books, 192 visible entries per selected book.
- Other boards: 128 books, 512 visible entries per selected book.
- Larger lists require an SD-backed paginated index.
- Milestone 3 was reported passed on Razor-Crest firmware. Other device targets
  and large-library edge cases still need separate coverage.
- Verify round trips with an existing WebUI-generated state, missing state,
  legacy-state migration, invalid state, and a file exceeding native size limits.
- The firmware build and on-device test were performed by the user. No push or
  release is included in this local integration.

## Worktrees

Milestone 2, device-tested and committed:
`crosspoint-reader-(Razor-Crest)/.thinkforge-worktrees/vocabulary-highlights-native`

Milestone 3, device-tested:
`crosspoint-reader-(Razor-Crest)/.thinkforge-worktrees/vocabulary-highlights-native-state`

Companion plugin:
`vocabulary&highlights.crosspoint-plugin/vocabulary-highlights`.
