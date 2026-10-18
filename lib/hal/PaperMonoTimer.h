#pragma once

// PaperMono RTC/PMIC timer adapter adapted from Tomek Fiedoruk (@fiedoruk):
// https://github.com/fiedoruk/crosspoint-reader/tree/5a4a99c9e509fc8da6f622a2c4b72ddf136a3ff7/contrib/papermono-local-timed

#include <PaperMonoBoard.h>
#include <Rtc.h>

namespace papermonotimer {
inline bool cancel() {
  freeink::Rtc rtc;
  const bool stopped = rtc.begin() && rtc.cancelWakeTimer();
  const bool disabled = freeink::papermono::setRtcWake(false);
  return stopped && disabled;
}

inline bool takeWake(bool& timerWake) {
  timerWake = false;
  if (!freeink::papermono::ensureBooted()) return false;
  const auto wake = freeink::papermono::bootWakeState();
  freeink::Rtc rtc;
  bool fired = false;
  const bool read = rtc.begin() && rtc.readWakeTimer(fired);
  const bool stopped = rtc.present() && rtc.cancelWakeTimer();
  const bool disabled = freeink::papermono::setRtcWake(false);
  if (!read || !stopped || !disabled || !wake.valid) return false;
  // Exclude a button/USB/reset flag or another enabled PMIC wake input.
  timerWake = fired && wake.source == freeink::m5pm1::WAKE_EXTERNAL &&
              (wake.enabledPins & freeink::m5pm1::GPIO_WAKE_MASK) == freeink::m5pm1::GPIO0;
  return true;
}

inline bool arm(uint64_t intervalUs) {
  if (!cancel()) return false;
  if (!intervalUs) return true;
  if (intervalUs % 1000000ULL || intervalUs / 1000000ULL > UINT32_MAX) return false;
  freeink::Rtc rtc;
  if (!rtc.begin() || !freeink::papermono::setRtcWake(true) ||
      !rtc.armWakeTimer(static_cast<uint32_t>(intervalUs / 1000000ULL))) {
    cancel();
    return false;
  }
  return true;
}
}  // namespace papermonotimer
