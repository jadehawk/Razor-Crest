#include "HomeMenuButtonsActivity.h"

#include <BoardConfig.h>
#include <I18n.h>
#include <Memory.h>

#include <utility>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
constexpr StrId kChoices[] = {StrId::STR_HOME_SHOW, StrId::STR_HOME_HIDE, StrId::STR_HOME_MOVE_UP,
                              StrId::STR_HOME_MOVE_DOWN};
}

void HomeMenuButtonsActivity::onEnter() {
  UiListActivity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int rowHeight = GUI.getMenuRowHeight(renderer);
  const int capacity = HomeMenuRegistry::capacityFor(
      renderer, top, rowHeight, metrics.menuSpacing, BoardConfig::hasTouch() ? 0 : metrics.buttonHintsHeight,
      UITheme::getInstance().activeExternalScene() ? (BoardConfig::hasTouch() ? 80 : 46) : 0);
  registry.load(capacity, SETTINGS.pluginsEnabled != 0, OPDS_STORE.hasServers());
}

bool HomeMenuButtonsActivity::handleCustomInput() {
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void HomeMenuButtonsActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void HomeMenuButtonsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  rows.resize(listCount());
  const auto& entries = registry.all();
  const bool full = registry.visible().size() >= static_cast<size_t>(registry.capacity());
  for (size_t i = 0; i < entries.size(); ++i) {
    rows[i].label = entries[i].label.c_str();
    rows[i].value = entries[i].target == HomeMenuRegistry::Target::Settings ? I18N.get(StrId::STR_HOME_REQUIRED)
                    : entries[i].enabled                                    ? I18N.get(StrId::STR_HOME_VISIBLE)
                                         : I18N.get(full ? StrId::STR_HOME_NO_ROOM : StrId::STR_HOME_HIDDEN);
    rows[i].actionValue = static_cast<int16_t>(i);
  }
  rows.back().label = I18N.get(StrId::STR_HOME_RESTORE_DEFAULTS);
  rows.back().value = "";
  rows.back().actionValue = static_cast<int16_t>(entries.size());
  fui::ListProps props;
  props.items = rows.data();
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void HomeMenuButtonsActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto& entries = registry.all();
  if (index == static_cast<int>(entries.size())) {
    if (registry.reset()) {
      onEnter();
      requestUpdate();
    }
    return;
  }
  const auto& entry = entries[index];
  if (entry.target == HomeMenuRegistry::Target::Settings) return;
  const std::string id = entry.id;
  popup.show(StrId::STR_HOME_MENU_BUTTONS, kChoices, 4, entry.enabled ? 0 : 1, [this, id](const int choice) {
    if (choice == 0)
      registry.setEnabled(id, true);
    else if (choice == 1)
      registry.setEnabled(id, false);
    else if (choice == 2)
      registry.move(id, -1);
    else if (choice == 3)
      registry.move(id, +1);
    requestUpdate();
  });
  requestUpdate();
}
