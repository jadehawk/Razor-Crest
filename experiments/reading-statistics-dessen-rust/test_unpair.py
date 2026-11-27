#!/usr/bin/env python3
"""Confirm, cancel and delete pairing through generic Dessen services."""
import json
from pathlib import Path
import tempfile
import sys
from test_sync import run, TOKEN

def main(host, app):
    for touch, portrait in ((False, False), (True, False), (True, True)):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            secret = root / ".crosspoint" / "reading-statistics-token.json"
            secret.parent.mkdir()
            secret.write_text(json.dumps({"token": TOKEN}))
            app_dir = root / "app"
            app_dir.mkdir()
            caches = [app_dir / name for name in ("server-overview.json", "web-login.json",
                      "server-overview.json.bak", "web-login.json.part",
                      "history-upload-v1", "history-upload-v1.bak", "history-upload-v1.part")]
            for cache in caches:
                cache.write_text("{}")
            local = secret.parent / "reading-stats"
            local.mkdir()
            stats = local / "global_v2.bin"
            stats.write_bytes(b"local reading statistics")
            history=secret.parent/"reading_sessions.jsonl"
            history.write_bytes(b"local history")
            prefix = b"uT" if touch else b"upc"
            prompt = run(host, app, root, keys=prefix, touch=touch, portrait=portrait)
            assert "Disconnect this reader?" in prompt
            assert secret.exists() and all(c.exists() for c in caches)
            run(host, app, root, keys=prefix + (b"N" if touch else b"b"), touch=touch, portrait=portrait)
            assert secret.exists() and all(c.exists() for c in caches)
            confirmed = run(host, app, root, keys=prefix + (b"Y" if touch else b"c"), touch=touch, portrait=portrait)
            assert "Disconnecting reader..." in confirmed and "Please wait." in confirmed
            assert confirmed.index("Disconnecting reader...") < confirmed.index("NOT PAIRED")
            assert not secret.exists() and not any(c.exists() for c in caches)
            assert stats.read_bytes() == b"local reading statistics"
            assert history.read_bytes()==b"local history"
            restarted = run(host, app, root, keys=b"u", touch=touch, portrait=portrait)
            assert "READER PAIRED" not in restarted
    print("Generic unpair confirmation, cancellation and offline local cleanup: PASS")

if __name__ == "__main__":
    main(*sys.argv[1:3])
