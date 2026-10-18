# PaperMono timer wake

This integrates the contributed PaperMono adapter into the current
standard `razor-crest` branch, including the
Power handoff prerequisite. The pinned SDK `084fdfb2` descends from
`7fcd3b34` and includes FreeInk PR #138; no SDK
revision or RTC driver is changed.

## Credits and source provenance

**Tomek Fiedoruk ([@fiedoruk](https://github.com/fiedoruk))** authored
the Power-button handoff and PaperMono RTC/PMIC adapter used by this branch.
These integrations are adapted from his original MIT-licensed source:

- **T1 — Power handoff:**
  [`t1-power.patch`](https://github.com/fiedoruk/crosspoint-reader/blob/3529a9dadea4a93cb9e6e9ef87712331c750d7db/contrib/timed-power-handoff/t1-power.patch)
  at commit `3529a9dadea4a93cb9e6e9ef87712331c750d7db`.
  The GPIO/Power HAL and startup handoff adapt this contribution; the
  `test/timed_power_wake/` tests follow its test implementation.
- **T2 — PaperMono RTC/PMIC adapter:**
  [`t2-papermono.patch`](https://github.com/fiedoruk/crosspoint-reader/blob/5a4a99c9e509fc8da6f622a2c4b72ddf136a3ff7/contrib/papermono-local-timed/t2-papermono.patch)
  at commit `5a4a99c9e509fc8da6f622a2c4b72ddf136a3ff7`.
  The PaperMono HAL timer adapter, settings gates and `test/papermono_timer/`
  tests adapt this contribution.

Michael Kazakov authored the upstream timer-wake primitive. Jadehawk
authored Razor-Crest's local wallpaper rotation and X3 SD-rail handling.
Both source bundles include CrossPoint's MIT license:
[T1 license](https://github.com/fiedoruk/crosspoint-reader/blob/3529a9dadea4a93cb9e6e9ef87712331c750d7db/contrib/timed-power-handoff/LICENSE)
and [T2 license](https://github.com/fiedoruk/crosspoint-reader/blob/5a4a99c9e509fc8da6f622a2c4b72ddf136a3ff7/contrib/papermono-local-timed/LICENSE).
The source contributor's physical PaperMono results do not validate a binary
built from this newer Razor-Crest branch.

## Implementation

`startDeepSleep(gpio, intervalUs)` uses RX8130 and PMIC GPIO0 on PaperMono. The
ESP loses power during PMIC shutdown, so the ESP timer cannot provide this wake.
Use whole seconds: 1 through 3,932,100 seconds. The SDK counts intervals above
65,535 seconds in minutes, rounded up. Zero clears the RTC timer and its PMIC
route before ordinary button-only shutdown. Invalid intervals or an arm failure
fall back to button-only shutdown only after successful cancellation. If the
route cannot be cleared, firmware restarts instead of shutting down with unknown
wake state. A returned PMIC shutdown also cancels the timer and restarts; it must
not fall through into ESP-only sleep without a usable Power-button wake source.

The first wake query consumes the RTC flag and clears the route. It reports
`Timer` only when RTC timer state and the PMIC's exclusive GPIO0 wake source agree,
with a cold power-on reset and no ESP wake cause. Button/USB flags, another wake
input, invalid RTC state, I2C failure, and software/watchdog resets do not establish
a board RTC wake. The RTC wake result is cached for that boot; subsequent queries do not read or
clear the RTC again.

The existing local Custom sleep-screen refresh is now available on PaperMono.
The default remains Off. This adds no network request or second timer consumer.
During maintenance, the HAL reads the PMIC's held/read-clear event bits. A click
released during blocking panel I/O is retained until the next check. There is no
GPIO interrupt or periodic sampler for this board. PMIC read failure leaves the
maintenance path; capture setup failure prevents periodic refresh.

Power checked before shutdown, including the SDK pre-command settle callback,
cancels the RTC route and returns to the ordinary
reader through the existing software handoff. A returned shutdown on USB also
cancels the route and restarts instead of entering unusable ESP-only sleep. A
Power event read after returned shutdown uses the same handoff. The final PMIC
power-cut transition still needs physical timing checks; host tests cannot
establish that every possible pulse is retained there.

## Verification

```sh
python3 test/papermono_timer/run.py
python3 test/timer_wake_arm/run.py
```

The PaperMono tests compile the real adapter and RTC driver against the SDK's
recording I2C bus. Reset/wake values and PMIC power removal are simulated. The
sleep-path tests use the production function with controlled API results. Neither
is evidence of physical power removal, battery runtime or button behavior. A new
PaperMono binary requires its own device check before claiming those results.

For device acceptance, check one-minute local rotation on battery, a real Power
click during panel work returning to the same book, Off after an armed cycle,
and the returned-shutdown path on USB when reachable. Keep brownout detection
enabled. Record the firmware hash and scoped results with the contribution;
earlier adapter results do not qualify a new binary.
