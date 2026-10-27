#pragma once

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/HomeMenuRegistry.h"

class HomeMenuButtonsActivity final : public UiListActivity {
 public:
  HomeMenuButtonsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("HomeMenuButtons", renderer, input), registry() {}
  void onEnter() override;

 private:
  HomeMenuRegistry registry;
  std::vector<freeink::ui::ListItem> rows;
  OptionPopup popup;
  int listCount() const override { return static_cast<int>(registry.all().size()) + 1; }
  const char* headerTitle() const override { return tr(STR_HOME_MENU_BUTTONS); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void render(RenderLock&& lock) override;
};
