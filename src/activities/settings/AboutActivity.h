#pragma once
#include <BoardConfig.h>

#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller and Picco panel, touch, frontlight, RTC, IMU), firmware
// version, MAC address, EEGO UID, and SD card usage. The About list opens
// instantly; SD usage is calculated only when Storage Details is selected.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Picco adds a live panel row; EEGO A4 adds a UID row.
  static constexpr int ITEM_COUNT = 13 + FREEINK_DEVICE_PICCO + FREEINK_DEVICE_EEGO_A4;
  static constexpr int DETAIL_COUNT = 2;

  void onEnter() override;

 private:
  int listCount() const override { return showingStorageDetails_ ? DETAIL_COUNT : ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;

  std::string rowValues_[ITEM_COUNT];
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
  std::string detailValues_[DETAIL_COUNT];
  freeink::ui::ListItem detailItems_[DETAIL_COUNT]{};
  bool showingStorageDetails_ = false;
  bool sdUsageCalculated_ = false;
};
