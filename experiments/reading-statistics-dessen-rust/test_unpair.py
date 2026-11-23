#!/usr/bin/env python3
"""Standalone Dessen unpair UI regression: no credentials altered in the guest."""
import json
from pathlib import Path
import tempfile
import sys
from test_sync import run, TOKEN

def main(host, app):
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        secret = root / ".crosspoint" / "reading-statistics-token.json"
        secret.parent.mkdir(parents=True)
        secret.write_text(json.dumps({"token": TOKEN}))
        cache = root / ".crosspoint" / "plugins" / "reading-statistics-dessen-rust" / "server-overview.json"
        cache.parent.mkdir(parents=True)
        cache.write_text(json.dumps({"source":"server","month_seconds":7200,"month_active_days":8,"current_streak":5,"books":13,"sessions":19,"page_turns":327,"best_day_seconds":1800,"daily_average_seconds":900}))
        secret_before, cache_before = secret.read_bytes(), cache.read_bytes()
        prompt = run(host, app, root, keys=b"up", native=True)
        assert "Disconnect this reader?" in prompt, "Previous should show confirmation: " + prompt[-3500:]
        assert "[launch stats-unpair" not in prompt, "Opening dialog must not unpair"
        cancelled = run(host, app, root, keys=b"upb", native=True)
        assert "Disconnect this reader?" in cancelled
        assert "[launch stats-unpair" not in cancelled, "Cancelled unpair must never launch"
        confirmed = run(host, app, root, keys=b"upc", native=True)
        assert "[launch stats-unpair" in confirmed, "Approved unpair must request native firmware action"
        resume = cache.parent / "resume-page"
        assert resume.read_bytes() == bytes([0]), "Unpair should return to Overall"
        assert secret.read_bytes() == secret_before, "Guest must not delete protected token"
        assert cache.read_bytes() == cache_before, "Guest must not delete cache"
        # Touch devices expose a separate Disconnect reader button above Refresh.
        for portrait in (False, True):
            prompt_touch = run(host, app, root, keys=b"uT", native=True, touch=True, portrait=portrait)
            assert "Disconnect this reader?" in prompt_touch, "Touch disconnect button not reachable: " + prompt_touch[-3700:]
            cancel_touch = run(host, app, root, keys=b"uTN", native=True, touch=True, portrait=portrait)
            assert "[launch stats-unpair" not in cancel_touch, "Touch Cancel unexpectedly reset pairing"
            confirm_touch = run(host, app, root, keys=b"uTY", native=True, touch=True, portrait=portrait)
            assert "[launch stats-unpair" in confirm_touch, "Touch confirmation did not invoke firmware-owned unpair"
            assert secret.read_bytes() == secret_before and cache.read_bytes() == cache_before
    print("Dessen firmware-owned unpair confirmation: PASS")

if __name__ == "__main__":
    main(*sys.argv[1:3])
