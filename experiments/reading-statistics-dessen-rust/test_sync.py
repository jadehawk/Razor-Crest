#!/usr/bin/env python3
"""Offline device-code pairing integration regression for the Rust Dessen app.

Uses desktop HTTPS fixtures; never contacts the production server.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

CODE = "A" * 64
TOKEN = "tnstats_" + "b" * 64
PAIR_CODE = "XPUG-OWXE"


def fixture(root, endpoint, payload, status=200):
    location = root / "sync.techy-notes.com" / "api" / "device" / endpoint
    location.parent.mkdir(parents=True, exist_ok=True)
    location.write_text(json.dumps(payload), encoding="utf-8")
    if status != 200:
        location.with_name(location.name + ".status").write_text(str(status), encoding="ascii")


def run(host, app, filesystem, network=None, keys=b"ucccbb", touch=False, portrait=False, installed=False):
    env = os.environ.copy()
    env["DESSEN_FS_ROOT"] = str(filesystem)
    if installed:
        env["DESSEN_APP_DIR"] = "/.crosspoint/plugins/reading-statistics"
    else:
        env.pop("DESSEN_APP_DIR", None)
    if touch:
        env["DESSEN_TEST_TOUCH"] = "1"
    else:
        env.pop("DESSEN_TEST_TOUCH", None)
        env["DESSEN_TEST_HINTS"] = "1"
    if portrait:
        env["DESSEN_TEST_PORTRAIT"] = "1"
    else:
        env.pop("DESSEN_TEST_PORTRAIT", None)
    if network:
        env["DESSEN_NET_ROOT"] = str(network)
    try:
        p = subprocess.run([host, app], env=env, input=keys, capture_output=True, timeout=25)
    except subprocess.TimeoutExpired as e:
        raise AssertionError((keys, e.stdout[-1800:] if e.stdout else b'', e.stderr[-500:] if e.stderr else b'')) from e
    assert p.returncode == 0, (p.returncode, p.stdout[-600:], p.stderr[-600:])
    return p.stdout.decode(errors='replace')


def main(host, app):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        network = root / "network"
        sd = root / "sd"
        sd.mkdir()
        (sd / 'app').mkdir()
        (sd / '.crosspoint').mkdir()
        fixture(network, "code", {"device_code": CODE, "user_code": PAIR_CODE,
                                  "verification_uri": "https://sync.techy-notes.com/link?code=" + PAIR_CODE,
                                  "interval": 5, "expires_in": 600})
        fixture(network, "token", {"access_token": TOKEN, "token_type": "Bearer"})
        fixture(network, "login-code", {"user_code": PAIR_CODE, "expires_in": 86400, "expires_at": int(time.time()) + 86400,
                   "verification_uri": "https://sync.techy-notes.com/signin?code=" + PAIR_CODE + "#reader-login"})
        fixture(network, "overview", {
            "source": "server", "total_seconds": 4730460,
            "month_seconds": 7200, "month_active_days": 8,
            "current_streak": 5, "books": 13, "sessions": 19,
            "page_turns": 327, "best_day_seconds": 1800,
            "daily_average_seconds": 900, "today_seconds": 120
        })
        target = sd / ".crosspoint" / "reading-statistics-token.json"
        prompt = run(host, app, sd, network, keys=b"uc")
        assert "Generate a pairing code?" in prompt and "The reader will connect to Wi-Fi." in prompt
        assert "Generating pairing code..." not in prompt and not target.exists()
        cancelled = run(host, app, sd, network, keys=b"ucb")
        assert "Generating pairing code..." not in cancelled and not target.exists()
        trace = run(host, app, sd, network)
        assert trace.index("Generate a pairing code?") < trace.index("Generating pairing code...")
        assert "Syncing..." in trace
        assert json.loads((sd / "app" / "server-overview.json").read_text())["month_seconds"] == 7200
        assert target.exists(), ("Completed pairing did not persist shared credential", trace[-3500:])
        assert json.loads(target.read_text())["token"] == TOKEN, "Saved token mismatch"
        # A capability file stands in for the firmware's virtual capture endpoint.
        capture = sd / "app" / "sleep-frame.capture"
        capture.write_bytes(b"\x01")
        original_wallpaper = sd / "sleep.bmp"
        original_wallpaper.write_bytes(b"user wallpaper")
        preference = sd / "app" / "stats-on-sleep"
        run(host, app, sd, keys=b"")
        assert not preference.exists(), "Sleep stats must default to off"
        enabled = run(host, app, sd, keys=b"uunc")
        assert "Stats on Sleep" in enabled and preference.read_bytes() == b"\x01"
        assert capture.read_bytes() == b"\x01", "Paired capture must publish the server page"
        run(host, app, sd, keys=b"uuppc")
        assert preference.read_bytes() == b"\x00"
        assert original_wallpaper.read_bytes() == b"user wallpaper"
        preference.write_bytes(b"\x01")
        target.unlink()
        run(host, app, sd, keys=b"")
        assert capture.read_bytes() == b"\x00", "Unpaired capture must publish the device page"
        capture.unlink()
        target.write_text(json.dumps({"token": TOKEN}))
        # Firmware injects event 11 every five seconds while waiting; the app
        # must poll approval without another press of Confirm/touch.
        target.unlink()
        timer_trace = run(host, app, sd, network, keys=b"ucczbb")
        assert "Reader paired successfully" in timer_trace, "Timer did not complete the pairing flow"
        assert "Checking pairing..." not in timer_trace, "Background polling should not flash a progress frame"
        assert json.loads(target.read_text())["token"] == TOKEN, "Timed poll did not save token"
        # Picco touch UI must draw a real button, not a hardware-key instruction.
        target.unlink()
        touch_trace = run(host, app, sd, network, keys=b"utYtbb", touch=True)
        assert "Generate Code" in touch_trace, "Touch page missing Generate Code button: " + touch_trace[-2400:]
        assert "Check Pairing" in touch_trace, "Touch tap did not generate pairing code"
        assert "Generating pairing code..." in touch_trace, "Touch button did not visibly acknowledge tap"
        assert "Open: Generate pairing code" not in touch_trace, "Hardware-key label leaked to touch UI"
        assert json.loads(target.read_text())["token"] == TOKEN
        target.unlink()
        portrait_trace = run(host, app, sd, network, keys=b"utYtbb", touch=True, portrait=True)
        assert "Check Pairing" in portrait_trace, "Portrait touch button did not activate"
        assert json.loads(target.read_text())["token"] == TOKEN

        # Allow a card-level tap to compensate for touch/safe-area offsets.
        target.unlink()
        card_trace = run(host, app, sd, network, keys=b"uxYtbb", touch=True)
        assert "Check Pairing" in card_trace, "Card-level touch tap was ignored"
        assert json.loads(target.read_text())["token"] == TOKEN
        paired_trace = run(host, app, sd, keys=b"ub", touch=True)
        assert "Generate Code" not in paired_trace and "Check Pairing" not in paired_trace, "Paired device still offers registration"
        assert "Open Web Stats" in paired_trace, "Paired device lacks web sign-in action"
        web_trace = run(host, app, sd, network, keys=b"uccbb", touch=False)
        assert "New Web PIN" in web_trace, "Button-reader cannot refresh login PIN"
        assert PAIR_CODE in web_trace, "Login PIN missing after authenticated request"
        assert "sync.techy-notes.com/signin" in web_trace
        assert json.loads(target.read_text())["token"] == TOKEN, "Web login must never replace pairing credential"
        portrait_web = run(host, app, sd, network, keys=b"uccbb", touch=True, portrait=True)
        assert PAIR_CODE in portrait_web and "New Web PIN" in portrait_web
        assert "THIS MONTH" in portrait_web, "Paired Overall must use remote stats"
        assert "TAP TO REFRESH" not in portrait_web, "Clipped refresh instruction still present"
        # Refresh is an e-ink vector sync icon, not a text label.
        assert "THIS MONTH" in portrait_web and portrait_web.count("line ") >= 12, "Refresh icon missing"
        assert "13" in portrait_web and "69h 9m" in portrait_web and "Avg. Session" in portrait_web, "Remote statistics not displayed"
        revisit = run(host, app, sd, network, keys=b"ubb", touch=False)
        assert revisit.count("THIS MONTH") == 1, "Back from Server Sync must exit, not reopen Overall"
        offline_web = run(host, app, sd, None, keys=b"b", touch=True)
        assert "THIS MONTH" in offline_web and "FALLBACK" not in offline_web, "Offline UI exposed fallback label"
        assert json.loads(target.read_text())["token"] == TOKEN

        # A previously paired reader must not restart registration, even offline.
        run(host, app, sd, None, keys=b"uccbb")
        assert json.loads(target.read_text())["token"] == TOKEN

        target.unlink()
        fixture(network, "token", {"error": "authorization_pending"}, status=400)
        run(host, app, sd, network)
        assert not target.exists(), "Pending authorization must never save a token"

        fixture(network, "token", {"error": "expired_token"}, status=400)
        run(host, app, sd, network)
        assert not target.exists(), "Expired authorization must never save a token"

        fixture(network, "token", {"access_token": "invalid"}, status=200)
        run(host, app, sd, network)
        assert not target.exists(), "Invalid token must never be persisted"

        run(host, app, sd, None)
        assert not target.exists(), "Offline registration should not create credentials"

    print("Dessen device pairing and automatic refresh fixtures: PASS")


if __name__ == "__main__":
    main(*sys.argv[1:3])
