#!/usr/bin/env python3
"""Host-level regression tests for the experimental native-statistics Dessen app."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x32545352
def seal(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return data + struct.pack("<I", h)

def snapshot():
    day = int(time.time() // 86400) + 1
    arr = [0] * 366
    for delta, seconds in enumerate((900, 1800, 1200, 300)):
        arr[(day - delta) % 366] = seconds
    return seal(struct.pack("<8I366I", MAGIC, 2, 17, 12, 9000, 0, 234, day, *arr))

def book(num):
    title = ("Test Book %02d" % num).encode()
    path = ("/books/test%02d.epub" % num).encode()
    author = b"Test Author"
    return seal(struct.pack("<4I176s72s48s", 0x314B4252, 1, num, 0, path, title, author))

def time_buckets():
    return seal(struct.pack("<8I28I", 0x31505452, 1, 17, int(time.time()//86400)+1,
                            3600, 1800, 2400, 1200, *([0] * 28)))

def run(host, app, root, keys):
    env = os.environ.copy()
    env["DESSEN_FS_ROOT"] = str(root)
    p = subprocess.run([host, app], input=keys, env=env, capture_output=True, timeout=15)
    if p.returncode:
        raise AssertionError(f"Host returned {p.returncode}:\n{p.stdout.decode(errors='replace')}\n{p.stderr.decode(errors='replace')}")
    return p.stdout.decode(errors="replace")

def main():
    host, app = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        out = run(host, app, root, b"nnb")
        assert "No valid native reading statistics found." in out
        statsdir = root / ".crosspoint" / "reading-stats"
        statsdir.mkdir(parents=True)
        (statsdir / "global_v2.bin").write_bytes(snapshot())
        (statsdir / "time_global_v1.bin").write_bytes(time_buckets())
        (statsdir / "books_v1.bin").write_bytes(b"".join(book(i) for i in range(1, 7)))
        (statsdir / "book_0000000000000001_v2.bin").write_bytes(snapshot())
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in statsdir.iterdir()}
        out = run(host, app, root, b"nnnnncccccb")
        for expected in ("Total reading", "2 hours", "Reading sessions", "12",
                         "Qualified turns", "234", "Recent streak", "4 days",
                         "Reading Activity", "Reading Calendar", "Active days:", "Time of Day", "Morning", "Night", "Test Book 01", "Test Book 05", "Test Book 06", "Book Statistics", "Reading time"):
            assert expected in out, f"Missing {expected!r}:\n{out}"
        after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in statsdir.iterdir()}
        assert before == after, "Dessen must never mutate native statistics"
        (statsdir / "global_v2.bin").write_bytes(b"invalid primary")
        (statsdir / "global_v2.bin.bak").write_bytes(snapshot())
        out = run(host, app, root, b"b")
        assert "Total reading" in out and "12" in out
        (statsdir / "global_v2.bin.bak").write_bytes(b"invalid backup")
        out = run(host, app, root, b"b")
        assert "No valid native reading statistics found." in out
        print("Dessen native Reading Statistics: PASS (empty, totals, activity, calendar, time of day, book list/details, paging, read-only, recovery, corrupt records)")

if __name__ == "__main__":
    main()
