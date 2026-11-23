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

CODE = "A" * 64
TOKEN = "tnstats_" + "b" * 64
PAIR_CODE = "XPUG-OWXE"


def fixture(root, endpoint, payload, status=200):
    location = root / "sync.techy-notes.com" / "api" / "device" / endpoint
    location.parent.mkdir(parents=True, exist_ok=True)
    location.write_text(json.dumps(payload), encoding="utf-8")
    if status != 200:
        location.with_name(location.name + ".status").write_text(str(status), encoding="ascii")


def run(host, app, filesystem, network=None, keys=b"uccbb", touch=False, portrait=False, native=False):
    env = os.environ.copy()
    env["DESSEN_FS_ROOT"] = str(filesystem)
    if native:
        env["DESSEN_NATIVE_STATS"] = "1"
        env["DESSEN_APP_DIR"] = "/.crosspoint/plugins/reading-statistics-dessen-rust"
    else:
        env.pop("DESSEN_NATIVE_STATS", None)
        env.pop("DESSEN_APP_DIR", None)
    if touch:
        env["DESSEN_TEST_TOUCH"] = "1"
    else:
        env.pop("DESSEN_TEST_TOUCH", None)
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
        (sd / '.crosspoint').mkdir()
        fixture(network, "code", {"device_code": CODE, "user_code": PAIR_CODE,
                                  "verification_uri": "https://sync.techy-notes.com/link?code=" + PAIR_CODE,
                                  "interval": 5, "expires_in": 600})
        fixture(network, "token", {"access_token": TOKEN, "token_type": "Bearer"})
        fixture(network, "login-code", {"user_code": PAIR_CODE, "expires_in": 600,
                   "verification_uri": "https://sync.techy-notes.com/signin?code=" + PAIR_CODE + "#reader-login"})
        fixture(network, "overview", {
            "source": "server", "total_seconds": 12345,
            "month_seconds": 7200, "month_active_days": 8,
            "current_streak": 5, "books": 13, "sessions": 19,
            "page_turns": 327, "best_day_seconds": 1800,
            "daily_average_seconds": 900, "today_seconds": 120
        })
        trace = run(host, app, sd, network)
        target = sd / ".crosspoint" / "reading-statistics-token.json"
        assert target.exists(), ("Completed pairing did not persist shared credential", trace[-3500:])
        assert json.loads(target.read_text())["token"] == TOKEN, "Saved token mismatch"
        # Firmware injects event 11 every five seconds while waiting; the app
        # must poll approval without another press of Confirm/touch.
        target.unlink()
        timer_trace = run(host, app, sd, network, keys=b"uczbb")
        assert "Reader paired successfully" in timer_trace, "Timer did not complete the pairing flow"
        assert "Checking pairing..." not in timer_trace, "Background polling should not flash a progress frame"
        assert json.loads(target.read_text())["token"] == TOKEN, "Timed poll did not save token"
        # Picco touch UI must draw a real button, not a hardware-key instruction.
        target.unlink()
        touch_trace = run(host, app, sd, network, keys=b"uttbb", touch=True)
        assert "Generate Code" in touch_trace, "Touch page missing Generate Code button: " + touch_trace[-2400:]
        assert "Check Pairing" in touch_trace, "Touch tap did not generate pairing code"
        assert "Requesting pairing code..." in touch_trace, "Touch button did not visibly acknowledge tap"
        assert "Open: Generate pairing code" not in touch_trace, "Hardware-key label leaked to touch UI"
        assert json.loads(target.read_text())["token"] == TOKEN
        target.unlink()
        portrait_trace = run(host, app, sd, network, keys=b"uttbb", touch=True, portrait=True)
        assert "Check Pairing" in portrait_trace, "Portrait touch button did not activate"
        assert json.loads(target.read_text())["token"] == TOKEN

        # Allow a card-level tap to compensate for touch/safe-area offsets.
        target.unlink()
        card_trace = run(host, app, sd, network, keys=b"uxtbb", touch=True)
        assert "Check Pairing" in card_trace, "Card-level touch tap was ignored"
        assert json.loads(target.read_text())["token"] == TOKEN
        paired_trace = run(host, app, sd, keys=b"ub", touch=True)
        assert "Generate Code" not in paired_trace and "Check Pairing" not in paired_trace, "Paired device still offers registration"
        assert "Open Web Stats" in paired_trace, "Paired device lacks web sign-in action"
        web_trace = run(host, app, sd, network, keys=b"ucbb", touch=False)
        assert "New web PIN" in web_trace, "Button-reader cannot refresh login PIN"
        assert PAIR_CODE in web_trace, "Login PIN missing after authenticated request"
        assert "sync.techy-notes.com/signin" in web_trace
        assert json.loads(target.read_text())["token"] == TOKEN, "Web login must never replace pairing credential"
        portrait_web = run(host, app, sd, network, keys=b"ucbb", touch=True, portrait=True)
        assert PAIR_CODE in portrait_web and "New Web PIN" in portrait_web
        assert "THIS MONTH - SERVER" in portrait_web, "Paired Overall must use remote stats"
        assert "TAP TO REFRESH" not in portrait_web, "Clipped refresh instruction still present"
        # Refresh is an e-ink vector sync icon, not a text label.
        assert "THIS MONTH - SERVER" in portrait_web and portrait_web.count("line ") >= 12, "Refresh icon missing"
        assert "13" in portrait_web and "327" in portrait_web, "Remote statistics not displayed"
        revisit = run(host, app, sd, network, keys=b"ubb", touch=False)
        assert revisit.count("THIS MONTH - SERVER") >= 2, "Returning from Server Sync did not retry Overall"
        offline_web = run(host, app, sd, None, keys=b"b", touch=True)
        assert "THIS MONTH" in offline_web and "FALLBACK" not in offline_web, "Offline UI exposed fallback label"
        assert json.loads(target.read_text())["token"] == TOKEN

        # A previously paired reader must not restart registration, even offline.
        run(host, app, sd, None, keys=b"ucbb")
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

        # X3: guest uses only SD cache; no Wi-Fi syscall is issued while it
        # runs. Native actions unload the guest before the firmware network UI.
        x3 = root / "x3"
        cache_dir = x3 / ".crosspoint" / "plugins" / "reading-statistics-dessen-rust"
        cache_dir.mkdir(parents=True)
        assert not (x3 / ".crosspoint" / "plugins" / "reading-statistics").exists(), "Legacy plugin should be absent"
        unpaired = run(host, app, x3, keys=b"uc", native=True)
        assert "[launch stats-pair" in unpaired, "X3 unpaired action did not hand off to native registration"
        resume = x3 / ".crosspoint" / "plugins" / "reading-statistics-dessen-rust" / "resume-page"
        assert resume.read_bytes() == b"\x00", "Pairing must return to Overall, not the pairing page"
        resume.unlink()
        x3_token = x3 / ".crosspoint" / "reading-statistics-token.json"
        x3_token.write_text(json.dumps({"token": TOKEN}))
        cache_dir.joinpath("server-overview.json").write_text(json.dumps({
            "source": "server", "synced_at": 1763884800, "total_seconds": 12345,
            "month_seconds": 7200, "month_active_days": 8, "current_streak": 5,
            "books": 13, "sessions": 19, "page_turns": 327,
            "best_day_seconds": 1800, "daily_average_seconds": 900
        }))
        offline_x3 = run(host, app, x3, keys=b"b", native=True)
        assert "THIS MONTH - SERVER" in offline_x3 and "327" in offline_x3
        assert "FALLBACK" not in offline_x3
        # Reset pairing is an explicit two-step operation; the guest must
        # never delete protected credentials or server caches itself.
        before_token = x3_token.read_bytes()
        before_cache = cache_dir.joinpath("server-overview.json").read_bytes()
        ask_disconnect = run(host, app, x3, keys=b"up", native=True)
        assert "Disconnect this reader?" in ask_disconnect, "X3 Previous did not show disconnect confirmation"
        assert "[launch stats-unpair" not in ask_disconnect, "Previous alone unpaired without approval"
        cancel_disconnect = run(host, app, x3, keys=b"upb", native=True)
        assert "Disconnect this reader?" in cancel_disconnect and "[launch stats-unpair" not in cancel_disconnect, (
            "Cancel unexpectedly requested unpair"
        )
        assert x3_token.read_bytes() == before_token and cache_dir.joinpath("server-overview.json").read_bytes() == before_cache
        confirm_disconnect = run(host, app, x3, keys=b"upc", native=True)
        assert "[launch stats-unpair" in confirm_disconnect, "Confirm did not request firmware-owned unpair"
        assert resume.read_bytes() == b"\x00", "Unpair must return to Overall"
        resume.unlink()
        assert x3_token.read_bytes() == before_token, "Untrusted Dessen guest deleted protected credentials"
        assert cache_dir.joinpath("server-overview.json").read_bytes() == before_cache, "Guest deleted server cache"
        native_refresh = run(host, app, x3, keys=b"uc", native=True)
        assert "[launch stats-refresh" in native_refresh, "Paired X3 should refresh through native firmware"
        assert resume.read_bytes() == b"\x00", "Manual refresh must return to Overall"
        resume.unlink()
        # Touch tests run in an X3-native capability simulation to validate the
        # whole month widget; X3 hardware itself uses physical buttons.
        month_touch = run(host, app, x3, keys=b"m", native=True, touch=True)
        assert "Sync with server?" in month_touch, "Month tap failed to show confirmation"
        assert "[launch stats-refresh" not in month_touch, "Single tap must never initiate networking"
        assert not resume.exists(), "Unconfirmed tap created a native resume marker"
        cancelled = run(host, app, x3, keys=b"mb", native=True, touch=True)
        assert "Sync with server?" in cancelled and "[launch stats-refresh" not in cancelled
        assert "THIS MONTH - SERVER" in cancelled, "Cancel failed to return to Overall"
        assert not resume.exists(), "Cancelled sync wrote a resume marker"
        tap_cancelled = run(host, app, x3, keys=b"mN", native=True, touch=True)
        assert "[launch stats-refresh" not in tap_cancelled, "Cancel touch launched networking"
        assert not resume.exists()
        confirmed = run(host, app, x3, keys=b"mc", native=True, touch=True)
        assert "Sync with server?" in confirmed, "Confirmation missing"
        assert "Syncing..." in confirmed, "Monthly hours were not replaced before network operation"
        assert "[launch stats-refresh" in confirmed, "Confirmed sync failed to request native handoff"
        assert confirmed.index("Syncing...") < confirmed.index("[launch stats-refresh"), "Network handoff preceded status render"
        assert resume.read_bytes() == b"\x00"
        assert "api/device/overview" not in confirmed, "Guest performed an HTTP request"
        resume.unlink()
        touch_confirmed = run(host, app, x3, keys=b"mY", native=True, touch=True)
        assert "Syncing..." in touch_confirmed and "[launch stats-refresh" in touch_confirmed, (
            "Sync now touch did not confirm the request: " + touch_confirmed[-2200:]
        )
        assert resume.read_bytes() == b"\x00"
        resume.unlink()
        # The snapshot is owned by the Dessen app directory. No legacy SD
        # Reading Statistics installation is present anywhere in this fixture.
        alt_cache = cache_dir / "server-overview.json"
        assert alt_cache.exists()
        assert not (x3 / ".crosspoint" / "plugins" / "reading-statistics").exists()
        # A recent successful sync hides the icon until the user touches
        # This Month; an aged cache shows it without a tap.
        import time
        cached = json.loads(alt_cache.read_text())
        cached["synced_at"] = int(time.time())
        alt_cache.write_text(json.dumps(cached))
        fresh = run(host, app, x3, keys=b"b", native=True, touch=True)
        assert "line 751,63 - 753,57" not in fresh, "Fresh sync still shows its icon"
        cached["synced_at"] = 1
        alt_cache.write_text(json.dumps(cached))
        stale = run(host, app, x3, keys=b"b", native=True, touch=True)
        assert "line 751,63 - 753,57" in stale, "Old snapshot failed to show the sync icon"
    print("Dessen device pairing fixtures: PASS")


if __name__ == "__main__":
    main(*sys.argv[1:3])
