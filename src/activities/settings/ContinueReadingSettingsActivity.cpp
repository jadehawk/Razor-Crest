#include "ContinueReadingSettingsActivity.h"

#include <HalMemory.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "util/RecentBookLimits.h"

namespace fui = freeink::ui;

namespace {
constexpr uint8_t OPTIONS[] = {0, 5, 10, 20};
constexpr int NON_PSRAM_COUNT = 3;
constexpr int PSRAM_COUNT = 4;
bool hasPsram() { return HalMemory::getPsramHeap().totalBytes > 0; }
int optionCount() { return hasPsram() ? PSRAM_COUNT : NON_PSRAM_COUNT; }
int selectedOption() {
  const uint8_t current = recentbooklimits::effective(SETTINGS.continueReadingBookLimit, hasPsram());
  if (SETTINGS.continueReadingBookLimit == 0) return 0;
  for (int i = 1; i < optionCount(); ++i)
    if (OPTIONS[i] == current) return i;
  return 0;
}
}  // namespace

bool ContinueReadingSettingsActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void ContinueReadingSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= ROW_COUNT || popup.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;
  if (index == 0) {
    SETTINGS.continueReadingCarousel = SETTINGS.continueReadingCarousel ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  std::vector<std::string> labels;
  labels.reserve(optionCount());
  labels.emplace_back("Auto (" + std::to_string(recentbooklimits::deviceMaximum(hasPsram())) + ")");
  for (int i = 1; i < optionCount(); ++i) labels.emplace_back(std::to_string(OPTIONS[i]));
  popup.show(StrId::STR_CONTINUE_READING_BOOK_LIMIT, labels, selectedOption(), [this](const int selected) {
    if (selected < 0 || selected >= optionCount()) return;
    SETTINGS.continueReadingBookLimit = OPTIONS[selected];
    SETTINGS.saveToFile();
    requestUpdate();
  });
  requestUpdate();
}

void ContinueReadingSettingsActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void ContinueReadingSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rows[0].label = tr(STR_CONTINUE_READING_CAROUSEL);
  rows[0].actionValue = 0;
  GUI.setCheckboxRow(rows[0], SETTINGS.continueReadingCarousel != 0);
  rows[1].label = tr(STR_CONTINUE_READING_BOOK_LIMIT);
  rows[1].actionValue = 1;
  const uint8_t count = recentbooklimits::effective(SETTINGS.continueReadingBookLimit, hasPsram());
  limitLabel = SETTINGS.continueReadingBookLimit == 0 ? "Auto (" + std::to_string(count) + ")" : std::to_string(count);
  rows[1].value = limitLabel.c_str();

  fui::ListProps props;
  props.items = rows;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
