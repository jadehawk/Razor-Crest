#!/usr/bin/env python3
"""Non-destructive Razor-Crest RST2 -> JSONL migration rehearsal.

Run on a COPY of the SD history, never on the live card. Output is a new
directory and the old session log stays byte-for-byte at the start of the
new file. Historical snapshot totals replace the aggregate contribution of
that prefix; original events remain available to stock sync upload.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MAX_LINE = 511
MAX_BOOKS = 2048
DAYS = 366
TIME_DAYS = 7
PREFIX = "rc-baseline-"


def fnv(data, value=2166136261, bits=32):
    prime = 16777619 if bits == 32 else 1099511628211
    for byte in data:
        value = ((value ^ byte) * prime) & ((1 << bits) - 1)
    return value


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checked(path, size, magic, version):
    """Do not silently promote a possibly stale backup over corrupt primary."""
    data = path.read_bytes()
    require(len(data) == size, f"Wrong size: {path}")
    fields = struct.unpack("<" + "I" * (size // 4), data)
    require(fields[:2] == (magic, version) and fields[-1] == fnv(data[:-4]),
            f"Invalid checksum or version: {path}")
    return fields


def available(path, size, magic, version, required=False):
    backup = path.with_name(path.name + ".bak")
    if path.exists():
        return checked(path, size, magic, version)
    require(not backup.exists(), f"Primary missing but backup exists: {path}; recover first")
    require(not required, f"Required source missing: {path}")
    return None


def decode_text(field):
    require(b"\0" in field, "Unterminated catalog string")
    return field.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def day_number(ordinal):
    # Legacy ordinal is Unix day + 1; zero is undated.
    if not ordinal:
        return 0
    day = ordinal - 1
    require(10957 <= day <= 47482, f"Unsupported historical day: {ordinal}")
    return day


def snapshot(folder, hash_name=None, required=False):
    name = "global_v2.bin" if hash_name is None else f"book_{hash_name}_v2.bin"
    time_name = "time_global_v1.bin" if hash_name is None else f"time_{hash_name}_v1.bin"
    snap = available(folder / name, 1500, 0x32545352, 2, required)
    time = available(folder / time_name, 148, 0x31505452, 1)
    if snap is None:
        require(time is None, f"Time profile without snapshot: {name}")
        return {"seconds": 0, "sessions": 0, "turns": 0, "latest": 0,
                "periods": [0] * 4, "days": {}, "week": {}, "sequence": 0}
    seq, sessions, lo, hi, turns, latest = snap[2:8]
    total = lo | (hi << 32)
    daily = {}
    if latest:
        day_number(latest)
        for ordinal in range(max(1, latest - DAYS + 1), latest + 1):
            value = snap[8 + ordinal % DAYS]
            if value:
                daily[str(day_number(ordinal))] = value
    periods, week = [0] * 4, {}
    if time:
        require(time[2] <= seq, f"Time profile is newer than snapshot: {name}")
        periods = list(time[4:8])
        end = time[3]
        if end:
            day_number(end)
            for ordinal in range(max(1, end - TIME_DAYS + 1), end + 1):
                row = list(time[8 + (ordinal % TIME_DAYS) * 4:12 + (ordinal % TIME_DAYS) * 4])
                if any(row):
                    week[str(day_number(ordinal))] = row
    require(sum(daily.values()) <= total and sum(periods) <= total,
            f"Historical counters disagree: {name}")
    return {"seconds": total, "sessions": sessions, "turns": turns,
            "latest": day_number(latest), "periods": periods, "days": daily,
            "week": week, "sequence": seq}


def book_catalog(folder):
    path = folder / "books_v1.bin"
    require(path.exists(), "Missing native book catalog")
    data = path.read_bytes()
    require(len(data) % 316 == 0 and len(data) // 316 <= MAX_BOOKS, "Invalid catalog length")
    result, hashes, ids = [], set(), set()
    for offset in range(0, len(data), 316):
        row = data[offset:offset + 316]
        magic, version, lo, hi = struct.unpack_from("<4I", row)
        require((magic, version) == (0x314B4252, 1) and
                struct.unpack_from("<I", row, 312)[0] == fnv(row[:312]),
                f"Invalid catalog row at {offset}")
        name = decode_text(row[16:192])
        title = decode_text(row[192:264])
        author = decode_text(row[264:312])
        hash_id = lo | (hi << 32)
        require(name.startswith("/") and name.rsplit("/", 1)[-1] and
                fnv(name.encode("utf-8"), 14695981039346656037, 64) == hash_id,
                f"Catalog path is truncated or mismatched: {name}")
        # Explicit filename-mode rehearsal. Binary matching requires actual
        # EPUB files or an explicit verified mapping, never fabricated IDs.
        doc = hashlib.md5(name.rsplit("/", 1)[-1].encode("utf-8")).hexdigest()
        require(hash_id not in hashes and doc not in ids, f"Duplicate path or document: {name}")
        hashes.add(hash_id)
        ids.add(doc)
        result.append({"path": name, "title": title, "author": author,
                       "hash": f"{hash_id:016x}", "doc": doc})
    return result


def encode(row):
    data = json.dumps(row, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    require(len(data) <= MAX_LINE, f"JSONL record exceeds {MAX_LINE} bytes: {row.get('type')}")
    return data + b"\n"


def record(kind, **fields):
    return {"type": PREFIX + kind, "v": 1, **fields}


def make_rows(global_stats, books):
    rows = []
    scopes = [(None, global_stats)] + [(book, book["stats"]) for book in books]
    # Totals before day records; the inferred date anchor covers all scopes.
    for book, stats in scopes:
        fields = {k: stats[k] for k in ("seconds", "sessions", "turns", "latest", "periods")}
        if book:
            fields.update({k: book[k] for k in ("doc", "title", "author", "path")})
            if "to" in book:
                fields["to"] = book["to"]
        row = record("total", **fields)
        # Metadata is optional; numeric historical data is never truncated.
        try:
            encode(row)
        except ValueError:
            row.pop("path", None)
            encode(row)
        rows.append(row)
    for book, stats in scopes:
        key = {"doc": book["doc"]} if book else {}
        for day, seconds in sorted(stats["days"].items(), key=lambda item: int(item[0])):
            rows.append(record("day", **key, day=int(day), seconds=seconds))
        # Preserve per-book weekly data even though current UI only consumes
        # global weekly periods. These records are not synthesized sessions.
        for day, periods in sorted(stats["week"].items(), key=lambda item: int(item[0])):
            rows.append(record("week", **key, day=int(day), periods=periods))
    return rows


def rehearse(source, output):
    require(source.resolve() != output.resolve() and not output.exists(),
            "Choose a new output directory; refusing overwrite")
    folder = source / "reading-stats"
    require(folder.is_dir(), "Missing Razor-Crest reading-stats directory")
    for path in folder.iterdir():
        # Reject unfinished transactions; the running firmware must recover
        # them before a new snapshot can be migrated.
        require(not re.match(r"(?:import_)?pending_v1\.bin(?:\..*)?$", path.name),
                f"Unrecovered transaction: {path.name}")
        require(not path.name.endswith(".tmp"), f"Unfinished write: {path.name}")
    global_stats = snapshot(folder, required=True)
    books = book_catalog(folder)
    known = {b["hash"] for b in books}
    for path in folder.iterdir():
        match = re.fullmatch(r"(?:book_([0-9a-f]{16})_v2|time_([0-9a-f]{16})_v1)\.bin(?:\.bak)?", path.name)
        if match:
            require((match[1] or match[2]) in known, f"Orphan snapshot: {path.name}")
    by_path = {b["path"]: b for b in books}
    for book in books:
        book["stats"] = snapshot(folder, book["hash"])
        require(book["stats"]["sequence"] <= global_stats["sequence"],
                f"Book snapshot newer than global: {book['path']}")
    source_log = source / "reading_sessions.jsonl"
    original = source_log.read_bytes() if source_log.exists() else b""
    require(not original or original.endswith(b"\n"), "Incomplete original JSONL tail; recover before migration")
    old_sessions = old_seconds = 0
    for line in original.splitlines():
        require(len(line) <= MAX_LINE, "Overlong original record; inspect before migration")
        entry = json.loads(line)
        require(isinstance(entry, dict) and entry.get("type", "session") == "session",
                "Existing extension/baseline records; refusing a second cutover")
        book = by_path.get(entry.get("path"))
        # Missing path is safe only with an exact known document ID.
        if book is None and not entry.get("path"):
            book = next((b for b in books if b["doc"] == entry.get("doc")), None)
        require(book is not None and book["doc"] == entry.get("doc"),
                f"Source session is not covered by filename-mode catalog: {entry.get('doc')}")
        require(type(entry.get("secs")) is int and 10 <= entry["secs"] <= 604800, "Invalid session duration")
        progress = entry.get("to")
        if type(progress) is int and 0 <= progress <= 10000:
            book["to"] = progress
        book["original_events"] = book.get("original_events", 0) + 1
        book["original_event_seconds"] = book.get("original_event_seconds", 0) + entry["secs"]
        old_sessions += 1
        old_seconds += entry["secs"]
    rows = make_rows(global_stats, books)
    body = b"".join(map(encode, rows))
    migrated = (original + encode(record("begin", records=len(rows))) + body +
                encode(record("end", hash=fnv(body))))
    require(migrated[:len(original)] == original, "Original prefix changed")
    output.mkdir(parents=True)
    (output / "reading_sessions.jsonl").write_bytes(migrated)
    cursor = source / "reading_sessions_sync.json"
    if cursor.exists():
        (output / cursor.name).write_bytes(cursor.read_bytes())
    report = {"mode": "filename", "source": str(source.resolve()),
              "prefix_bytes": len(original), "prefix_sha256": hashlib.sha256(original).hexdigest(),
              "original_events": old_sessions, "original_event_seconds": old_seconds,
              "global": global_stats, "books": books, "baseline_records": len(rows),
              "output_bytes": len(migrated), "output_sha256": hashlib.sha256(migrated).hexdigest(),
              "source_untouched": True, "cursor_copied": cursor.exists()}
    (output / "expected.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Rehearsal: {global_stats['sessions']} sessions, {global_stats['seconds']} seconds, "
          f"{global_stats['turns']} qualified turns, {len(books)} catalog books")
    print(f"Preserved {len(original)} original JSONL bytes; output {len(migrated)} bytes")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--match", choices=("filename",), required=True)
    args = parser.parse_args()
    try:
        rehearse(args.source, args.output)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"Migration refused: {exc}\n")


if __name__ == "__main__":
    main()
