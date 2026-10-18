#include <HalGPIO.h>

#include <cassert>
#include <cstdio>

#define LOG_ERR(...)
using gpio_num_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, GPIO_INTR_ANYEDGE = 3;
static int serviceResult = 0, edgeResult = 0, attachResult = 0;
static int level = HIGH, removes = 0, disables = 0;
static uint64_t nowUs = 0;
static void (*handler)(void*) = nullptr;
static void* argument = nullptr;
static bool createTimerFails = false, startTimerFails = false, stopTimerFails = false;
static TimerHandle_t timer = nullptr;
TimerHandle_t xTimerCreateStatic(const char*, TickType_t ticks, int repeat, void* id, TimerCallbackFunction_t callback,
                                 StaticTimer_t* storage) {
  assert(ticks == 5 && repeat == pdTRUE);
  if (createTimerFails) return nullptr;
  *storage = {id, callback, false};
  timer = storage;
  return timer;
}
int xTimerStart(TimerHandle_t t, TickType_t wait) {
  assert(wait == 0);
  if (startTimerFails) return 0;
  t->active = true;
  return pdPASS;
}
int xTimerStop(TimerHandle_t t, TickType_t wait) {
  assert(wait == 0);
  if (stopTimerFails) return 0;
  t->active = false;
  return pdPASS;
}
void* pvTimerGetTimerID(TimerHandle_t t) { return t->id; }
static void sample() {
  if (timer && timer->active) timer->callback(timer);
}
struct {
} GPIO;
int gpio_ll_get_level(decltype(GPIO)*, int pin) {
  assert(pin == 3);
  return level;
}
int64_t esp_timer_get_time() { return nowUs; }
int gpio_install_isr_service(int flags) {
  assert(flags == 0);
  return serviceResult;
}
int gpio_set_intr_type(gpio_num_t pin, int type) {
  assert(pin == 3 && type == GPIO_INTR_ANYEDGE);
  return edgeResult;
}
int gpio_isr_handler_add(gpio_num_t pin, void (*callback)(void*), void* context) {
  assert(pin == 3);
  if (attachResult != ESP_OK) return attachResult;
  handler = callback;
  argument = context;
  return ESP_OK;
}
int gpio_intr_disable(gpio_num_t pin) {
  assert(pin == 3);
  ++disables;
  return ESP_OK;
}
int gpio_isr_handler_remove(gpio_num_t pin) {
  assert(pin == 3);
  ++removes;
  handler = nullptr;
  argument = nullptr;
  return ESP_OK;
}
namespace BoardConfig {
struct {
  struct {
    int8_t power = 3;
    bool powerActiveHigh = false;
  } input;
} ACTIVE;
}  // namespace BoardConfig

#include "CaptureSource.inc"

static void edge(bool pressed) {
  level = pressed == BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  if (handler) handler(argument);
}

int main() {
  using Request = HalGPIO::PowerWakeRequest;
  HalGPIO gpio;
  createTimerFails = true;
  assert(!gpio.beginPowerWakeCapture());
  createTimerFails = false;
  assert(gpio.beginPowerWakeCapture() && handler);
  assert(gpio.powerWakeRequest() == Request::None);
  edge(true);
  nowUs += 15000;
  sample();  // The timer daemon runs while the main task is blocked.
  edge(false);
  assert(gpio.powerWakeRequest() == Request::Verified);  // No polling during the press.
  gpio.endPowerWakeCapture();
  assert(!handler && removes == 1 && gpio.powerWakeRequest() == Request::None);

  assert(gpio.beginPowerWakeCapture());
  handler(argument);  // Both edges happened before the ISR; the pin is released.
  assert(gpio.powerWakeRequest() == Request::Pressed);
  gpio.endPowerWakeCapture();

  assert(gpio.beginPowerWakeCapture());
  edge(true);
  nowUs += 5000;
  level = HIGH;  // Physical release; defer the GPIO ISR until 15 ms.
  nowUs += 10000;
  sample();
  handler(argument);
  assert(gpio.powerWakeRequest() == Request::Pressed);
  gpio.endPowerWakeCapture();

  assert(gpio.beginPowerWakeCapture());
  edge(true);
  nowUs += 9000;
  edge(false);
  assert(gpio.powerWakeRequest() == Request::Pressed);
  gpio.endPowerWakeCapture();

  edge(true);
  assert(gpio.beginPowerWakeCapture());
  nowUs += 10000;
  assert(gpio.powerWakeRequest() == Request::Verified);
  edge(false);
  gpio.endPowerWakeCapture();

  nowUs = UINT32_MAX - 5000ULL;
  assert(gpio.beginPowerWakeCapture());
  edge(true);
  nowUs += 10000;
  sample();
  edge(false);
  assert(gpio.powerWakeRequest() == Request::Verified);
  gpio.endPowerWakeCapture();

  serviceResult = ESP_ERR_INVALID_STATE;
  BoardConfig::ACTIVE.input.powerActiveHigh = true;
  edge(false);
  assert(gpio.beginPowerWakeCapture());
  edge(true);
  nowUs += 10000;
  sample();
  edge(false);
  assert(gpio.powerWakeRequest() == Request::Verified);
  gpio.endPowerWakeCapture();

  serviceResult = 9;
  assert(!gpio.beginPowerWakeCapture() && !handler);
  serviceResult = 0;
  edgeResult = 9;
  assert(!gpio.beginPowerWakeCapture() && !handler);
  edgeResult = 0;
  attachResult = 9;
  const int before = disables;
  assert(!gpio.beginPowerWakeCapture() && !handler && disables == before + 1);
  assert(gpio.powerWakeRequest() == Request::None);
  attachResult = 0;
  startTimerFails = true;
  assert(!gpio.beginPowerWakeCapture() && !handler && !timer->active);
  startTimerFails = false;
  assert(gpio.beginPowerWakeCapture());
  stopTimerFails = true;
  gpio.endPowerWakeCapture();
  sample();  // A full command queue can leave a callback pending after cleanup.
  assert(!handler && gpio.powerWakeRequest() == Request::None);
  stopTimerFails = false;
  gpio.endPowerWakeCapture();
  BoardConfig::ACTIVE.input.power = -1;
  assert(!gpio.beginPowerWakeCapture() && !handler);
  std::puts("Production capture: both polarities, released/held presses, stability, wrap, cleanup and failures passed");
}
