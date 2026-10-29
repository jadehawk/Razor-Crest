#pragma once

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Display > Continue Reading. Both controls live on a dedicated page so the
// main Display list keeps one simple navigation entry.
class ContinueReadingSettingsActivity final : public UiListActivity {
 public:
  ContinueReadingSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("ContinueReadingSettings", renderer, input) {}

 private:
  static constexpr int ROW_COUNT = 2;
  freeink::ui::ListItem rows[ROW_COUNT]{};
  OptionPopup popup;
  std::string limitLabel;

  int listCount() const override { return ROW_COUNT; }
  const char* headerTitle() const override { return tr(STR_CONTINUE_READING); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&& lock) override;
};
