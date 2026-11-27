#!/usr/bin/env python3
"""Regression checks for the Rust Dessen port of Razor-Crest native statistics."""
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
from datetime import date, datetime

def seal(data):
    h=2166136261
    for b in data:
        h=((h ^ b)*16777619)&0xffffffff
    return data+struct.pack("<I",h)

def snapshot(secs=9000, sessions=12, turns=234):
    ordinal=(date.today()-date(1970,1,1)).days+1
    hist=[0]*366
    for delta,value in enumerate((1800,900,1200)):
        hist[(ordinal-delta)%366]=value
    return seal(struct.pack("<8I366I",0x32545352,2,17,sessions,secs,0,turns,ordinal,*hist))

def index_book(key,title,path):
    return seal(struct.pack("<4I176s72s48s",0x314b4252,1,key,0,
                            path.encode(), title.encode(), b"Test Author"))

def period_data():
    return seal(struct.pack("<8I28I",0x31505452,1,17,(date.today()-date(1970,1,1)).days+1,
                            3600,1800,2400,1200,*([0]*28)))

def run(host,app,root,keys, touch=True, one_book=False, series=False, portrait=False):
    env=os.environ.copy()
    if touch: env['DESSEN_TEST_TOUCH']='1'
    else: env['DESSEN_TEST_HINTS']='1'
    if one_book: env['DESSEN_TEST_ONE_BOOK']='1'
    if series: env['DESSEN_TEST_SERIES']='1'
    if portrait: env['DESSEN_TEST_PORTRAIT']='1'
    env["DESSEN_FS_ROOT"]=str(root)
    proc=subprocess.run([host,app],input=keys,env=env,capture_output=True,timeout=20)
    assert proc.returncode==0,(proc.returncode,proc.stderr[-1000:],proc.stdout[-1000:])
    return proc.stdout.decode(errors="replace")

def main(host,app):
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        unpaired = run(host, app, root, b"dddddddd")
        assert unpaired.index("Device Statistics") < unpaired.index("Current Book")
        assert "Server Statistics" not in unpaired
        assert "Server Sync" not in unpaired and "Settings" in unpaired
        backwards = run(host, app, root, b"uuuuuuuu")
        assert "Server Statistics" not in backwards
        assert "Server Sync" not in backwards and "Settings" in backwards

        token = root / ".crosspoint" / "reading-statistics-token.json"
        token.parent.mkdir()
        token.write_text('{"token":"tnstats_' + 'a' * 64 + '"}')
        empty=run(host,app,root,b"b")
        assert len(re.findall(r"(?m)^text \d+,\d+ N/A$", empty.split("refresh", 1)[0])) == 9, empty[:4200]
        assert "Server Statistics" in empty and "THIS MONTH" in empty and "Continue Reading" in empty,empty[-2000:]
        assert "Best Day" in empty and "Daily Avg." in empty
        assert not re.search(r"(?m)^text \d+,\d+ Total Time$", empty), "Repeated total time widget"
        assert not re.search(r"(?m)^text \d+,\d+ Books$", empty.split("Continue Reading")[0].split("Avg. Session")[-1]), "Repeated books widget"
        # The desktop shelf service provides real fixture books. Their names
        # and progress must come from that service, not hardcoded UI samples.
        for value in ("Dune", "37%", "Frank Herbert", "1/2"):
            assert value in empty, f"Missing initial preview content {value}"
        assert "Hyperion" not in empty, "Gallery should show exactly one book at a time"
        with_series = run(host,app,root,b"b",series=True)
        assert "#6 - Saga of the Forgotten Mountain" in with_series, "Carousel must preserve series and index"
        assert "37%" in with_series, "Series column must not corrupt progress parsing"
        assert re.search(r"(?m)^text \d+,\d+ 1/2$", with_series), "Counter missing from carousel header"
        # Series and index use the shared book hero's two-line box.
        assert re.search(r"(?m)^text \d+,\d+ #6 - Saga of the Forgotten Mountain$", with_series)
        # The 1/N count shares the Continue Reading heading row, rather
        # than consuming a separate row beneath the book card.
        heading = re.search(r"(?m)^text \d+,(\d+) Continue Reading$", with_series)
        counter = re.search(r"(?m)^text \d+,(\d+) 1/2$", with_series)
        assert heading and counter and heading.group(1) == counter.group(1)
        # The background progress strip begins to the RIGHT of the portrait
        # cover column; it must never extend beneath the book image.
        bars = re.findall(r"(?m)^fill (\d+),\d+ \d+x7 c2$", with_series)
        assert bars and all(int(x) > 250 for x in bars), "Progress drawn beneath cover"
        narrow = run(host, app, root, b"b", series=True, portrait=True)
        assert "Daily Avg." in narrow and "37%" in narrow
        assert "#6 - Saga of the Forgotten Mountain" in narrow
        assert "1/2" in narrow
        assert "Continue Reading" in narrow
        # Both layouts reserve two title rows even for short titles; the
        # author must begin at least two full line-heights after the title.
        current_book = run(host, app, root, b"ddb", series=True, touch=False)
        assert "Current Book" in current_book, "Current Book page missing"
        assert "#6 - Saga of the Forgotten Mountain" in current_book
        for trace in (with_series, narrow, current_book):
            title_y = re.search(r"(?m)^text \d+,(\d+) Dune$", trace)
            author_y = re.search(r"(?m)^text \d+,(\d+) Frank Herbert$", trace)
            assert title_y and author_y and int(author_y.group(1)) - int(title_y.group(1)) >= 32, f"Book title lacks two-line allocation: title={title_y.group(1) if title_y else None} author={author_y.group(1) if author_y else None} trace={trace[-1400:]}"
        # The progress bar must stay near the BOTTOM of the hero rather than
        # occupying the second line of title or series metadata.
        for trace in (with_series, current_book):
            author_y = int(re.search(r"(?m)^text \d+,(\d+) Frank Herbert$", trace).group(1))
            series_y = int(re.search(r"(?m)^text \d+,(\d+) #6 - Saga of the Forgotten Mountain$", trace).group(1))
            bar_y = int(re.search(r"(?m)^fill \d+,(\d+) \d+x7 c2$", trace).group(1))
            assert series_y >= author_y + 16
            assert bar_y >= series_y + 32, "Progress bar invades second series line"
        assert "Books Tracked" not in empty, "Monthly card should say Books"
        assert "< Prev" not in empty and "Next >" not in empty, "Touch carousel must have no pill controls"
        button_preview = run(host, app, root, b"b", touch=False)
        for label in ("Exit", "Open", "Prev Book", "Next Book"):
            assert re.search(r"(?m)^text \d+,\d+ " + label + r"$", button_preview), f"Missing physical footer {label}: {button_preview[-2500:]}"
        # X3-style narrow four-key footer measures labels instead of four
        # equal cells; confirm each label fits in its actual stroke rectangle.
        narrow_buttons = run(host, app, root, b"b", touch=False, portrait=True)
        footer_strokes = re.findall(r"(?m)^stroke (\d+),764 (\d+)x36 w1$", narrow_buttons.split("refresh", 1)[0])
        assert len(footer_strokes) == 4, "Four native theme hint cells missing"
        for (_, width), label in zip(footer_strokes, ("Exit","Open","Prev Book","Next Book")):
            assert int(width) - 8 >= len(label) * 8, f"Footer label {label} clipped"
        # The arrow belongs only on touch screens.
        assert re.search(r"(?m)^line 25,\d+ - 47,\d+$", empty)
        assert not re.search(r"(?m)^line 25,\d+ - 47,\d+$", button_preview)
        front_next = run(host, app, root, b"nb", touch=False)
        assert "Hyperion" in front_next and "2/2" in front_next, "Front Next must browse carousel"
        front_prev = run(host, app, root, b"pb", touch=False)
        assert "Hyperion" in front_prev and "2/2" in front_prev, "Front Prev must loop carousel"
        side_next = run(host, app, root, b"db", touch=False)
        assert "Device Statistics" in side_next, "Side Next must switch statistics pages"
        side_prev = run(host, app, root, b"ub", touch=False)
        assert "Settings" in side_prev, "Side Previous must switch statistics pages"
        single = run(host, app, root, b"nb", touch=False, one_book=True)
        assert "Device Statistics" in single, "With one book, front Next must page"
        single_prev = run(host, app, root, b"pb", touch=False, one_book=True)
        assert "Settings" in single_prev, "With one book, front Prev must page"
        single_home = run(host, app, root, b"b", touch=False, one_book=True)
        assert "Prev Book" not in single_home and "Next Book" not in single_home
        assert "Prev Page" in single_home and "Next Page" in single_home
        assert "1/1" not in single_home, "Single-book carousel has no index"

        # Swipe start inside the card changes the book, and swipes outside
        # keep their original page-navigation behavior.
        inside = run(host, app, root, b"Lbb")
        assert "Hyperion" in inside and "2/2" in inside
        reverse = run(host, app, root, b"Rbb")
        assert "Hyperion" in reverse and "2/2" in reverse
        outside = run(host, app, root, b"lbb")
        assert "Device Statistics" in outside, "Outside swipe should change page"
        looped = run(host, app, root, b"LLbb")
        assert len(re.findall(r"(?m)^text \d+,\d+ 1/2$", looped)) >= 2
        next_book = run(host, app, root, b"nb")
        for value in ("Hyperion", "5%", "2/2"):
            assert value in next_book, f"Missing next book: {value}"
        wrapped = run(host, app, root, b"nnb")
        assert len(re.findall(r"(?m)^text \d+,\d+ 1/2$", wrapped)) >= 2, "Carousel next must loop"
        restored = run(host, app, root, b"db")
        assert "Device Statistics" in restored, "Side Next must preserve page navigation"
        token.unlink()
        folder=root/".crosspoint"
        folder.mkdir(parents=True,exist_ok=True)
        log=folder/"reading_sessions.jsonl"
        now=int(datetime.now().timestamp())
        def event(i,title,path,secs,ago=0):
            return json.dumps(dict(doc=f"{i:032x}",title=title,path=path,secs=secs,start=now-ago*86400,tz=0))
        rows=[event(1,"Dune","/books/dune.epub",1200,i) for i in range(3)]
        rows += [event(2,"Other Book","/books/other.epub",600,i%3) for i in range(9)]
        log.write_text("\n".join(rows)+"\n")
        hashes={log.name:hashlib.sha256(log.read_bytes()).hexdigest()}
        os.environ["DESSEN_NATIVE_STATS"]="1"
        output=run(host,app,root,b"dddddddddddddb")
        device=run(host,app,root,b"db")
        assert "Device Statistics" in device and "Session Start Periods" in device, device[-6000:]
        os.environ["DESSEN_TEST_NARROW"]="1"
        try:
            for wrap in (False, True):
                if wrap: os.environ["DESSEN_TEST_WRAP"]="1"
                narrow=run(host,app,root,b"ddb",portrait=True)
                assert "metric-fit FAIL" not in narrow, narrow[-6000:]
                assert narrow.count("metric-fit ok Avg. Session") >= 2, narrow[-6000:]
                assert "metric-fit ok Reading Time" in narrow, narrow[-6000:]
        finally:
            os.environ.pop("DESSEN_TEST_NARROW",None)
            os.environ.pop("DESSEN_TEST_WRAP",None)
        offline_pages=run(host,app,root,b"dddddbb")
        for value in ("Your Week", "Reading Calendar", "Books • Local", "Last Read Day",
                      "Best Day", "Daily Average", "Reading Consistency",
                      "None", "<15m", "15-44m", "45m+", "Books tracked: 2", "Dune"):
            assert value in offline_pages, f"Missing offline page content {value}:\\n{offline_pages[-5000:]}"
        for value in ["Device Statistics","Current Book","THIS MONTH","12","12m","Session Start Periods",
                      "Morning","Afternoon","Evening","Night","Latest 7 Days",
                      "Dune","Reading Time","Active Days","Avg. Session","1h 0m","3"]:
            assert value in output,f"Missing {value}:\n{output[-5000:]}"
        for value in ("3", "1h 0m", "20m", "12m"):
            assert re.search(r"(?m)^text \d+,\d+ " + re.escape(value) + r"$", output), f"Missing book metric {value}"
        # The shared Time of Day widget must render all four labels on
        # Device Statistics, Current Book, and Your Week.
        for label in ("Morning", "Afternoon", "Evening", "Night"):
            count = len(re.findall(r"(?m)^text \d+,\d+ " + label + r"$", output))
            assert count >= 3, f"Time of Day label {label} rendered only {count} times"
        assert not re.search(r"(?m)^text \d+,\d+ 1 / 1$", offline_pages), "Redundant single-page footer"
        assert {log.name:hashlib.sha256(log.read_bytes()).hexdigest()}==hashes,"App modified data"
        # All Books must page through native records without requiring a cache
        # proportional to library size.
        log.write_text("\n".join(event(i,f"Native Book {i}",f"/books/book{i}.epub",600*i) for i in range(1,8))+"\n")
        all_books=run(host,app,root,b"dddddn bb".replace(b" ",b""))
        for value in ("Books tracked: 7", "Native Book 1",
                      "Native Book 6", "Native Book 7"):
            assert value in all_books, f"Missing paginated book item {value}:\\n{all_books[-5000:]}"
        # No binary fallback, even with a valid legacy snapshot.
        legacy=folder/"reading-stats"
        legacy.mkdir()
        (legacy/"global_v2.bin").write_bytes(snapshot())
        log.unlink()
        empty=run(host,app,root,b"b")
        assert "No reading sessions yet" in empty and "12m" not in empty
        os.environ.pop("DESSEN_NATIVE_STATS",None)
        print("PASS: six offline pages with consolidated Settings; paired availability; JSONL totals; book data; period bars; calendar; reading time and average session; read-only; no BIN fallback")

if __name__=="__main__":
    main(*sys.argv[1:3])
