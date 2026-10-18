#include <PaperMonoTimer.h>
#include <RecordingRtcBus.h>
#include <WakeupClassify.h>

#include <cstdlib>

static int resetReason = wakeup::RST_POWERON;
static int wakeCause = wakeup::WAKEUP_UNDEFINED;
static bool usb = false;
[[maybe_unused]] constexpr int ESP_RST_POWERON = wakeup::RST_POWERON;
[[maybe_unused]] constexpr int ESP_SLEEP_WAKEUP_UNDEFINED = wakeup::WAKEUP_UNDEFINED;
int esp_reset_reason() { return resetReason; }
int esp_sleep_get_wakeup_cause() { return wakeCause; }
#define LOG_ERR(...) ((void)0)
struct HalGPIO {
  using WakeupReason = wakeup::Reason;
  bool isUsbConnected() const { return usb; }
  bool coldBootImpliesPowerButton() const { return true; }
  WakeupReason getWakeupReason() const;
};
#include "WakeSource.inc"

int main(int argc, char** argv) {
  assert(argc == 2 || argc == 3);
  const int mode = std::atoi(argv[1]);
  reset();
  regs[0x6E][0x05] = mode == 1 ? 0x24 : mode == 2 ? 0x22 : 0x20;
  regs[0x6E][0x18] = mode == 3 ? 0x11 : 0x01;
  regs[0x32][0x1D] = mode == 4 ? 0 : mode == 5 ? 0x12 : 0x10;
  regs[0x32][0x1C] = 0x12;
  regs[0x32][0x1E] = 0x10;
  if (mode == 6) deadBus = true;
  usb = mode == 2;
  if (mode >= 7 && mode <= 10) resetReason = mode == 7 ? 3 : mode == 8 ? 9 : mode == 9 ? 5 : 4;
  if (mode == 11) wakeCause = wakeup::WAKEUP_EXT1;
  if (mode == 12) regs[0x6E][0x05] = 0;
  HalGPIO gpio;
  const auto expected = mode == 0                 ? wakeup::Reason::Timer
                        : mode == 2               ? wakeup::Reason::AfterUSBPower
                        : mode >= 7 && mode <= 11 ? wakeup::Reason::Other
                                                  : wakeup::Reason::PowerButton;
  const auto actual = gpio.getWakeupReason();
  if (argc == 3) return actual != expected ? 42 : 0;
  assert(actual == expected);
  const int afterFirstQuery = operations;
  assert(gpio.getWakeupReason() == actual && operations == afterFirstQuery);
  if (mode == 6) return 0;
  assert(!(regs[0x32][0x1C] & 0x10) && !(regs[0x32][0x1E] & 0x10));
  assert(!(regs[0x6E][0x18] & 1));
  reset();
  assert(papermonotimer::arm(60000000ULL));
  assert((regs[0x32][0x1C] & 0x10) && regs[0x6E][0x18] == 1);
  const int armedOperations = operations;
  assert(gpio.getWakeupReason() == actual && operations == armedOperations);
  assert(papermonotimer::arm(0));
  assert(!(regs[0x32][0x1C] & 0x10) && regs[0x6E][0x18] == 0);
  assert(!papermonotimer::arm(60000001ULL));
  assert(papermonotimer::arm(65536ULL * 1000000ULL));
  assert(regs[0x32][0x1A] == 0x45 && regs[0x32][0x1B] == 0x04);
  assert(!papermonotimer::arm(3932101ULL * 1000000ULL));
  assert(!(regs[0x32][0x1C] & 0x10) && !(regs[0x6E][0x18] & 1));
  regs[0x6E][0x18] = 0x10;
  regs[0x6E][0x07] = 0x20;
  assert(!papermonotimer::arm(60000000ULL));
  assert(regs[0x6E][0x18] == 0x10 && !(regs[0x32][0x1C] & 0x10));
  assert(regs[0x6E][0x07] == 0x20);
  reset();
  dropCounter = true;
  assert(!papermonotimer::arm(60000000ULL));
  assert(!(regs[0x32][0x1C] & 0x10) && regs[0x6E][0x18] == 0);
}
