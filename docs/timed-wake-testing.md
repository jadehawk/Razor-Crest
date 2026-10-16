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

## Current validation status

| Device | Build path | Battery-only timed rotation | Off disables timer | Normal wake | Status |
| --- | --- | --- | --- | --- | --- |
| X3 | shared X3 / X4 C3 image | Pass | Pass | Pass | Hardware tested; battery impact measurement in progress |
| X4 | shared X3 / X4 C3 image | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| Seeed reTerminal Sticky | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| X4 Pro | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| X4 Classic | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |
| Paper Mono | dedicated build | Not exposed | Not exposed | Existing sleep path retained | Timer-refresh setting intentionally unavailable |
| MetalIO EInk4 | dedicated build | Not exposed | Not exposed | Existing sleep path retained | Timer-refresh setting intentionally unavailable |
| EEGO A4 | dedicated build | Not yet independently reported | Not yet independently reported | Existing non-timed wake path retained | Testing requested |

The X3 and X4 rows use the same compiled C3 firmware image. Board detection selects the board-specific GPIO13 behavior at runtime; there is no separate X3 source tree or special X3-only firmware binary.

## Feedback for upstream

The useful upstream finding is not a change to the neutral timer primitive itself, but a board-specific downstream integration detail: a consumer that needs local SD access during a timer wake must account for hardware where the SD rail is intentionally powered down in normal deep sleep. X3 exposed that difference because GPIO13 is its SD power switch, while the same pin serves a different role on X4.

When sharing results on upstream PR #3820, include the Razor-Crest timed-wake branch/release, the X3 battery-only reproduction, the GPIO13 SD-rail finding, and the eventual measured standby-current impact. This gives other timer-wake consumers a concrete hardware case to test without turning the upstream primitive into a scheduler or product-specific feature.
