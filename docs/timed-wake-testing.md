# Timed wake testing

Razor-Crest's **Timed Sleep-Screen Refresh** is an experimental downstream consumer of the optional timer-wake primitive introduced in upstream CrossPoint PR [#3820](https://github.com/crosspoint-reader/crosspoint-reader/pull/3820).

The upstream primitive stays neutral: `startDeepSleep(..., timerWakeUs)` optionally arms a one-shot timer wake, `WakeupReason::Timer` identifies that wake on the next boot, and a zero interval preserves the existing button-only sleep behavior. Razor-Crest adds the product behavior on top: when **Sleep Screen** is set to **Custom**, a timer wake performs a minimal local maintenance boot, rotates to another valid BMP from the hidden `.sleep` directory, refreshes the e-ink panel, and immediately returns to deep sleep. It does not join Wi-Fi, fetch remote content, run a scheduler, or expose a new plugin capability.

## X3 hardware detail

X3 and X4 share the ESP32-C3 firmware image but use GPIO13 differently. On X4 it is the battery/power latch. On X3 it switches the SD-card power rail.

During initial testing, timed wallpaper rotation worked on X3 only while USB/pogo power was present. Battery-only testing showed that the normal X3 sleep path held GPIO13 LOW, leaving the SD card unavailable to the unattended timer-wake maintenance boot. Razor-Crest therefore reasserts the X3 SD rail only after a timer wake has been successfully armed. Normal X3 sleep with **Timed Sleep-Screen Refresh = Off**, and timer-arm failure paths, retain the original SD-rail-off behavior.

This correction has been hardware-tested on X3: one-minute Custom wallpaper rotation continues on battery power, manual power-button wake still works, and setting Timed Sleep-Screen Refresh back to Off stops the periodic wake/rotation behavior.

Keeping the X3 SD rail powered during timed sleep may increase standby current. Battery-consumption measurement is still in progress, so X3 timed refresh should be treated as experimental until that cost is quantified.

## Test procedure

Use **Custom** sleep mode with at least two valid BMP files in the SD-card root `.sleep` directory. The one-minute interval is intended for functional testing; longer intervals are more representative of real use.

For each device under test:

1. Flash the timed-wake test firmware for the device.
2. Set **Sleep Screen = Custom**.
3. Set **Timed Sleep-Screen Refresh = 1 minute**.
4. Put the device to sleep.
5. Disconnect external power if the device can run from battery.
6. Confirm that the retained sleep image changes repeatedly at roughly one-minute intervals without user input.
7. Wake with the normal power button and confirm normal resume behavior.
8. Put the device to sleep again and confirm timed rotation resumes.
9. Set **Timed Sleep-Screen Refresh = Off**.
10. Put the device to sleep and confirm that periodic refresh stops while normal power-button wake still works.

Please report the exact device, panel/controller variant when known, whether the test was battery-only, the selected interval, and which steps passed or failed.

## Power during a timed refresh

The expected behavior is to retain Power while SD or panel I/O blocks the main
task; confirm this on the device being tested. The panel transfer may finish
before the reader resumes. A software restart restores the ordinary wake path,
so partly shut-down peripherals are initialized normally. No reader position is
written by this handoff.

On X3/X4 and other GPIO-button boards, the GPIO capture retains a request;
one static software timer on the existing
FreeRTOS timer daemon samples the held level while main is blocked. A press is
verified only while still held after at least 10 ms. A delayed release ISR alone
cannot establish its physical duration. An unverified contact follows the saved **Short Power Button Press** preference:
**Sleep** permits the click; otherwise it returns to the configured timer cadence.
This is separate from the long-press duration used to put an awake reader to sleep.

Test Power before refresh, during panel transfer and just as the device returns
to sleep, with both short-press settings. Confirm the same book position resumes
and the release does not immediately put it back to sleep. Also check timer Off,
ordinary button-only wake and the four orientations. The change adds no touch or
remapping behavior. Capture setup failure stops periodic refresh rather than
running a maintenance cycle without Power capture.

Host regression checks:

```sh
python3 test/timed_power_wake/run.py --baseline 59555361aef55608cc56fdb5a9fccc167c37db34
python3 test/timer_wake_arm/run.py
```

The first command checks the patched handoff and production capture methods and
requires the unpatched timer branch to reproduce the lost press (exit 42),
with simulated GPIO, clock and restart boundaries. The tests do not qualify the
physical input controller or panel timing.
The last capture check follows Power-wake arming. Pulses completed wholly inside
the subsequent SDK sleep-entry operation remain a hardware timing boundary;
these host checks do not establish a no-loss guarantee there.

## Baseline validation status

The [PaperMono candidate](papermono-timer.md) enables this existing local refresh
with RTC/PMIC wake and Power handoff. Build and device evidence must identify
the exact candidate; the r1 observations below remain historical.

The X3 observations below belong to timedwake-r1 (`5955536`); the Paper Mono
row describes the newly integrated host-tested adaptation. Earlier observations do not qualify
the later Power handoff; repeat the relevant device checks on the actual candidate.

| Device | Build path | Battery-only timed rotation | Off disables timer | Normal wake | Status |
| --- | --- | --- | --- | --- | --- |
| X3 | shared X3 / X4 C3 image | Pass | Pass | Pass | Hardware tested; battery impact measurement in progress |
| X4 | shared X3 / X4 C3 image | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| Seeed reTerminal Sticky | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| X4 Pro | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| X4 Classic | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| Paper Mono | dedicated build | Not yet tested on this branch | Not yet tested on this branch | RTC/PMIC timer and Power handoff added | Host tested; physical device verification required |
| MetalIO EInk4 | dedicated build | Not exposed | Not exposed | Existing sleep path retained | Timer-refresh setting intentionally unavailable |
| EEGO A4 | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |

The X3 and X4 rows use the same compiled C3 firmware image. Board detection selects the board-specific GPIO13 behavior at runtime; there is no separate X3 source tree or special X3-only firmware binary.

## Feedback for upstream

The useful upstream finding is not a change to the neutral timer primitive itself, but a board-specific downstream integration detail: a consumer that needs local SD access during a timer wake must account for hardware where the SD rail is intentionally powered down in normal deep sleep. X3 exposed that difference because GPIO13 is its SD power switch, while the same pin serves a different role on X4.

When sharing results on upstream PR #3820, include the Razor-Crest timed-wake branch/release, the X3 battery-only reproduction, the GPIO13 SD-rail finding, and the eventual measured standby-current impact. This gives other timer-wake consumers a concrete hardware case to test without turning the upstream primitive into a scheduler or product-specific feature.
