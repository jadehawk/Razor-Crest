#!/usr/bin/env python3
"""Exercise new wire contracts against the desktop VM without changing the SDK."""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from datetime import date, timedelta
from pathlib import Path

from test_sync import fixture, run, TOKEN
from test_sync_state import overview
from test_history_upload import digest, SOURCE

def build_host(sdk, out):
    source = (sdk / "host/main.cpp").read_text()
    marker = "    if (kind == 0) return 1;"
    assert source.count(marker) == 1
    source = source.replace(marker, marker + """
    if (kind == 9 && first == 0 && cap == 0) return 1;
    if (kind == 9 && ready && cap == 364)
      return fsReadAt("/.crosspoint/test-native-rich-books.bin", first * 364, out, cap);
""")
    marker = "  using DesktopServices::DesktopServices;"
    assert source.count(marker) == 1
    source = source.replace(marker, marker + """
  int validateBookOpen(const char* path) override { return std::string_view(path)=="/books/removed.epub"?DSN_ERR_UNAVAILABLE:0; }
  int bookCover(const char* path, uint32_t height, uint8_t* out, uint32_t cap) override {
    if (std::string_view(path) != "/books/dune.epub" || height < 24) return 0;
    const unsigned width=height*2/3, bytes=4+((width+7)/8)*height;
    if (cap<bytes) return 0;
    out[0]=width; out[1]=width>>8; out[2]=height; out[3]=height>>8;
    for (unsigned i=4;i<bytes;++i) out[i]=0x55;
    return bytes;
  }
""")
    source = source.replace("input.touchY = (c == 'L' || c == 'R') ? 380 : 150;",
                            "input.touchY = (c == 'L' || c == 'R') ? screen.height-30 : 150;")
    source = source.replace("c == 'm' ? 85", "c == 'm' ? 13+2*target.lineHeight(4)+target.lineHeight(5)+5+target.lineHeight(4)/2")
    src = out.parent / "fixture.cpp"
    src.write_text(source)
    subprocess.run(["c++","-std=c++17","-O2", "-I"+str(sdk/"vm"),
                    "-I"+str(sdk/"sdk"), "-I"+str(sdk/"freeink"), "-I"+str(sdk/"host"),
                    "-I"+str(sdk.parent/"freeink-sdk/libs/ui/FreeInkUI/include"),
                    str(src),str(sdk/"freeink/DessenFreeInk.cpp"),
                    str(sdk/"host/DesktopServices.cpp"),str(sdk/"build/dessen_vm.o"),
                    "-lcrypto","-o",str(out)],check=True)

def record(title,path,series,seconds,progress):
    title=title.encode()
    return (struct.pack("<QHBB",seconds,progress,len(title),0) +
            title.ljust(80,b"\0") + path.encode().ljust(176,b"\0") +
            series.encode().ljust(96,b"\0"))

def main(sdk,app):
    with tempfile.TemporaryDirectory() as td:
        root=Path(td)
        host=root/"host"
        build_host(Path(sdk).resolve(),host)
        sd=root/".crosspoint"
        folder=sd/"plugins/reading-statistics"
        folder.mkdir(parents=True)
        (sd/"reading-statistics-token.json").write_text(json.dumps(dict(token=TOKEN)))
        (folder/"stats-settings").write_bytes(b"\1")
        today=(date.today()-date(1970,1,1)).days
        raw=b'{"doc":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","secs":1200}\n'
        (sd/"reading_sessions.jsonl").write_bytes(raw)
        summary=struct.pack("<8I20Q",1,0,today,today,1,1,2,0,
                            len(raw),0,1200,1,1,0,0,0,1200,1200,1,1,0,0,0,1200,0,0,0,1200)
        (sd/"test-native-summary.bin").write_bytes(summary)
        (sd/"test-native-days.bin").write_bytes(bytes(370*16)+struct.pack("<2Q",1200,1200))
        (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,1,SOURCE,len(raw),digest(raw)))
        (sd/"test-native-rich-books.bin").write_bytes(
            record("Dungeon Crawler Carl Book 1","/books/dune.epub","#1 - Dungeon Crawler Carl",11520,2200)+
            record("Standalone Book","/books/removed.epub","",3600,2200))
        offsets=sd/"test-native-offsets.bin"
        offsets.write_bytes(bytes.fromhex("aa"*16)+struct.pack("<I",0)+bytes.fromhex("bb"*16)+struct.pack("<I",30))
        os.environ["DESSEN_TEST_NATIVE_STATS"]="1"
        os.environ["DESSEN_TEST_HISTORY_UPLOAD"]="1"
        net=root/"network"
        fixture(net,"overview",{**overview(7200,int(time.time())),"calendar_version":1})
        path=net/"sync.techy-notes.com/api/device/calendar"
        payload=b"RCA1"+struct.pack("<II",today,1)+SOURCE+bytes([0x84,0x03,0,0x58,0x02,0])+bytes(370*6)
        assert len(payload)==2254
        path.write_bytes(payload)
        trace=run(str(host),app,root,net,keys=b"ddddcdb",portrait=True,installed=True)
        assert (folder/"server-calendar.bin").read_bytes()[:2254]==payload
        assert "All Devices" in trace and "This Device" in trace
        assert all("Reading Calendar" in f for f in trace.split("refresh") if "Reading Consistency" in f)
        assert "35m" in trace and "20m" in trace, trace[-8000:]
        assert "Dungeon Crawler Carl Book 1" in trace and "#1 - Dungeon Crawler Carl" in trace
        assert "Standalone Book" in trace and "Time: 03h 12m" in trace and "Time: 01h 00m" in trace
        assert "37%" in trace and "22%" in trace and "bitmap" in trace
        for keys,expected,launch in (
            (b"dddddc","Delete from List",False),
            (b"dddddcb","Hide Book",False),
            (b"dddddccc","Open Book?",True),
            (b"dddddnccc","Book unavailable",False)):
            opened=run(str(host),app,root,keys=keys,portrait=True,installed=True)
            assert expected in opened,opened[-3000:]
            assert ("launch reader /books/dune.epub" in opened)==launch,opened[-3000:]
        def frame(trace):
            return trace.split("refresh")[-2]
        # Temporary deletion survives restart, then reappears on a new session.
        run(str(host),app,root,keys=b"dddddcncb",portrait=True,installed=True)
        listed=frame(run(str(host),app,root,keys=b"dddddb",portrait=True,installed=True))
        assert "Standalone Book" in listed and "Dungeon Crawler Carl Book 1" not in listed,listed
        assert (folder/"book-list.bin").read_bytes()[22]==1
        offsets.write_bytes(bytes.fromhex("aa"*16)+struct.pack("<I",8)+bytes.fromhex("bb"*16)+struct.pack("<I",30))
        listed=frame(run(str(host),app,root,keys=b"dddddb",portrait=True,installed=True))
        assert "Dungeon Crawler Carl Book 1" in listed and "Standalone Book" in listed,listed
        # Persistent hide survives changed session offset. Restore via Settings.
        run(str(host),app,root,keys=b"dddddcnncb",portrait=True,installed=True)
        offsets.write_bytes(bytes.fromhex("aa"*16)+struct.pack("<I",12)+bytes.fromhex("bb"*16)+struct.pack("<I",30))
        listed=frame(run(str(host),app,root,keys=b"dddddb",portrait=True,installed=True))
        assert "Dungeon Crawler Carl Book 1" not in listed and "Standalone Book" in listed,listed
        assert (folder/"book-list.bin").read_bytes()[22]==2
        hidden=run(str(host),app,root,keys=b"unnnnc",portrait=True,installed=True)
        assert "Hidden Books" in hidden and "Dungeon Crawler Carl Book 1" in frame(hidden),hidden[-2000:]
        restored=run(str(host),app,root,keys=b"unnnncccb",portrait=True,installed=True)
        assert "Restore Book" in restored and (folder/"book-list.bin").read_bytes()==b"",restored[-2000:]
        assert "Dungeon Crawler Carl Book 1" in frame(run(str(host),app,root,keys=b"dddddb",portrait=True,installed=True))
        # Read both views offline after reopening. No network fixture is supplied.
        trace=run(str(host),app,root,keys=b"ddddcb",portrait=True,installed=True)
        assert "35m" in trace and "20m" in trace
        # Physical front keys browse calendar months; side keys navigate pages.
        trace=run(str(host),app,root,keys=b"ddddpnb",portrait=True,installed=True)
        last_month=date.today().replace(day=1)-timedelta(days=1)
        assert last_month.strftime("%B %Y") in trace and "Prev Month" in trace and "Next Month" in trace
        # Exercise a six-row month on both X3-like metrics and portrait touch.
        month=date.today().replace(day=1)
        for months_back in range(12):
            next_month=(month.replace(day=28)+timedelta(days=4)).replace(day=1)
            if month.weekday()+(next_month-month).days>35:
                break
            month=(month-timedelta(days=1)).replace(day=1)
        else:
            raise AssertionError("No six-row month in previous year")
        for x3 in (False,True):
            if x3: os.environ["DESSEN_TEST_X3"]="1"
            try:
                trace=run(str(host),app,root,keys=b"dddd"+b"p"*months_back+b"b",
                          portrait=True,installed=True)
                frame=trace.split("refresh")[-2]
                assert month.strftime("%B %Y") in frame
                day=(next_month-month).days
                # The last date cell's outline must end before the legend.
                match=re.search(r"stroke (\d+),(\d+) (\d+)x(\d+) w[12]\ntext \d+,\d+ "+str(day)+r"\n",frame)
                legend=re.search(r"text \d+,(\d+) None\n",frame)
                assert match and legend and int(match[2])+int(match[4])+5<=int(legend[1]),frame
                trace=run(str(host),app,root,keys=b"dddddb",portrait=True,installed=True)
                frame=trace.split("refresh")[-2]
                assert "Books • Local" in frame and "Standalone Book" in frame
                cover=re.search(r"bitmap (\d+),(\d+) (\d+)x(\d+) src",frame)
                assert cover and int(cover[4])>=76,frame
                assert int(cover[3])*3<=int(cover[4])*2+2
            finally:
                os.environ.pop("DESSEN_TEST_X3",None)
        # Inside the calendar: right goes older, left goes newer, and a
        # further left swipe at the current month advances to All Books.
        touch=run(str(host),app,root,keys=b"ddddrlmlb",touch=True,portrait=True,installed=True)
        months=[f for f in touch.split("refresh") if "Reading Consistency" in f]
        assert len(months)==4, touch[-8000:]
        for frame_text, expected in zip(months, (date.today(),last_month,date.today(),date.today())):
            assert expected.strftime("%B %Y") in frame_text, frame_text
        assert "Books" in touch.split("refresh")[-2], touch[-5000:]
        # A left swipe at the current month advances immediately, including
        # after changing the calendar scope; outside swipes still page normally.
        for keys,expected in ((b"ddddlb","Books"),(b"ddddLb","Books"),
                              (b"ddddRb","Your Week"),(b"ddddpnb",date.today().strftime("%B %Y"))):
            result=run(str(host),app,root,keys=keys,touch=True,portrait=True,installed=True)
            assert expected in result.split("refresh")[-2], result[-5000:]
        oldest=date.today().replace(day=1)
        for _ in range(11):
            oldest=(oldest-timedelta(days=1)).replace(day=1)
        result=run(str(host),app,root,keys=b"dddd"+b"r"*12+b"b",touch=True,portrait=True,installed=True)
        assert oldest.strftime("%B %Y") in result.split("refresh")[-2], result[-5000:]
        assert "< Prev" not in touch and "Next >" not in touch
        assert "This Device" in touch and "Books • Local" in touch,touch[-5000:]
        # A reader with no RTC and no dated sessions still has a server calendar.
        undated=bytearray(summary)
        struct.pack_into("<II",undated,8,0,0)
        (sd/"test-native-summary.bin").write_bytes(undated)
        os.environ["DESSEN_TEST_NO_TIME"]="1"
        try:
            trace=run(str(host),app,root,keys=b"ddddcb",portrait=True,installed=True)
            assert date.today().strftime("%B %Y") in trace and "25m" in trace
            assert "No dated reading sessions or device time." in trace
        finally:
            os.environ.pop("DESSEN_TEST_NO_TIME",None)
            (sd/"test-native-summary.bin").write_bytes(summary)
        # A different history lifetime is not overlaid on server totals.
        (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,1,bytes([99])*16,len(raw),digest(raw)))
        trace=run(str(host),app,root,keys=b"ddddcb",portrait=True,installed=True)
        assert "All Devices (server only)" in trace and "25m" in trace and "20m" in trace and "35m" not in trace
        # Token rotation cannot reuse another account's cached calendar.
        (sd/"reading-statistics-token.json").write_text(json.dumps(dict(token=TOKEN[:8]+"c"*64)))
        trace=run(str(host),app,root,keys=b"ddddb",portrait=True,installed=True)
        assert "This Device (not synced)" in trace and "35m" not in trace
        print("PASS: rich books, cached cover, live/fallback progress, offline calendar, deduplication and account isolation")

if __name__=="__main__":
    main(*sys.argv[1:3])
