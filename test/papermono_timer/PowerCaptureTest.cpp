#include <HalGPIO.h>

#include <cassert>
#include <cstdio>

#define LOG_ERR(...)
using gpio_num_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, GPIO_INTR_ANYEDGE = 3;
static bool bootReady = true, readOk = true;
static uint8_t buttonStatus = 0;
static unsigned reads = 0;
namespace freeink {
namespace papermono {
bool ensureBooted() { return bootReady; }
}  // namespace papermono
namespace m5pm1 {
constexpr uint8_t BTN_EVENT = 0x80, BTN_PRESSED = 1;
bool readButtonStatus(uint8_t* status) {
  ++reads;
  if (!readOk) return false;
  *status = buttonStatus;
  buttonStatus &= ~BTN_EVENT;
  return true;
}
}  // namespace m5pm1
}  // namespace freeink
namespace BoardConfig {
struct {
  struct {
    int8_t power = -1;
    bool powerActiveHigh = false;
  } input;
} ACTIVE;
}  // namespace BoardConfig
struct {
} GPIO;
int gpio_ll_get_level(decltype(GPIO)*, int) {
  assert(false);
  return 0;
}
int64_t esp_timer_get_time() {
  assert(false);
  return 0;
}
int gpio_install_isr_service(int) {
  assert(false);
  return 0;
}
int gpio_set_intr_type(gpio_num_t, int) {
  assert(false);
  return 0;
}
int gpio_isr_handler_add(gpio_num_t, void (*)(void*), void*) {
  assert(false);
  return 0;
}
int gpio_intr_disable(gpio_num_t) {
  assert(false);
  return 0;
}
int gpio_isr_handler_remove(gpio_num_t) {
  assert(false);
  return 0;
}
TimerHandle_t xTimerCreateStatic(const char*, TickType_t, int, void*, TimerCallbackFunction_t, StaticTimer_t*) {
  assert(false);
  return nullptr;
}
int xTimerStart(TimerHandle_t, TickType_t) {
  assert(false);
  return 0;
}
int xTimerStop(TimerHandle_t, TickType_t) {
  assert(false);
  return 0;
}
void* pvTimerGetTimerID(TimerHandle_t) {
  assert(false);
  return nullptr;
}
#include "CaptureSource.inc"

int main() {
  using Request = HalGPIO::PowerWakeRequest;
  HalGPIO gpio;
  assert(gpio.beginPowerWakeCapture());
  assert(gpio.powerWakeRequest() == Request::None);
  buttonStatus = freeink::m5pm1::BTN_EVENT;  // Released while main was blocked.
  assert(gpio.powerWakeRequest() == Request::Verified);
  assert(buttonStatus == 0);
  const auto afterEvent = reads;
  assert(gpio.powerWakeRequest() == Request::Verified && reads == afterEvent);
  gpio.endPowerWakeCapture();
  assert(gpio.powerWakeRequest() == Request::None && reads == afterEvent);

  buttonStatus = freeink::m5pm1::BTN_PRESSED;
  assert(gpio.beginPowerWakeCapture());
  assert(gpio.powerWakeRequest() == Request::Verified);
  gpio.endPowerWakeCapture();
  buttonStatus = 0x40;  // Unrelated status bits do not ask for reader wake.
  assert(gpio.beginPowerWakeCapture());
  assert(gpio.powerWakeRequest() == Request::None);
  readOk = false;
  assert(gpio.powerWakeRequest() == Request::Verified);  // Leave maintenance on read failure.
  gpio.endPowerWakeCapture();
  assert(!gpio.beginPowerWakeCapture());
  assert(gpio.powerWakeRequest() == Request::None);
  readOk = true;
  bootReady = false;
  assert(!gpio.beginPowerWakeCapture());
  assert(gpio.powerWakeRequest() == Request::None);
  std::puts("PMIC capture: latched/held contact, read-clear retention, failure and cleanup PASS");
}
