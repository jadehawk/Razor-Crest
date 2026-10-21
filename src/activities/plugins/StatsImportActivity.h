#pragma once

#include <array>
#include <string>

#include "activities/UiListActivity.h"
#include "util/ReadingStatsLegacyImport.h"

// Review and explicit two-step confirmation before importing a legacy source.
// The dashboard's sixth page is only a source preview; this screen owns writes.
class StatsImportActivity final : public UiListActivity {
 public:
  StatsImportActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;

 private:
  std::array<freeink::ui::ListItem, 2> items_{};
  std::array<std::string, 2> labels_{};
  std::array<std::string, 2> values_{};
  readingstats::legacy::Preview previews_[2]{};
  int armed_ = -1;
  std::string status_;

  int listCount() const override { return 2; }
  const char* headerTitle() const override { return "Import Statistics"; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void refreshRows();
};
