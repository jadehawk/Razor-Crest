#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller, touch, frontlight, RTC, IMU), firmware version, MAC address, and the SD card's size, used and free space.
// The About list opens instantly; SD usage is calculated only in Storage Details.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 13;
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
