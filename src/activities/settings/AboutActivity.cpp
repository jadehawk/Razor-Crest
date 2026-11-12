#include "AboutActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <esp_mac.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_DEVICE = 0,
  ITEM_FIRMWARE,
  ITEM_CHIP,
  ITEM_FLASH,
  ITEM_SD_SIZE,
  ITEM_STORAGE_DETAILS,
  ITEM_DISPLAY,
  ITEM_RESOLUTION,
  ITEM_TOUCH,
  ITEM_FRONTLIGHT,
  ITEM_RTC,
  ITEM_IMU,
  ITEM_MAC,
};

// Deliberately hardcoded English, exempt from the tr() rule: support reads
// these screenshots across every device language, so the labels must be
// identical on every unit.
const char* const menuNames[AboutActivity::ITEM_COUNT] = {
    "Device",     "Firmware", "Chip",       "Flash", "SD Size",           "Storage Details", "Display Controller",
    "Resolution", "Touch",    "Frontlight", "RTC",   "Tilt Sensor (IMU)", "MAC Address",
};

// "14.5 GB" / "35 MB": GiB with one decimal, MiB below 1 GiB.
std::string formatBytes(const uint64_t bytes) {
  char buf[24];
  constexpr uint64_t MIB = 1024ull * 1024ull;
  constexpr uint64_t GIB = 1024ull * MIB;
  if (bytes >= GIB) {
    const uint64_t tenths = (bytes * 10 + GIB / 2) / GIB;
    snprintf(buf, sizeof(buf), "%u.%u GB", static_cast<unsigned>(tenths / 10), static_cast<unsigned>(tenths % 10));
  } else {
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>((bytes + MIB / 2) / MIB));
  }
  return buf;
}

// Chip part numbers, not user prose — deliberately untranslated.
const char* displayControllerName(const BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
  }
  return "?";
}

const char* touchControllerName(const BoardConfig::TouchController c) {
  switch (c) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
    case BoardConfig::TouchController::Cst816s:
      return "CST816S";
    case BoardConfig::TouchController::Tma525c:
      return "TMA525C";
  }
  return "?";
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("About", renderer, mappedInput) {}

const char* AboutActivity::headerTitle() const { return showingStorageDetails_ ? "Storage Details" : "About"; }

void AboutActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = menuNames[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  char buf[32];
  // Hardware and firmware information is fixed after boot; fill it once here.
  // BoardConfig::ACTIVE reflects RUNTIME detection: selectDevice() picked the
  // profile and applyXteinkDisplayController() may have promoted the display
  // controller to the panel actually found on the bus.
  rowValues_[ITEM_DEVICE] = BoardConfig::ACTIVE.name;
  rowValues_[ITEM_FIRMWARE] = CROSSPOINT_VERSION;
  snprintf(buf, sizeof(buf), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  rowValues_[ITEM_CHIP] = buf;
  snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  rowValues_[ITEM_FLASH] = buf;
  // The profile names a boot default; boards that probe their panel (Picco,
  // Sticky) can run another driver, and the driver knows which.
  const char* liveController = display.controllerName();
  rowValues_[ITEM_DISPLAY] =
      liveController ? liveController : displayControllerName(BoardConfig::ACTIVE.displayController);
  snprintf(buf, sizeof(buf), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  rowValues_[ITEM_RESOLUTION] = buf;
  const char* touch = touchControllerName(BoardConfig::ACTIVE.touch.controller);
  rowValues_[ITEM_TOUCH] = touch ? touch : "No";
  rowValues_[ITEM_FRONTLIGHT] = Frontlight.present() ? "Yes" : "No";
  rowValues_[ITEM_RTC] = halClock.isAvailable() ? "Yes" : "No";
  rowValues_[ITEM_IMU] = halTiltSensor.isAvailable() ? "Yes" : "No";
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  rowValues_[ITEM_MAC] = buf;

  // SD capacity is cached at mount; calculating used space scans the FAT.
  rowValues_[ITEM_SD_SIZE] = formatBytes(Storage.sdTotalBytes());
  rowValues_[ITEM_STORAGE_DETAILS] = ">";
  detailItems_[0].label = "SD Used";
  detailItems_[1].label = "SD Free";
  for (int i = 0; i < DETAIL_COUNT; ++i) {
    detailItems_[i].actionValue = static_cast<int16_t>(i);
    detailValues_[i].clear();
  }
  showingStorageDetails_ = false;
  sdUsageCalculated_ = false;
}

void AboutActivity::activateIndex(const int index) {
  if (showingStorageDetails_ || index != ITEM_STORAGE_DETAILS) return;

  app.clearTapFlash();
  showingStorageDetails_ = true;
  nav.reset();

  if (!sdUsageCalculated_) {
    // The FAT free-cluster scan may take seconds; show Loading only on request.
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }

    const uint64_t sdTotal = Storage.sdTotalBytes();
    uint64_t sdUsed = 0;
    if (Storage.sdUsedBytes(sdUsed)) {
      detailValues_[0] = formatBytes(sdUsed);
      detailValues_[1] = formatBytes(sdUsed <= sdTotal ? sdTotal - sdUsed : 0);
      sdUsageCalculated_ = true;
    } else {
      detailValues_[0] = detailValues_[1] = "Error.";
    }
  }
  requestUpdate();
}

void AboutActivity::onBackButton() {
  if (!showingStorageDetails_) {
    finish();
    return;
  }
  showingStorageDetails_ = false;
  nav.reset(ITEM_STORAGE_DETAILS);
  requestUpdate();
}

void AboutActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = listCount();
  for (int i = 0; i < count; ++i) {
    if (showingStorageDetails_)
      detailItems_[i].value = detailValues_[i].c_str();
    else
      rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = showingStorageDetails_ ? detailItems_ : rowItems_;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
