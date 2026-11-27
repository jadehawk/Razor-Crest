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
  int bookCover(const char* path, uint32_t height, uint8_t* out, uint32_t cap) override {
    if (std::string_view(path) != "/books/dune.epub" || height < 24 || cap < 52) return 0;
    out[0]=16; out[1]=0; out[2]=24; out[3]=0;
    for (unsigned i=4;i<52;++i) out[i]=0x55;
    return 52;
  }
""")
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
        assert "35m" in trace and "20m" in trace, trace[-8000:]
        assert "Dungeon Crawler Carl Book 1" in trace and "#1 - Dungeon Crawler Carl" in trace
        assert "Standalone Book" in trace and "Time: 03h 12m" in trace and "Time: 01h 00m" in trace
        assert "37%" in trace and "22%" in trace and "bitmap" in trace
        # Read both views offline after reopening. No network fixture is supplied.
        trace=run(str(host),app,root,keys=b"ddddcb",portrait=True,installed=True)
        assert "35m" in trace and "20m" in trace
        # Physical front keys browse calendar months; side keys navigate pages.
        trace=run(str(host),app,root,keys=b"ddddpnb",portrait=True,installed=True)
        last_month=date.today().replace(day=1)-timedelta(days=1)
        assert last_month.strftime("%B %Y") in trace and "Prev Month" in trace and "Next Month" in trace
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
