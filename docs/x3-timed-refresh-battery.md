# X3 timed wallpaper refresh — battery power measurements

These measurements were taken on an Xteink X3 running Razor-Crest's
**Timed Sleep-Screen Refresh** with local wallpaper cycling. The tests
used the **battery-capable, GPIO13-HIGH timed-sleep implementation**:
the X3 keeps its SD/power rail asserted so the ESP32-C3 can wake and
rotate a sleep image without USB/pogo power. These are gauge-derived
whole-test averages, **not** direct electrical measurements of the
SD-card supply or a full-discharge endurance trial.

## Recorded tests

| Metric | 1-minute selection | 15-minute selection |
| --- | ---: | ---: |
| Elapsed time | 37,044 s (10 h 17 m 24 s) | 73,188 s (20 h 19 m 48 s) |
| Timer wakes | 566 | 80 |
| Gauge remaining-capacity decrease | 47 mAh | 37 mAh |
| Battery state-of-charge change | 100% → 93% | 100% → 94% |
| Observed average refresh cycle | ~65.4 s/wake | ~914.9 s/wake |
| Average battery current (capacity / hours) | **4.57 mA** | **1.82 mA** |
| Projected consumption over 24 h | **109.6 mAh/day** | **43.7 mAh/day** |
| Estimated sleep-only endurance from 640 mAh | **5.8 days** | **14.7 days** |

The elapsed time, wake counts, mAh deltas and start/end percentages
above are the recorded X3 battery-test results. All current, per-day
consumption and endurance figures are **calculated from those results**,
not separately measured. The one-minute refresh takes some active time
(the observed cycle was about 65 seconds), so wake counts should be
used rather than assuming precisely 60 actual wakes per hour.
The two tests had different lengths and therefore should be
compared by current or consumption per day, not raw mAh or percent lost.
At the tested settings, the 15-minute timer drew about **60% less
average current** than the 1-minute timer.

## Projecting other intervals

For a deliberately simple two-point approximation, express average
current as:

`average_mA = idle_mA + energy_per_wake_mAh * wakes_per_hour`

Using **actual observed wake counts** from the two tests:

- 1-minute test: 566 / 10.29 = 55.00 wakes/hour;
  47 / 10.29 = 4.568 mA.
- 15-minute test: 80 / 20.33 = 3.935 wakes/hour;
  37 / 20.33 = 1.820 mA.
- Two-point fit: **~1.61 mA baseline while timed sleep stays enabled**,
  plus **~0.054 mAh per wallpaper refresh**.

The fitted baseline includes *all* consumption between refreshes,
not just the SD card. It is **not** a measured SD-rail-only current.
For longer selections, use the fitted baseline plus 60 / interval_minutes
refreshes per hour. Approximate results, assuming a **fully charged
640 mAh battery**:

| Selected interval | Sleep-only endurance | Basis | Days shown in X3 warning |
| --- | ---: | --- | ---: |
| Every 1 minute | ~5.8 days | Extrapolation of measured 4.57 mA average | ~6 |
| Every 15 minutes | ~14.7 days | Extrapolation of measured 1.82 mA average | ~15 |
| Every 30 minutes | ~15.5 days | Two-point model, **not tested** | ~16 |
| Every 1 hour | ~16.0 days | Two-point model, **not tested** | ~16 |
| Every 3 hours | ~16.4 days | Two-point model, **not tested** | ~16 |
| Every 6 hours | ~16.5 days | Two-point model, **not tested** | ~17 |
| Every 12 hours | ~16.5 days | Two-point model, **not tested** | ~17 |
| Every 24 hours | ~16.6 days | Two-point model, **not tested** | ~17 |

The firmware intentionally rounds to whole days, because the gauge,
capacity and fitted model cannot justify greater precision. The
nominal battery capacity may not equal the usable capacity on every
device or after aging. A previous normal-sleep, timer-Off observation
was approximately **1% SoC lost over 24 hours**, but percentage
rounding and the lack of a comparable capacity-based measurement mean
it is **not** a precise control-current measurement.

### Essential warning / scope

**Every estimate starts from a fully charged battery and assumes
the device stays asleep with only wallpaper refreshes running.
Normal reading, Wi-Fi, other day-to-day use, aging and varying
conditions are NOT included. Actual runtime may be shorter.**

These values do **not** predict remaining days at the reader's current
battery percentage, nor do they imply the device will run for the shown
duration while being used for reading. A second test at different
intervals and/or direct electrical measurement would improve the model.

## Hardware constraint and attempted optimization

On X3, GPIO13 is assigned to the SD/power rail; X4 uses GPIO13 as a
battery-power latch, despite sharing its C3 firmware binary. When
the X3 rail was held LOW during timed deep sleep, wallpaper rotation
worked when sleep began on USB/pogo power but **did not** cycle when
sleep began on battery. Plugging in USB *after* entering battery sleep
did not restart cycling. The experimental early-rail-restore worktree
`thinkforge/x3-timed-sd-rail-off` **failed the battery-only test** and
must **not** be integrated.

This points to a power or wake dependency, but without a measured
ESP32 supply/reset trace it does not conclusively identify the circuit
responsible. The proven compromise retains GPIO13 HIGH during X3
timed sleep; the resulting standby drain is why an X3-only
confirmation is presented before saving any different, non-Off
interval. Selecting Off needs no warning and restores normal sleep.

## Test and attribution references

- Implementation and device acceptance steps:
  [timed-wake-testing.md](timed-wake-testing.md).
- PaperMono RX8130/PMIC timer adaptation and Power-handoff credit:
  [papermono-timer.md](papermono-timer.md).
- Contributor **Tomek Fiedoruk (@fiedoruk)** authored the T1 Power
  handoff and T2 PaperMono adaptations, with exact patch/commit links
  in the PaperMono document and a `Co-authored-by` trailer on commit
  `b6f10648`. The upstream timer primitive is attributed there to
  **Michael Kazakov (`@kivarum`, upstream PR #3820)**;
  Jadehawk authored Razor-Crest's local wallpaper
  cycle and X3 rail handling. These credits must be retained when
  merging into `razor-crest`.
