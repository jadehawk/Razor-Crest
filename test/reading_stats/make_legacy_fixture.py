#!/usr/bin/env python3
r"""Generate disposable legacy-reading-statistics SD-card fixtures.

Usage: python test/reading_stats/make_legacy_fixture.py --output C:\Temp\legacy-stats-tests
Will REFUSE an existing output directory. Do not apply to a personal SD card:
the fixture deliberately contains synthetic native and legacy reading records.
"""
import argparse
import json
import struct
import zipfile
from pathlib import Path


def fnv64(text: str) -> int:
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def fnv32(binary: bytes) -> int:
    value = 2166136261
    for byte in binary:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def write(path: Path, contents: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)


def seed_epub(root: Path, name: str):
    dest = root / "FixtureBooks" / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w") as archive:
        archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        archive.writestr(
            "META-INF/container.xml",
            '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            '<rootfiles><rootfile full-path="OEBPS/content.opf"'
            ' media-type="application/oebps-package+xml"/></rootfiles></container>',
        )
        archive.writestr(
            "OEBPS/content.opf",
            '<?xml version="1.0" encoding="utf-8"?>'
            '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
            '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
            f'<dc:title>Fixture {name}</dc:title><dc:identifier id="id">{name}</dc:identifier>'
            '<dc:language>en</dc:language></metadata><manifest>'
            '<item id="x" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
            '</manifest><spine><itemref idref="x"/></spine></package>',
        )
        archive.writestr(
            "OEBPS/chapter.xhtml",
            '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Synthetic import fixture.</p></body></html>',
        )


def crossink_book(seconds: int, turns: int) -> bytes:
    contents = bytearray(73)
    contents[0] = 5
    struct.pack_into("<HII", contents, 1, 2, seconds, turns)
    return bytes(contents)


def native_snapshot(seconds: int, sessions: int, turns: int, day: int, sequence: int) -> bytes:
    buckets = [0] * 366
    buckets[day % 366] = seconds
    data = struct.pack(
        "<8I366I", 0x32545352, 2, sequence, sessions, seconds & 0xFFFFFFFF,
        seconds >> 32, turns, day, *buckets,
    )
    return data + struct.pack("<I", fnv32(data))


def book_entry(path: str, title: str, author: str) -> bytes:
    identity = fnv64(path)
    def fit(text: str, limit: int) -> bytes:
        raw = text.encode("utf-8")
        if len(raw) >= limit:
            raise ValueError("Fixture title/path too long")
        return raw + b"\x00" * (limit - len(raw))
    data = struct.pack("<4I176s72s48s", 0x314B4252, 1, identity & 0xFFFFFFFF,
                       identity >> 32, fit(path, 176), fit(title, 72), fit(author, 48))
    return data + struct.pack("<I", fnv32(data))


def generate(output: Path):
    if output.exists():
        raise ValueError(f"Refusing to overwrite existing path: {output}")

    paths = ["/FixtureBooks/A.epub", "/FixtureBooks/B.epub"]
    for mode in ("crossink_sd", "vcodex_sd"):
        root = output / mode
        for name in ("A.epub", "B.epub"):
            seed_epub(root, name)

    crossink = output / "crossink_sd"
    global_bytes = bytearray(159)
    global_bytes[0] = 3
    struct.pack_into("<IIII", global_bytes, 1, 4, 3661, 30, 0)
    write(crossink / ".crosspoint/global_stats.bin", global_bytes)
    for path, seconds, turns in zip(paths, (3600, 61), (29, 1)):
        cache = f".crosspoint/epub_{fnv64(path)}/stats_v5.bin"
        write(crossink / cache, crossink_book(seconds, turns))
    write(
        crossink / ".crosspoint/recent.json",
        json.dumps({"books": [
            {"path": paths[0], "title": "Fixture A", "author": "Test Author", "coverBmpPath": ""},
            {"path": paths[1], "title": "Fixture B", "author": "Test Author", "coverBmpPath": ""},
        ]}).encode("utf-8"),
    )

    vcodex = output / "vcodex_sd"
    data = {
        "formatVersion": 6,
        "readingDays": [],
        "legacyReadingDays": [],
        "sessionLog": [],
        "books": [
            {
                "bookId": f"fixture-id-{i}",
                "path": path,
                "knownPaths": [],
                "title": "Fixture " + "AB"[i],
                "author": "Test Author",
                "totalReadingMs": seconds * 1000,
                "sessions": 2,
                "readingDays": [{"dayOrdinal": 20500 + i, "readingMs": seconds * 1000}],
            }
            for i, (path, seconds) in enumerate(zip(paths, (3600, 61)))
        ],
    }
    write(vcodex / ".crosspoint/reading_stats.json", json.dumps(data).encode("utf-8"))
    # Fixture A already has native Razor-Crest history; the importer should ADD
    # rather than overwrite this record and must never double-count on retry.
    native = vcodex / ".crosspoint/reading-stats"
    write(native / "global_v2.bin", native_snapshot(125, 1, 2, 20502, 1))
    hash_a = fnv64(paths[0])
    write(native / f"book_{hash_a:016x}_v2.bin", native_snapshot(125, 1, 2, 20502, 1))
    write(native / "books_v1.bin", book_entry(paths[0], "Fixture A", "Test Author"))
    print("Created isolated test SD roots:", crossink, "and", vcodex)
    print("NEVER merge these fixtures into your personal SD card.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    generate(args.output)
