#!/usr/bin/env python3
"""Create artificial Razor-Crest binary stats, including two book entries.

Writes to a local fixture directory, never directly to an SD card.
"""
from __future__ import annotations

import argparse
from datetime import date
from pathlib import Path
import struct

MAGIC = 0x32545352
VERSION = 2
DAYS = 366
SNAPSHOT_SIZE = (9 + DAYS) * 4
BOOK_INDEX_MAGIC = 0x314B4252
BOOK_INDEX_SIZE = 316


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def path_hash(path: str) -> int:
    value = 14695981039346656037
    for byte in path.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def day_minutes(ago: int) -> int:
    minutes = ((ago * 11 + 17) % 67) + (0 if ago % 5 else -10)
    return 0 if ago % 6 == 0 else max(minutes, 0)


def snapshot(day: int, part: int = 0) -> bytes:
    values = [0] * DAYS
    for ago in range(28):
        seconds = day_minutes(ago) * 60
        if part == 1:
            seconds = seconds // 2
        elif part == 2:
            seconds -= seconds // 2
        values[(day - ago) % DAYS] = seconds
    total = sum(values)
    sessions = sum(1 for v in values if v >= 60)
    turns = (290 if part == 1 else 283 if part == 2 else 573)
    words = [MAGIC, VERSION, 26, sessions, total & 0xFFFFFFFF, total >> 32,
             turns, day, *values]
    result = struct.pack("<" + "I" * len(words), *words)
    result += struct.pack("<I", fnv1a(result))
    assert len(result) == SNAPSHOT_SIZE
    return result


def field(value: str, length: int) -> bytes:
    encoded = value.encode("utf-8")[:length - 1]
    return encoded + bytes(length - len(encoded))


def book_entry(path: str, title: str, author: str) -> bytes:
    digest = path_hash(path)
    result = struct.pack("<IIII", BOOK_INDEX_MAGIC, 1,
                         digest & 0xFFFFFFFF, digest >> 32)
    result += field(path, 176) + field(title, 72) + field(author, 48)
    result += struct.pack("<I", fnv1a(result))
    assert len(result) == BOOK_INDEX_SIZE
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("test/reading_stats/sample_sd"))
    parser.add_argument("--book-path", default="/Books/Example Book.epub",
                        help="exact path of first sample book on device")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    target = args.out / ".crosspoint" / "reading-stats"
    target.mkdir(parents=True, exist_ok=True)
    day = (date.today() - date(1970, 1, 1)).days + 1

    books = [
        (args.book_path, "Example Reading Book", "Test Author", 1),
        ("/Books/Another Example.epub", "Another Example", "Second Author", 2),
    ]
    files = {"global_v2.bin": snapshot(day)}
    index = bytearray()
    for path, title, author, part in books:
        files[f"book_{path_hash(path):016x}_v2.bin"] = snapshot(day, part)
        index += book_entry(path, title, author)
    files["books_v1.bin"] = bytes(index)

    for name, data in files.items():
        output = target / name
        if output.exists() and not args.force:
            raise SystemExit(f"Refusing to overwrite {output}; use --force")
        output.write_bytes(data)
        print(output)
    print("Artificial data only: never merge fixture counters into real statistics.")


if __name__ == "__main__":
    main()
