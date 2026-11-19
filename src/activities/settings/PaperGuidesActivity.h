#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The Decorators menu and its Paper Guides subpage share one list controller.
class PaperGuidesActivity final : public UiListActivity {
 public:
  enum class Page : uint8_t { Decorators, Guides };

  PaperGuidesActivity(GfxRenderer& renderer, MappedInputManager& input, Page page)
      : UiListActivity("PaperGuides", renderer, input), page_(page) {}

  void onEnter() override;
  void render(RenderLock&& lock) override;

 private:
  enum class Row : uint8_t { Mode, Darkness, Thickness, Position, GridSize, Inset, Dotted, DashLength, DashGap };
  // Position and Grid Size are mutually exclusive: at most eight visible rows.
  static constexpr int kMaxRows = 8;

  Page page_;
  OptionPopup popup_;
  int rowCount_ = 0;
  Row rowTypes_[kMaxRows]{};
  freeink::ui::ListItem rows_[kMaxRows]{};
  std::string values_[kMaxRows];

  void rebuildRows();
  void showPixels(StrId title, uint8_t CrossPointSettings::* field, int min, int max, int step, int bias = 0);
  std::string valueFor(Row row) const;
  int listCount() const override { return rowCount_; }
  const char* headerTitle() const override {
    return page_ == Page::Decorators ? tr(STR_DECORATORS) : tr(STR_PAPER_GUIDES);
  }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
};
