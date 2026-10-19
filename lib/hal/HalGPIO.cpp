#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>

#if FREEINK_DEVICE_PAPERMONO
#include "PaperMonoTimer.h"
#endif

// Global HalGPIO instance
HalGPIO gpio;

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  const uint8_t pageButtons = inputMgr.capacitivePageButtonMask();
  capacitivePagePressed = (pageButtons & ~previousCapacitivePageButtons) != 0;
  previousCapacitivePageButtons = pageButtons;
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

void HalGPIO::prepareForDeepSleep() {
  if (!inputMgr.prepareForDeepSleep()) LOG_ERR("GPIO", "Failed to put touch controller to sleep");
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

bool HalGPIO::rawInputActive() {
  if (inputMgr.isPowerButtonPhysicallyPressed()) return true;
  InputManager::ButtonAdcSample g1{}, g2{};
  inputMgr.readButtonAdc(g1, g2);
  // The Xteink ladder idles at the ADC full-scale rail (~4095); every button band sits below 3900.
  constexpr int kIdleRailMin = 4000;
  return (g1.raw >= 0 && g1.raw < kIdleRailMin) || (g2.raw >= 0 && g2.raw < kIdleRailMin);
}

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyPressed() const { return inputMgr.wasHomeKeyPressed(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::hasToggleSwitch() const { return inputMgr.hasToggleSwitch(); }

bool HalGPIO::isToggleSwitchOn() const { return inputMgr.isToggleSwitchOn(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

// Power handoff adapted from Tomek Fiedoruk (@fiedoruk), T1 source 3529a9da.
void IRAM_ATTR HalGPIO::recordPowerWake(const bool pressed, const uint32_t nowUs) {
  if (pressed && !powerCaptureDown) {
    if (powerCaptureRequest == PowerWakeRequest::None) powerCaptureRequest = PowerWakeRequest::Pressed;
    powerCaptureStartUs = nowUs;
  } else if (pressed && powerCaptureDown && nowUs - powerCaptureStartUs >= POWER_WAKE_STABILITY_MS * 1000U) {
    powerCaptureRequest = PowerWakeRequest::Verified;
  }
  powerCaptureDown = pressed;
}

void IRAM_ATTR HalGPIO::capturePowerWake(void* context) {
  auto* self = static_cast<HalGPIO*>(context);
  portENTER_CRITICAL_ISR(&self->powerCaptureMux);
  if (self->powerCapturePin >= 0) {
    // Retain an edge even if press/release coalesced before this ISR ran.
    if (self->powerCaptureRequest == PowerWakeRequest::None) self->powerCaptureRequest = PowerWakeRequest::Pressed;
    const bool pressed = gpio_ll_get_level(&GPIO, self->powerCapturePin) == self->powerCaptureLevel;
    if (pressed) self->powerCaptureDown = false;  // A queued edge may include an intervening release.
    self->recordPowerWake(pressed, static_cast<uint32_t>(esp_timer_get_time()));
  }
  portEXIT_CRITICAL_ISR(&self->powerCaptureMux);
}

void HalGPIO::verifyPowerWake(TimerHandle_t timer) {
  static_cast<HalGPIO*>(pvTimerGetTimerID(timer))->powerWakeRequest();
}

bool HalGPIO::beginPowerWakeCapture() {
  endPowerWakeCapture();
#if FREEINK_DEVICE_PAPERMONO
  uint8_t status = 0;
  if (!freeink::papermono::ensureBooted() || !freeink::m5pm1::readButtonStatus(&status)) {
    LOG_ERR("GPIO", "PMIC Power capture unavailable");
    return false;
  }
  powerCapturePmic = true;
  // PaperMono accepts the PMIC click; it has no GPIO hold-verification policy.
  if (status & (freeink::m5pm1::BTN_EVENT | freeink::m5pm1::BTN_PRESSED))
    powerCaptureRequest = PowerWakeRequest::Verified;
  return true;
#endif
  const int8_t pin = BoardConfig::ACTIVE.input.power;
  if (pin < 0) return false;
  if (!powerVerifyTimer) {
    constexpr TickType_t pollTicks = pdMS_TO_TICKS(POWER_WAKE_STABILITY_MS / 2);
    powerVerifyTimer =
        xTimerCreateStatic("PowerWake", pollTicks ? pollTicks : 1, pdTRUE, this, verifyPowerWake, &powerVerifyStorage);
    if (!powerVerifyTimer) {
      LOG_ERR("GPIO", "Power verification timer unavailable");
      return false;
    }
  }
  const auto installed = gpio_install_isr_service(0);
  if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
    LOG_ERR("GPIO", "Power capture service failed: %d", static_cast<int>(installed));
    return false;
  }
  const auto configured = gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_ANYEDGE);
  if (configured != ESP_OK) {
    LOG_ERR("GPIO", "Power capture edge failed: %d", static_cast<int>(configured));
    return false;
  }
  powerCapturePin = pin;
  powerCaptureLevel = BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  const auto attached = gpio_isr_handler_add(static_cast<gpio_num_t>(pin), capturePowerWake, this);
  if (attached != ESP_OK) {
    LOG_ERR("GPIO", "Power capture handler failed: %d", static_cast<int>(attached));
    gpio_intr_disable(static_cast<gpio_num_t>(pin));
    powerCapturePin = -1;
    return false;
  }
  powerWakeRequest();  // Include a button already down when capture starts.
  // The existing timer daemon samples a held level while main is blocked in I/O.
  if (xTimerStart(powerVerifyTimer, 0) != pdPASS) {
    LOG_ERR("GPIO", "Power verification timer start failed");
    endPowerWakeCapture();
    return false;
  }
  return true;
}

HalGPIO::PowerWakeRequest HalGPIO::powerWakeRequest() {
#if FREEINK_DEVICE_PAPERMONO
  if (powerCapturePmic) {
    if (powerCaptureRequest != PowerWakeRequest::None) return powerCaptureRequest;
    uint8_t status = 0;
    if (!freeink::m5pm1::readButtonStatus(&status)) {
      LOG_ERR("GPIO", "PMIC Power read failed; leaving timer operation");
      powerCaptureRequest = PowerWakeRequest::Verified;
    } else if (status & (freeink::m5pm1::BTN_EVENT | freeink::m5pm1::BTN_PRESSED)) {
      // The read-clear event survives a released click during blocking I/O.
      powerCaptureRequest = PowerWakeRequest::Verified;
    }
    return powerCaptureRequest;
  }
#endif
  portENTER_CRITICAL(&powerCaptureMux);
  if (powerCapturePin >= 0) {
    recordPowerWake(gpio_ll_get_level(&GPIO, powerCapturePin) == powerCaptureLevel,
                    static_cast<uint32_t>(esp_timer_get_time()));
  }
  const auto request = powerCaptureRequest;
  portEXIT_CRITICAL(&powerCaptureMux);
  return request;
}

void HalGPIO::endPowerWakeCapture() {
#if FREEINK_DEVICE_PAPERMONO
  powerCapturePmic = false;
#endif
  // A queued callback is harmless after the pin is cleared; storage lives with gpio.
  if (powerVerifyTimer) xTimerStop(powerVerifyTimer, 0);
  if (powerCapturePin >= 0) {
    gpio_intr_disable(static_cast<gpio_num_t>(powerCapturePin));
    gpio_isr_handler_remove(static_cast<gpio_num_t>(powerCapturePin));
  }
  portENTER_CRITICAL(&powerCaptureMux);
  powerCapturePin = -1;
  powerCaptureRequest = PowerWakeRequest::None;
  powerCaptureDown = false;
  portEXIT_CRITICAL(&powerCaptureMux);
}

bool HalGPIO::verifyPowerButtonWakeup() {
  // M5Paper v1.1: the classic ESP32's reset-to-setup() latency exceeds a normal
  // wheel click, so a click wake is always released before this samples and
  // verification would re-sleep on every wake. Its wheel has hard external
  // pull-ups, so the ghost-wake debounce this implements is not needed.
  if (BoardConfig::isPaperMono() || BoardConfig::isM5PaperV11() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }

  const bool heldAtFirstSample = inputMgr.isPowerButtonPhysicallyPressed();
  const unsigned long sampleStart = millis();
  inputMgr.update();
  while (millis() - sampleStart < POWER_WAKE_STABILITY_MS || inputMgr.isDebouncePending()) {
    delay(1);
    inputMgr.update();
  }
  return heldAtFirstSample && inputMgr.isPowerButtonPhysicallyPressed();
}

bool HalGPIO::isUsbConnected() const {
  if (!deviceIsX3() && BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  // X3 uses GPIO20 for I2C, not USB detection. Boards without a digital
  // USB-detect line (e.g. Sticky, whose PWR_IN_VOLT is an analog
  // divider): infer external power from charging state instead. BatteryMonitor
  // picks the board's best source — charger IC status, gauge Current() sign, or
  // a /STAT pin — and reports false on boards with no battery telemetry at all.
  // A charger IC that reports its input rail (BQ25896, SGM41562, M5 PMIC)
  // answers directly; otherwise infer from charging state, where charge
  // termination at 100% reads as "not connected".
  static const BatteryMonitor battery;
  bool known = false;
  const bool present = battery.isExternalPowerPresent(&known);
  if (known) return present;
  return battery.isCharging();
}

bool HalGPIO::coldBootImpliesPowerButton() const {
  // Xteink-style power topology: the power button energizes the rail until
  // firmware latches it, so a no-USB POWERON can only be a still-held button
  // boot, and plugging USB into an off device should charge-sleep, not boot.
  // Everything else boots on any cold boot: boards with no USB detection at
  // all (M5Paper v1.1, PaperColor, Murphy, de-link) would misread USB and
  // post-flash boots as battery button boots, and STAT-only boards like the
  // EEGO A4 misread them the same way once the charger terminates at 100%
  // (STAT inactive reads as "no USB").
  // The Metalio sleeps powered off and only its button turns it back on.
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky() || BoardConfig::isMetalioEInk4();
}

// Guard the wake/reset value pinning in WakeupClassify.h against IDF drift.
static_assert(ESP_SLEEP_WAKEUP_UNDEFINED == wakeup::WAKEUP_UNDEFINED,
              "wake-cause values drifted from WakeupClassify.h");
static_assert(ESP_SLEEP_WAKEUP_EXT1 == wakeup::WAKEUP_EXT1, "wake-cause values drifted from WakeupClassify.h");
static_assert(ESP_SLEEP_WAKEUP_TIMER == wakeup::WAKEUP_TIMER, "wake-cause values drifted from WakeupClassify.h");
static_assert(ESP_SLEEP_WAKEUP_GPIO == wakeup::WAKEUP_GPIO, "wake-cause values drifted from WakeupClassify.h");
static_assert(ESP_RST_UNKNOWN == wakeup::RST_UNKNOWN, "reset-reason values drifted from WakeupClassify.h");
static_assert(ESP_RST_POWERON == wakeup::RST_POWERON, "reset-reason values drifted from WakeupClassify.h");
static_assert(ESP_RST_SW == wakeup::RST_SW, "reset-reason values drifted from WakeupClassify.h");
static_assert(ESP_RST_DEEPSLEEP == wakeup::RST_DEEPSLEEP, "reset-reason values drifted from WakeupClassify.h");

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
#if FREEINK_DEVICE_PAPERMONO
  static const bool rtcTimerWake = [] {
    bool fired = false;
    if (!papermonotimer::takeWake(fired)) {
      LOG_ERR("GPIO", "RTC wake state unavailable or cleanup failed");
      return false;
    }
    return fired;
  }();
  if (rtcTimerWake && esp_reset_reason() == ESP_RST_POWERON &&
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED)
    return WakeupReason::Timer;
#endif
  return wakeup::classify(esp_sleep_get_wakeup_cause(), esp_reset_reason(), isUsbConnected(),
                          coldBootImpliesPowerButton());
}
