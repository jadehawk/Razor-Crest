# Library series and index display

The Library keeps its existing Title, Author and Recent sort tabs. With
**Read book metadata** enabled, each visible Library row now shows:

- Book title (first line)
- Author (second line, except where the Author tab already groups by author)
- Series name and the optional series index (next subtitle line)

Example: `Saga of the Forgotten Warrior #4`. A book without series metadata
has no extra line or blank placeholder. Index values are formatted to at most
two decimal places, dropping insignificant trailing zeros.

The library index schema is now **version 3**. Firmware rejects its old
on-disk index and performs the normal one-time Library index rebuild; it
extracts and stores series information alongside title and author. Subsequent
Library visits read only the visible rows from the index. No EPUB files are
opened during list repaint or scrolling.

Both metadata extraction and series are optional; disabled metadata mode,
unreadable books, and formats without series metadata continue showing title
and author only. The pinned Recent overlay remains bounded at ten entries,
including on PSRAM devices storing up to 20 recent books.

Initial rebuilding EPUB metadata has an additional OPF read for series.
This costs SD-card I/O once during the build, not on each visible Library page.
X3 responsiveness and rebuild duration should be checked on hardware.
