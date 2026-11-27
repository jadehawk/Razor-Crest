#!/usr/bin/env python3
"""App-owned history upload: offline native wire fixtures, no live traffic."""
import json
import os
import struct
import sys
import tempfile
from pathlib import Path
from datetime import date
from test_sync import fixture, run, TOKEN
from test_sync_state import overview

DEVICE = "crosspoint-a0a1a2a3a4a5"
SOURCE = bytes(range(1,17))
DOC = "aa"*16
DAY = 20782

def digest(data):
    h=14695981039346656037
    for b in data:
        h=((h^b)*1099511628211)&0xffffffffffffffff
    return h

def main(host, app):
    with tempfile.TemporaryDirectory() as td:
        root=Path(td)
        sd=root/".crosspoint"
        folder=sd/"plugins"/"reading-statistics"
        folder.mkdir(parents=True)
        (sd/"reading-statistics-token.json").write_text(json.dumps(dict(token=TOKEN)))
        log=(json.dumps(dict(doc=DOC,start=0,secs=90,tracked_secs=90,v=2,turns=7,
                             device="a0a1a2a3a4a5",title='A "quoted" book',
                             author="Author",series="Series",series_index="2.5",
                             path="/private/book.epub"))+"\n").encode()
        (sd/"reading_sessions.jsonl").write_bytes(log)
        target=len(log)
        def ack(i,docs,checkpoint=None,seconds=10800):
            (sd/f"test-upload-ack-{i}.json").write_text(json.dumps(dict(v=2,device_id=DEVICE,
                history_id=SOURCE.hex(),accepted=[dict(document=doc,checkpoint=target if checkpoint is None else checkpoint) for doc in docs],
                overview=overview(seconds,1795700200))))
        def summary():
            (sd/"test-native-summary.bin").write_bytes(struct.pack("<8I20Q",1,0,DAY,DAY,1,1,1,0,
                target,0,150,2,1,0,60,0,0,150,2,1,0,60,0,0,0,60,0,0))
            (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,1,SOURCE,target,digest(log)))
        summary()
        (sd/"test-native-turns.bin").write_bytes(struct.pack("<2I2Q",1,3,9,9))
        (sd/"test-native-days.bin").write_bytes(bytes(370*16)+struct.pack("<2Q",60,60))
        (sd/"test-native-offsets.bin").write_bytes(bytes.fromhex(DOC)+struct.pack("<I",0))
        (sd/"test-native-books.bin").write_bytes(bytes.fromhex(DOC)+bytes(84))
        net=root/"network"
        # No capability GET fixture: batch uploads must not require one.
        os.environ["DESSEN_TEST_NATIVE_STATS"]="1"
        os.environ["DESSEN_TEST_HISTORY_UPLOAD"]="1"
        cursor=folder/"history-upload-v1"
        cache=folder/"server-overview.json"
        old=overview(3600,1795700000)
        cache.write_text(json.dumps(old))
        before=cache.read_bytes()
        fixture(net,"overview",overview(7200,1795700100))
        # Two side-previous presses enter Settings; Confirm immediately
        # selects upload. Front Previous/Next traverse displayed rows.
        keys=b"uc"
        try:
            prompt=run(host,app,root,net,keys=keys,portrait=True,installed=True)
            assert "Upload this reader's history?" in prompt, prompt[-3000:]
            assert not cursor.exists() and "history-request " not in prompt
            assert cache.read_bytes()==before
            cancelled=run(host,app,root,net,keys=keys+b"b",portrait=True,installed=True)
            assert "history-request " not in cancelled and not cursor.exists() and cache.read_bytes()==before
            # Up goes to Auto-fetch when the sleep feature is absent;
            # Down goes to Refresh only, and wrapping follows visual order.
            auto=run(host,app,root,net,keys=b"upc",portrait=True,installed=True)
            assert "Enable Auto Sync?" in auto and "history-request " not in auto
            refresh=run(host,app,root,net,keys=b"unc",portrait=True,installed=True)
            assert "Sync with server?" in refresh and "history-request " not in refresh
            wrapped=run(host,app,root,net,keys=b"unnnnc",portrait=True,installed=True)
            assert "Enable Auto Sync?" in wrapped
            returned=run(host,app,root,net,keys=b"unduc",portrait=True,installed=True)
            assert "Upload this reader's history?" in returned
            # With the sleep option present, both navigation directions
            # can select it and no visible row is skipped.
            os.environ["DESSEN_TEST_SCREENSHOT"]="1"
            try:
                selected=run(host,app,root,net,keys=b"upcnc",portrait=True,installed=True)
                assert (folder/"sleep-page").read_bytes()==b"\1"
                assert "Choose Sleep Screen" in selected and "history-request " not in selected
                auto=run(host,app,root,net,keys=b"uppc",portrait=True,installed=True)
                assert "Enable Auto Sync?" in auto
                run(host,app,root,net,keys=b"upcpc",portrait=True,installed=True)
                assert (folder/"sleep-page").read_bytes()==b"\0"
            finally:
                os.environ.pop("DESSEN_TEST_SCREENSHOT",None)
            assert not cursor.exists() and cache.read_bytes()==before
            ack(0,[None,DOC],target+1)
            trace=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in trace.splitlines() if l.startswith("history-request ")]
            assert len(rows)==1 and len(rows[0]["items"])==2 and rows[0]["items"][0]["seconds"]==150,trace[-3000:]
            assert rows[0]["v"]==2 and "device_id" not in rows[0]
            assert cursor.exists() and struct.unpack_from("<I",cursor.read_bytes(),60)[0]==0
            assert cache.read_bytes()==before
            # Missing, extra or reordered acknowledgment rows cannot advance.
            for docs in ([None],[DOC,None],[None,DOC,DOC]):
                ack(0,docs)
                bad=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
                assert "History upload failed" in bad
                assert struct.unpack_from("<I",cursor.read_bytes(),60)[0]==0 and cache.read_bytes()==before
            ack(0,[None,DOC])
            trace=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in trace.splitlines() if l.startswith("history-request ")]
            assert len(rows)==1 and len(rows[0]["items"])==2,trace[-3000:]
            assert "Server Statistics" in trace.split("\nrefresh\n")[-2], trace[-1800:]
            assert "Refreshing server statistics..." not in trace
            assert rows[0]["items"][0]==dict(checkpoint=target,seconds=150,sessions=2,turns=9,
                turns_complete=True,periods=[0,60,0,0],daily=[dict(date=date.fromordinal(DAY+719163).isoformat(),seconds=60)])
            book=rows[0]["items"][1]
            assert book["document"]==DOC and book["title"]=='A "quoted" book'
            assert book["series_index"]=="2.5" and book["author"]=="Author" and "path" not in book
            assert struct.unpack_from("<Q",cursor.read_bytes(),28)[0]==target
            assert json.loads(cache.read_text())["total_seconds"]==10800
            fixture(net,"overview",overview(14400,1795700300))
            trace=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            assert "history-request " not in trace
            assert "Server Statistics" in trace.split("\nrefresh\n")[-2], trace[-1800:]
            assert json.loads(cache.read_text())["total_seconds"]==14400
            fresh=cache.read_bytes()
            saved_cursor=cursor.read_bytes()
            fixture(net,"overview",dict(error="unavailable"),503)
            failed=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            assert "Server refresh failed" in failed and "Settings" in failed.split("\nrefresh\n")[-2]
            assert cache.read_bytes()==fresh and cursor.read_bytes()==saved_cursor
            # The desktop HTTP fixture keeps a separate status sidecar.
            (net/"sync.techy-notes.com"/"api"/"device"/"overview.status").unlink()
            fixture(net,"overview",overview(18000,1795700400))
            only=run(host,app,root,net,keys=b"unccb",portrait=True,installed=True)
            assert "history-request " not in only and cursor.read_bytes()==saved_cursor
            assert "Server Statistics" in only.split("\nrefresh\n")[-2], only[-1800:]
            assert json.loads(cache.read_text())["total_seconds"]==18000
            assert (sd/"reading_sessions.jsonl").read_bytes()==log
            assert TOKEN not in trace
            # Append one session after a completed sync, without changing
            # pairing or removing the cursor. Verify the saved exact prefix.
            old_target=target
            (sd/"test-native-source-prefix.bin").write_bytes((sd/"test-native-source.bin").read_bytes())
            extra=(json.dumps(dict(doc=DOC,start=0,secs=60,tracked_secs=60,v=2,turns=3,
                                   device="a0a1a2a3a4a5",title='A "quoted" book'))+"\n").encode()
            log+=extra;target=len(log)
            (sd/"reading_sessions.jsonl").write_bytes(log)
            (sd/"test-native-summary.bin").write_bytes(struct.pack("<8I20Q",1,0,DAY,DAY,1,1,1,0,
                target,0,210,3,1,0,60,0,0,210,3,1,0,60,0,0,0,60,0,0))
            (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,1,SOURCE,target,digest(log)))
            (sd/"test-native-turns.bin").write_bytes(struct.pack("<2I2Q",1,3,12,12))
            (sd/"test-native-offsets.bin").write_bytes(bytes.fromhex(DOC)+struct.pack("<I",old_target))
            ack(0,[None,DOC],seconds=21660)
            for name,code,message in (
                ("DESSEN_TEST_HISTORY_QUERY_ERROR","-6","Not enough memory to prepare history"),
                ("DESSEN_TEST_HISTORY_QUERY_ERROR","-2","Cannot read or save history"),
                ("DESSEN_TEST_HISTORY_SOURCE_ERROR","-5","History belongs to another reader"),
                ("DESSEN_TEST_HISTORY_SOURCE_ERROR","-1","Invalid or changed reading history"),
            ):
                os.environ[name]=code
                try:
                    bad=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
                    assert message in bad,bad[-2500:]
                    assert "history-request " not in bad and cursor.read_bytes()==saved_cursor
                finally:
                    os.environ.pop(name,None)
            os.environ["DESSEN_TEST_HISTORY_HTTP_STATUS"]="401"
            try:
                revoked=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
                assert "Pairing revoked or expired" in revoked,revoked[-2500:]
                assert struct.unpack_from("<Q",cursor.read_bytes(),28)[0]==old_target
                assert struct.unpack_from("<I",cursor.read_bytes(),60)[0]==0
            finally:
                os.environ.pop("DESSEN_TEST_HISTORY_HTTP_STATUS",None)
            appended=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in appended.splitlines() if l.startswith("history-request ")]
            assert len(rows)==1 and len(rows[0]["items"])==2,appended[-2500:]
            assert all(i["seconds"]==210 and i["sessions"]==3 and i["turns"]==12 for i in rows[0]["items"])
            assert struct.unpack_from("<Q",cursor.read_bytes(),28)[0]==target
            assert json.loads(cache.read_text())["total_seconds"]==21660
            assert (sd/"reading_sessions.jsonl").read_bytes()==log
            (sd/"test-native-source-prefix.bin").unlink()
            # Six books, as on the X4 rehearsal: one request includes every
            # scope. The overview is in the receipt.
            docs=[f"{i:032x}" for i in range(1,7)]
            logs=[]; offsets=[]; pos=0
            for doc in docs:
                line=(json.dumps(dict(doc=doc,title="A title",author="An author",series="Series",series_index="1"))+"\n").encode()
                offsets.append(bytes.fromhex(doc)+struct.pack("<I",pos))
                logs.append(line);pos+=len(line)
            log=b"".join(logs); target=len(log)
            (sd/"reading_sessions.jsonl").write_bytes(log)
            (sd/"test-native-offsets.bin").write_bytes(b"".join(offsets))
            (sd/"test-native-books.bin").write_bytes(b"".join(bytes.fromhex(doc)+bytes(84) for doc in docs))
            (sd/"test-native-summary.bin").write_bytes(struct.pack("<8I20Q",1,0,DAY,DAY,1,1,6,0,
                target,0,150,2,1,0,60,0,0,150,2,1,0,60,0,0,0,60,0,0))
            (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,1,SOURCE,target,digest(log)))
            cursor.unlink()
            ack(0,[None]+docs,seconds=21600)
            batch=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in batch.splitlines() if l.startswith("history-request ")]
            assert len(rows)==1 and [i.get("document") for i in rows[0]["items"]]==[None]+docs,batch[-3000:]
            assert len(json.dumps(rows[0],separators=(",",":")).encode())<=2304
            assert "Server Statistics" in batch.split("\nrefresh\n")[-2] and "History queued" not in batch
            assert json.loads(cache.read_text())["total_seconds"]==21600
            # Rotate the pairing token without removing the checkpoint: safe
            # cumulative replay, not a silent skip from the previous token.
            (sd/"reading-statistics-token.json").write_text(json.dumps(dict(token="tnstats_"+"c"*64)))
            replay=run(host,app,root,net,keys=keys+b"cddb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in replay.splitlines() if l.startswith("history-request ")]
            assert len(rows)==1 and len(rows[0]["items"])==7,replay[-3000:]
            assert "THIS MONTH" in replay
            # >20 nonzero days require multiple requests but one confirmation.
            cursor.unlink()
            (sd/"test-native-summary.bin").write_bytes(struct.pack("<8I20Q",1,0,DAY,DAY,1,1,1,0,
                target,0,150,2,1,0,60,0,0,150,2,1,0,60,0,0,0,60,0,0))
            (sd/"test-native-days.bin").write_bytes(bytes(350*16)+struct.pack("<2Q",1,1)*21)
            ack(0,[None]);ack(1,[None,docs[0]]);ack(2,[docs[0]],seconds=25200)
            pages=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            rows=[json.loads(l[16:]) for l in pages.splitlines() if l.startswith("history-request ")]
            assert len(rows)==3,pages[-3000:]
            assert [len(r["items"]) for r in rows]==[1,2,1]
            assert [len(i["daily"]) for r in rows for i in r["items"]]==[20,1,20,1]
            assert struct.unpack_from("<Q",cursor.read_bytes(),28)[0]==target
            assert json.loads(cache.read_text())["total_seconds"]==25200
            assert "Server Statistics" in pages.split("\nrefresh\n")[-2], pages[-1800:]
            assert (sd/"reading_sessions.jsonl").read_bytes()==log
            # X4Pro reproduction: valid zero-time migrated entries, no sessions,
            # no owner marker. Refresh must not bind ownership or upload.
            cursor.unlink()
            (sd/"test-native-summary.bin").write_bytes(struct.pack("<8I20Q",1,0,DAY,0,0,0,2,0,target,0,*([0]*18)))
            (sd/"test-native-source.bin").write_bytes(struct.pack("<2I16s2Q",1,0,bytes(16),target,digest(log)))
            fixture(net,"overview",overview(30000,1795700400))
            empty=run(host,app,root,net,keys=keys+b"cb",portrait=True,installed=True)
            assert "history-request " not in empty and not cursor.exists(),empty[-2500:]
            assert "Server Statistics" in empty.split("\nrefresh\n")[-2],empty[-2500:]
            assert json.loads(cache.read_text())["total_seconds"]==30000
            assert (sd/"reading_sessions.jsonl").read_bytes()==log
        finally:
            os.environ.pop("DESSEN_TEST_NATIVE_STATS",None)
            os.environ.pop("DESSEN_TEST_HISTORY_UPLOAD",None)
    print("App-owned upload, default physical focus, visual row order, consent, acknowledgments, refresh and metadata: PASS")

if __name__=="__main__":
    main(*sys.argv[1:3])
