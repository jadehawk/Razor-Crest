#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#define FREEINK_DEVICE_PAPERMONO 1
#define SOC_PM_SUPPORT_EXT1_WAKEUP 1
#define ENABLE_SERIAL_LOG
using gpio_num_t = int;
constexpr int XTEINK_C3_GPIO13 = 13, OUTPUT = 1, HIGH = 1;
struct PoweredOff {};
struct Restarted {};
static std::vector<std::string> trace;
static bool armOk = true, cancelOk = true, removePower = true, shutdownOk = true;
static uint64_t requested = 0;
#define LOG_ERR(...) trace.push_back("error")
struct HalGPIO {
  enum class PowerWakeRequest { None, Verified };
  static inline PowerWakeRequest request = PowerWakeRequest::None;
  PowerWakeRequest powerWakeRequest() { return request; }
};
static bool pressDuringArm = false, pressDuringShutdown = false, pressDuringSettle = false;
struct HalPowerManager {
  void startDeepSleep(HalGPIO&, uint64_t = 0, bool = false) const;
};
struct Serial {
  void end() { trace.push_back("serial-end"); }
} logSerial;
namespace BoardConfig {
struct Profile {
  struct Power {
    int8_t latch0 = -1, latch1 = -1;
  } power;
} ACTIVE;
}  // namespace BoardConfig
void gpio_hold_dis(int) {}
void gpio_hold_en(int) {}
void pinMode(int, int) {}
void digitalWrite(int, int) {}
void delay(int ms) {
  assert(ms == 1000);
  trace.push_back("settle");
}
[[noreturn]] void esp_restart() {
  trace.push_back("restart");
  throw Restarted{};
}
namespace papermonotimer {
bool arm(uint64_t us) {
  requested = us;
  if (pressDuringArm) HalGPIO::request = HalGPIO::PowerWakeRequest::Verified;
  trace.push_back("arm");
  return armOk;
}
bool cancel() {
  trace.push_back("cancel");
  return cancelOk;
}
}  // namespace papermonotimer
namespace freeink {
struct PowerManager {
  static void powerDownRailsForSleep() { trace.push_back("rails-off"); }
  static void armPowerButtonWakeup() { assert(false); }
  static void deepSleep() { assert(false); }
  static void deepSleepUntilPowerButton() { assert(false && "PaperMono has no ESP Power wake pin"); }
};
namespace m5pm1 {
bool requestShutdown(bool (*shouldAbort)(void*) = nullptr, void* context = nullptr) {
  // The SDK's 120 ms settle happens before its abort check and PMIC command.
  if (pressDuringSettle) HalGPIO::request = HalGPIO::PowerWakeRequest::Verified;
  if (shouldAbort && shouldAbort(context)) {
    trace.push_back("abort");
    return false;
  }
  trace.push_back("shutdown");
  if (pressDuringShutdown) HalGPIO::request = HalGPIO::PowerWakeRequest::Verified;
  if (removePower) throw PoweredOff{};
  return shutdownOk;
}
}  // namespace m5pm1
}  // namespace freeink
#include "SleepSource.inc"

int main() {
  HalGPIO gpio;
  HalPowerManager power;
  try {
    power.startDeepSleep(gpio, 60000000);
    assert(false);
  } catch (const PoweredOff&) {
  }
  assert(requested == 60000000);
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "shutdown"}));
  for (bool result : {false, true}) {
    trace.clear();
    removePower = false;
    shutdownOk = result;
    try {
      power.startDeepSleep(gpio, 60000000);
      assert(false);
    } catch (const Restarted&) {
    }
    const std::vector<std::string> expected =
        result ? std::vector<std::string>{"serial-end", "rails-off", "arm",   "shutdown",
                                          "settle",     "cancel",    "error", "restart"}
               : std::vector<std::string>{"serial-end", "rails-off", "arm", "shutdown", "cancel", "error", "restart"};
    assert(trace == expected);
  }
  trace.clear();
  armOk = false;
  cancelOk = true;
  removePower = true;
  try {
    power.startDeepSleep(gpio, 60000001);
    assert(false);
  } catch (const PoweredOff&) {
  }
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "error", "cancel", "shutdown"}));
  trace.clear();
  cancelOk = false;
  try {
    power.startDeepSleep(gpio, 60000000);
    assert(false);
  } catch (const Restarted&) {
  }
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "error", "cancel", "error", "restart"}));
  trace.clear();
  armOk = true;
  cancelOk = true;
  removePower = true;
  try {
    power.startDeepSleep(gpio);
    assert(false);
  } catch (const PoweredOff&) {
  }
  assert(requested == 0);
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "shutdown"}));
  trace.clear();
  HalGPIO::request = HalGPIO::PowerWakeRequest::Verified;
  power.startDeepSleep(gpio, 60000000, true);
  assert(trace.empty());
  HalGPIO::request = HalGPIO::PowerWakeRequest::None;
  pressDuringArm = true;
  power.startDeepSleep(gpio, 60000000, true);
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "cancel"}));
  trace.clear();
  HalGPIO::request = HalGPIO::PowerWakeRequest::None;
  pressDuringArm = false;
  removePower = false;
  shutdownOk = false;
  pressDuringShutdown = true;
  power.startDeepSleep(gpio, 60000000, true);
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "shutdown", "cancel"}));
  trace.clear();
  HalGPIO::request = HalGPIO::PowerWakeRequest::None;
  pressDuringShutdown = false;
  pressDuringSettle = true;
  removePower = true;
  power.startDeepSleep(gpio, 60000000, true);
  assert((trace == std::vector<std::string>{"serial-end", "rails-off", "arm", "abort", "cancel"}));
}
