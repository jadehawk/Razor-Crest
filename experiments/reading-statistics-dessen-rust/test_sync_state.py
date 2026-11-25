#!/usr/bin/env python3
"""Cache freshness, explicit refresh and app-owned Web PIN regression checks."""
import json
import sys
import tempfile
import time
from pathlib import Path
from test_sync import fixture, run, TOKEN, PAIR_CODE

def overview(seconds, timestamp):
    return dict(source="server",total_seconds=seconds,month_seconds=seconds,month_active_days=8,
                current_streak=5,books=13,sessions=19,page_turns=327,best_day_seconds=1800,
                daily_average_seconds=900,synced_at=timestamp,fetched_at=timestamp)

def main(host, app):
    with tempfile.TemporaryDirectory() as td:
        root=Path(td)
        directory=root/".crosspoint"/"plugins"/"reading-statistics"
        directory.mkdir(parents=True)
        token=root/".crosspoint"/"reading-statistics-token.json"
        token.write_text(json.dumps(dict(token=TOKEN)))
        cache=directory/"server-overview.json"
        preference=directory/"stats-settings"
        now=int(time.time())
        stale=overview(3600,now-86401)
        network=root/"network"
        fixture(network,"overview",overview(7200,now))
        fixture(network,"login-code",dict(user_code=PAIR_CODE,expires_in=86400))
        cache.write_text(json.dumps(stale))
        previous=cache.read_bytes()
        # Reopen paired on the X3-shaped screen without Web Login data or input.
        import os
        os.environ["DESSEN_TEST_X3"]="1"
        try:
            for _ in range(3):
                launched=run(host,app,root,keys=b"",portrait=True,installed=True)
                assert "THIS MONTH" in launched and "ALL TIME" in launched and "refresh" in launched
            for malformed in (b"", b'{"source":"server"}', b'{"source":"server","total_seconds":42949672960}', b'{"source":"server","books":'):
                cache.write_bytes(malformed)
                launched=run(host,app,root,keys=b"",portrait=True,installed=True)
                assert "THIS MONTH" in launched and "N/A" in launched and "refresh" in launched
            cache.write_bytes(previous)
        finally:
            os.environ.pop("DESSEN_TEST_X3",None)
        run(host,app,root,network,keys=b"b",installed=True)
        assert cache.read_bytes()==previous, "Automatic refresh must start disabled"
        prompt=run(host,app,root,network,keys=b"uuc",installed=True)
        assert "Enable Auto Sync?" in prompt and "Wi-Fi" in prompt
        assert not preference.exists() and cache.read_bytes()==previous
        cancelled=run(host,app,root,network,keys=b"uucb",installed=True)
        assert not preference.exists() and cache.read_bytes()==previous
        trace=run(host,app,root,network,keys=b"uuccdd",installed=True)
        assert preference.read_bytes()==b"\1"
        assert trace.index("Enable Auto Sync?") < trace.index("Syncing...")
        assert "Syncing..." in trace
        assert json.loads(cache.read_text())["month_seconds"]==7200
        assert json.loads(cache.read_text())["fetched_at"]>=now
        fresh=cache.read_bytes()
        preference.write_bytes(b"\0")
        fixture(network,"overview",overview(9999,now))
        enabled=run(host,app,root,network,keys=b"uuccdd",installed=True)
        assert preference.read_bytes()==b"\1" and cache.read_bytes()==fresh
        assert "Enable Auto Sync?" in enabled and "Syncing..." not in enabled
        run(host,app,root,network,keys=b"b",installed=True)
        assert cache.read_bytes()==fresh, "Fresh data should not refresh automatically"
        fixture(network,"overview",overview(9999,now))
        trace=run(host,app,root,network,keys=b"udub",installed=True)
        assert cache.read_bytes()==fresh and "Syncing..." not in trace
        assert trace.count("THIS MONTH")==2, "Back on Server Sync must exit directly"
        cache.write_text(json.dumps(stale))
        previous=cache.read_bytes()
        failed=run(host,app,root,keys=b"b",installed=True)
        assert failed.count("Syncing...")==1, "Failed refresh must not restart the workflow"
        assert cache.read_bytes()==previous and token.exists()
        fixture(network,"overview",dict(source="server",total_seconds=0))
        run(host,app,root,network,keys=b"b",installed=True)
        assert cache.read_bytes()==previous, "Malformed success must not overwrite valid cache"
        preference.write_bytes(b"\0")
        fixture(network,"overview",overview(10800,now))
        trace=run(host,app,root,network,keys=b"udub",installed=True)
        assert cache.read_bytes()==previous and "Syncing..." not in trace
        prompt=run(host,app,root,network,keys=b"uunc",installed=True)
        assert "Sync with server?" in prompt and cache.read_bytes()==previous
        run(host,app,root,network,keys=b"uuncb",installed=True)
        assert cache.read_bytes()==previous
        confirmed=run(host,app,root,network,keys=b"uuncc",installed=True)
        assert confirmed.index("Sync with server?") < confirmed.index("Syncing...")
        assert json.loads(cache.read_text())["month_seconds"]==10800
        prompt=run(host,app,root,network,keys=b"uc",installed=True)
        assert "Generate a web login?" in prompt
        assert not (directory/"web-login.json").exists()
        confirmed=run(host,app,root,network,keys=b"ucc",installed=True)
        assert "Generating web login..." in confirmed and PAIR_CODE in confirmed
        saved=json.loads((directory/"web-login.json").read_text())
        assert saved["user_code"]==PAIR_CODE and saved["expires_at"]>now
        offline=run(host,app,root,keys=b"u",installed=True)
        assert PAIR_CODE in offline, "Web PIN must survive application restart"
        assert TOKEN not in offline and json.loads(token.read_text())["token"]==TOKEN
    print("Generic refresh, failure cleanup, account cache and Web PIN persistence: PASS")

if __name__=="__main__":
    main(*sys.argv[1:3])
