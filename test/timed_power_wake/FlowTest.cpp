#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#define FREEINK_DEVICE_PICCO 0
#define RTC_NOINIT_ATTR
#define LOG_DBG(...)
constexpr int ESP_RST_SW = 3;
static int resetReason = ESP_RST_SW;
static uint32_t clockMs = 0;
static int renders = 0, sleeps = 0;
static uint64_t configured = 60000000, sleptInterval = 0;
static bool timerPowerCaptureReady = true, isSilentReboot = false, rebootedFromPanic = false;
enum class Phase { None, BeforeRender, DuringRender, DisplaySleep, StorageSleep, InSleep };
static Phase phase = Phase::None;
struct Slept {};
struct Restarted {};
struct HalGPIO {
  static constexpr uint32_t POWER_WAKE_STABILITY_MS = 10;
  enum class WakeupReason { Timer, PowerButton };
  enum class PowerWakeRequest { None, Pressed, Verified };
  PowerWakeRequest request = PowerWakeRequest::None;
  int captureEnds = 0;
  PowerWakeRequest powerWakeRequest() { return request; }
  bool isUsbConnected() const { return false; }
  void endPowerWakeCapture() { ++captureEnds; }
} gpio;
static void inject(Phase at) {
  if (phase == at) gpio.request = HalGPIO::PowerWakeRequest::Verified;
}
uint32_t millis() { return clockMs; }
void delay(unsigned ms) { clockMs += ms; }
int esp_reset_reason() { return resetReason; }
struct FakeESP {
  [[noreturn]] void restart() { throw Restarted{}; }
};
[[maybe_unused]] FakeESP ESP;
struct CrossPointSettings {
  enum class SHORT_PWRBTN { IGNORE, SLEEP };
  SHORT_PWRBTN shortPwrBtn = SHORT_PWRBTN::IGNORE;
} SETTINGS;
static bool wakePowerReleasePending = false;
struct {
  void deepSleep() { inject(Phase::DisplaySleep); }
  void setInverted(bool) {}
} display;
struct {
  void deepSleep() {}
} halTiltSensor;
struct {
  void prepareForDeepSleep() { inject(Phase::StorageSleep); }
} Storage;
struct {
  void startDeepSleep(HalGPIO&, uint64_t interval = 0, bool fromTimer = false) {
    inject(Phase::InSleep);
    if (fromTimer && gpio.request != HalGPIO::PowerWakeRequest::None) return;
    ++sleeps;
    sleptInterval = interval;
    throw Slept{};
  }
} powerManager;
int renderer = 0, mappedInputManager = 0;
void setupDisplayAndFonts(bool) { inject(Phase::BeforeRender); }
uint64_t timedSleepScreenRefreshUs() { return configured; }
struct SleepActivity {
  SleepActivity(int, int) {}
  bool renderTimedSleepScreen() {
    ++renders;
    inject(Phase::DuringRender);
    return true;
  }
};

#include "HandoffSource.inc"

static bool runTimer() {
  try {
    switch (HalGPIO::WakeupReason::Timer) {
#include "TimerCase.inc"
      case HalGPIO::WakeupReason::PowerButton:
        break;
    }
  } catch (const Restarted&) {
    return true;
  } catch (const Slept&) {
    return false;
  }
  assert(false);
  return false;
}

[[maybe_unused]] static bool runPowerResume(HalGPIO::PowerWakeRequest request, bool wakeHoldVerified) {
  [[maybe_unused]] const auto resumedTimerPower = request;
  try {
    switch (HalGPIO::WakeupReason::PowerButton) {
#include "PowerCase.inc"
      case HalGPIO::WakeupReason::Timer:
        break;
    }
  } catch (const Slept&) {
    return false;
  }
  return wakePowerReleasePending;
}

static void reset(Phase at = Phase::None) {
  phase = at;
  gpio.request = HalGPIO::PowerWakeRequest::None;
  gpio.captureEnds = renders = sleeps = 0;
  configured = 60000000;
  timerPowerCaptureReady = true;
  isSilentReboot = rebootedFromPanic = false;
  clockMs = 0;
  wakePowerReleasePending = false;
  SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::IGNORE;
}

int main(int argc, char**) {
  reset(Phase::DuringRender);
  const bool resumed = runTimer();
  if (argc > 1) return resumed ? 0 : 42;
  assert(resumed && sleeps == 0 && renders == 1);
#if HAS_HANDOFF
  assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::Verified);
  assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::None);
  for (const auto at :
       {Phase::BeforeRender, Phase::DuringRender, Phase::DisplaySleep, Phase::StorageSleep, Phase::InSleep}) {
    reset(at);
    assert(runTimer() && sleeps == 0 && gpio.captureEnds == 1);
    assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::Verified);
  }
  reset();
  gpio.request = HalGPIO::PowerWakeRequest::Verified;
  assert(runTimer() && renders == 0);
  assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::Verified);
  reset();
  gpio.request = HalGPIO::PowerWakeRequest::Pressed;
  assert(runTimer() && renders == 0 && clockMs == 10);
  assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::Pressed);

  for (int mode = 0; mode < 4; ++mode) {
    reset();
    if (mode == 0) timerPowerCaptureReady = false;
    if (mode == 1) isSilentReboot = true;
    if (mode == 2) rebootedFromPanic = true;
    if (mode == 3) configured = 0;
    assert(!runTimer() && renders == 0 && sleptInterval == 0);
  }
  reset();
  assert(runPowerResume(HalGPIO::PowerWakeRequest::Verified, true) && sleeps == 0);
  reset();
  assert(!runPowerResume(HalGPIO::PowerWakeRequest::Pressed, false) && sleptInterval == configured);
  reset();
  SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::SLEEP;
  assert(runPowerResume(HalGPIO::PowerWakeRequest::Pressed, false) && sleeps == 0);
  reset();
  assert(!runPowerResume(HalGPIO::PowerWakeRequest::None, false) && sleptInterval == 0);
  for (const uint32_t stamp : {TIMED_POWER_PRESS, TIMED_POWER_VERIFIED, uint32_t{0xffffffff}}) {
    timedPowerWakeStamp = stamp;
    resetReason = 1;
    assert(consumeTimedPowerWake() == HalGPIO::PowerWakeRequest::None && timedPowerWakeStamp == 0);
  }
  resetReason = ESP_RST_SW;
#endif
  reset();
  assert(!runTimer() && renders == 1 && sleeps == 1 && sleptInterval == configured);
  std::puts("Timer Power handoff, sleep boundaries, wake policy, one-shot stamp and disabled cycles passed");
}
