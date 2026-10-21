#pragma once

#include <array>
#include <string>

#include "activities/UiListActivity.h"
#include "util/ReadingStatsBookIndex.h"

// Reusable native book-statistics list host. The calling plugin chooses when
// to open it; no firmware home/menu entry or plugin-specific screen is added.
class StatsBooksActivity final : public UiListActivity {
 public:
  StatsBooksActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifestPath);
  void onEnter() override;

 private:
  static constexpr int kPageSize = 7;
  static constexpr int kMaxRows = kPageSize + 2;
  std::array<freeink::ui::ListItem, kMaxRows> items_{};
  std::array<std::string, kMaxRows> titles_{};
  std::array<std::string, kMaxRows> values_{};
  std::array<readingstats::BookEntry, kPageSize> books_{};
  std::string manifestPath_;
  uint32_t total_ = 0;
  uint32_t start_ = 0;
  int shown_ = 0;
  int rowCount_ = 0;
  int firstBookRow_ = 0;
  bool prev_ = false;
  bool next_ = false;

  int listCount() const override { return rowCount_; }
  const char* headerTitle() const override { return "Reading Books"; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void loadPage();
};
