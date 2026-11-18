#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller, touch, frontlight, RTC, IMU), firmware version, MAC address,
// EEGO UID, and SD card usage. The About list opens instantly; SD usage is
// calculated only in Storage Details.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

#if FREEINK_DEVICE_EEGO_A4
  static constexpr int ITEM_COUNT = 14;
#else
  static constexpr int ITEM_COUNT = 13;
#endif
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
