# Reading Statistics for Dessen (experimental)

A separate, read-only Dessen app for the Razor-Crest native statistics store.
Existing native Reading Statistics plugin, imports, sync credentials, and
collector stay unchanged. This is **not full visual or feature parity** yet:
server sync, pairing and native import controls remain in the existing plugin.

Build the app with WSL using `make` from this directory (the Dessen SDK is
at `../../dessen`). No firmware compilation is required. Run `make test`
for host fixtures. Copy `build/app.bin` to
`/.crosspoint/plugins/reading-statistics-dessen/app.bin` and
`manifest.json` to the same SD folder. Dessen must be enabled in firmware.

Pages: overall totals; 28-day activity; monthly calendar; time-of-day distribution; indexed books; per-book details. The calendar is UTC-based and may differ from local time near midnight.
Physical Prev/Next switch pages. On Books, Confirm pages through groups of five;
Next opens the first book's detail. In Book Statistics, Confirm advances to the
next book and Back returns to the book list. Elsewhere Back exits.
Touch taps generally advance pages; tapping a book row opens its details.

Data is never modified. The app verifies native FNV-1a checksums and native
magic/version, falls back to the global backup on a damaged primary, and
reads book index rows individually. Streak uses UTC today from the Dessen clock
and is approximate near local midnight (native firmware uses local civil days).
