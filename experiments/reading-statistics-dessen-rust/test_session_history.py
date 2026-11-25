import json
import os
import tempfile
import sys
from pathlib import Path
from datetime import datetime, timezone
from test_fixture import run, snapshot

def main(host, app):
    with tempfile.TemporaryDirectory() as td:
        root=Path(td)
        base=root/".crosspoint"
        base.mkdir()
        log=base/"reading_sessions.jsonl"
        epoch=int(datetime(2026,10,1,2,0,tzinfo=timezone.utc).timestamp())
        def row(id, secs, start, title, path):
            return json.dumps(dict(doc=id*32,secs=secs,start=start,tz=-240,from_=0,to=500,title=title,path=path))
        records=[row("a",1800,epoch,"Dune","/books/dune.epub"),
                 row("a",1200,epoch-86400,"Dune","/books/dune.epub"),
                 row("b",600,epoch,"Second Book","/books/second.epub"),
                 row("c",900,0,"Undated Book","/books/undated.epub")]
        log.write_text("\n".join(records+["{bad}", "x"*800])+"\n"+row("d",999,epoch,"Incomplete","/books/incomplete.epub"))
        native=base/"reading-stats"
        native.mkdir()
        (native/"global_v2.bin").write_bytes(snapshot())
        before=log.read_bytes()
        os.environ["DESSEN_TEST_NO_TIME"]="1"
        trace=run(host,app,root,b"dddddb",portrait=True)
        assert "LAST RECORDED MONTH" in trace and "September 2026" in trace, trace[-4000:]
        assert "Books tracked: 3" in trace and "Second Book" in trace and "Undated Book" in trace and "5%" in trace
        assert "Incomplete" not in trace and "Session Start Periods" in trace
        assert "Avg. Session" in trace and "Reading Time" in trace and "234" not in trace, "stale native data leaked"
        assert "Today" not in trace and "Yesterday" not in trace
        assert "50m" in trace and "25m" in trace and "18m" in trace, "session aggregates missing"
        assert log.read_bytes()==before
        os.environ.pop("DESSEN_TEST_NO_TIME")
        os.environ["DESSEN_TEST_NO_CLOCK"]="1"
        trace=run(host,app,root,b"dddb",portrait=True)
        assert "THIS MONTH" in trace and "No dated reading sessions or device time." not in trace
        os.environ.pop("DESSEN_TEST_NO_CLOCK")
        os.environ["DESSEN_TEST_NO_TIME"]="1"
        log.write_text(records[3]+"\n")
        trace=run(host,app,root,b"dddb",portrait=True)
        assert "ALL TIME" in trace and "15m" in trace and "No dated reading sessions or device time." in trace
        os.environ.pop("DESSEN_TEST_NO_TIME")
        log.write_bytes(before)
        log.rename(base/"sessions.saved")
        trace=run(host,app,root,b"b",portrait=True)
        assert "12m" in trace and "Session Start Periods" not in trace
        (base/"sessions.saved").rename(log)
        # More than 64 books, reverse order and repeated IDs across chunks.
        rows=[]
        for i in reversed(range(70)):
            rows.append(json.dumps(dict(doc=f"{i+1:032x}",secs=600,start=epoch,tz=0,title=f"Book {i+1}",path=f"/books/{i+1}.epub")))
        rows.append(json.dumps(dict(doc=f"{1:032x}",secs=1200,start=epoch,tz=0,title="Book 1",path="/books/1.epub")))
        log.write_text("\n".join(rows)+"\n")
        trace=run(host,app,root,b"ddddnn",portrait=True,touch=False)
        assert "Books tracked: 70" in trace, trace[-3000:]
        assert "Book 1" in trace
        assert "Book 6" in trace and "Book 11" in trace and "Book 41" in trace, trace[-3000:]
    print("CrossPoint history, no RTC, undated totals, source isolation: PASS")
if __name__=="__main__":
    main(*sys.argv[1:3])