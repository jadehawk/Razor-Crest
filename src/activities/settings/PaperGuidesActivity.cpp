#include "PaperGuidesActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId kStyles[] = {StrId::STR_STATE_OFF, StrId::STR_RULED_PAPER, StrId::STR_GRAPH_PAPER};
constexpr StrId kDarkness[] = {StrId::STR_RULE_DARKNESS_25, StrId::STR_RULE_DARKNESS_50, StrId::STR_RULE_DARKNESS_75,
                               StrId::STR_RULE_DARKNESS_100};
}  // namespace

void PaperGuidesActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void PaperGuidesActivity::rebuildRows() {
  const int previous = nav.selected;
  rowCount_ = 0;
  const auto add = [this](Row row, StrId label) {
    const int index = rowCount_++;
    rows_[index] = {};
    rows_[index].label = I18N.get(label);
    rows_[index].actionValue = static_cast<int16_t>(index);
    rowTypes_[index] = row;
  };

  if (page_ == Page::Decorators) {
    add(Row::Mode, StrId::STR_PAPER_GUIDES);
  } else {
    add(Row::Mode, StrId::STR_PAPER_STYLE);
    if (SETTINGS.paperGuidesMode != 0) {
      add(Row::Darkness, StrId::STR_RULE_DARKNESS);
      add(Row::Thickness, StrId::STR_RULE_THICKNESS);
      if (SETTINGS.paperGuidesMode == 1)
        add(Row::Position, StrId::STR_RULE_POSITION);
      else
        add(Row::GridSize, StrId::STR_GRAPH_GRID_SIZE);
      add(Row::Inset, StrId::STR_RULE_INSET);
      add(Row::Dotted, StrId::STR_DOTTED_RULES);
      if (SETTINGS.paperGuidesDotted) {
        add(Row::DashLength, StrId::STR_RULE_DASH_LENGTH);
        add(Row::DashGap, StrId::STR_RULE_DASH_GAP);
      }
    }
  }
  nav.selected = std::clamp(previous, 0, rowCount_ - 1);
}

std::string PaperGuidesActivity::valueFor(const Row row) const {
  char value[20];
  switch (row) {
    case Row::Mode:
      return I18N.get(kStyles[std::min<int>(SETTINGS.paperGuidesMode, 2)]);
    case Row::Darkness:
      return I18N.get(kDarkness[std::min<int>(SETTINGS.paperGuidesDarkness, 3)]);
    case Row::Thickness:
      return std::to_string(SETTINGS.paperGuidesThickness) + " px";
    case Row::Position:
      snprintf(value, sizeof(value), "%+d px", std::clamp<int>(SETTINGS.paperGuidesOffset, 0, 10) - 5);
      return value;
    case Row::GridSize:
      return std::to_string(SETTINGS.paperGuidesGridSize) + " px";
    case Row::Inset:
      return std::to_string(SETTINGS.paperGuidesInset) + " px";
    case Row::Dotted:
      return "";
    case Row::DashLength:
      return std::to_string(SETTINGS.paperGuidesDashLength) + " px";
    case Row::DashGap:
      return std::to_string(SETTINGS.paperGuidesGap) + " px";
  }
  return "";
}

void PaperGuidesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < rowCount_; ++i) {
    values_[i] = valueFor(rowTypes_[i]);
    rows_[i].value = values_[i].empty() ? nullptr : values_[i].c_str();
    if (page_ == Page::Guides && rowTypes_[i] == Row::Dotted)
      GUI.setCheckboxRow(rows_[i], SETTINGS.paperGuidesDotted != 0);
  }

  fui::ListProps props;
  props.items = rows_;
  props.count = static_cast<uint16_t>(rowCount_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void PaperGuidesActivity::showPixels(StrId title, uint8_t CrossPointSettings::* field, int min, int max, int step,
                                     int bias) {
  std::vector<std::string> options;
  options.reserve((max - min) / step + 1);
  for (int v = min; v <= max; v += step) {
    char label[20];
    snprintf(label, sizeof(label), bias ? "%+d px" : "%d px", v - bias);
    options.emplace_back(label);
  }
  const int current = (std::clamp<int>(SETTINGS.*field, min, max) - min) / step;
  popup_.show(title, options, current, [this, field, min, step](int index) {
    SETTINGS.*field = static_cast<uint8_t>(min + index * step);
    SETTINGS.saveToFile();
    requestUpdate();
  });
}

void PaperGuidesActivity::activateIndex(int index) {
  if (index < 0 || index >= rowCount_ || popup_.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;

  if (page_ == Page::Decorators) {
    auto child = makeUniqueNoThrow<PaperGuidesActivity>(renderer, mappedInput, Page::Guides);
    if (!child) {
      LOG_ERR("GUIDES", "OOM: Paper Guides settings");
      return;
    }
    startActivityForResult(std::move(child), [this](const ActivityResult&) { requestUpdate(); });
    return;
  }

  switch (rowTypes_[index]) {
    case Row::Mode:
      popup_.show(StrId::STR_PAPER_STYLE, kStyles, static_cast<int>(std::size(kStyles)),
                  std::min<int>(SETTINGS.paperGuidesMode, 2), [this](int choice) {
                    SETTINGS.paperGuidesMode = static_cast<uint8_t>(choice);
                    SETTINGS.saveToFile();
                    rebuildRows();
                    requestUpdate();
                  });
      break;
    case Row::Darkness:
      popup_.show(StrId::STR_RULE_DARKNESS, kDarkness, static_cast<int>(std::size(kDarkness)),
                  std::min<int>(SETTINGS.paperGuidesDarkness, 3), [this](int choice) {
                    SETTINGS.paperGuidesDarkness = static_cast<uint8_t>(choice);
                    SETTINGS.saveToFile();
                    requestUpdate();
                  });
      break;
    case Row::Thickness:
      showPixels(StrId::STR_RULE_THICKNESS, &CrossPointSettings::paperGuidesThickness, 1, 4, 1);
      break;
    case Row::Position:
      showPixels(StrId::STR_RULE_POSITION, &CrossPointSettings::paperGuidesOffset, 0, 10, 1, 5);
      break;
    case Row::GridSize:
      showPixels(StrId::STR_GRAPH_GRID_SIZE, &CrossPointSettings::paperGuidesGridSize, 16, 32, 4);
      break;
    case Row::Inset:
      showPixels(StrId::STR_RULE_INSET, &CrossPointSettings::paperGuidesInset, 0, 20, 5);
      break;
    case Row::Dotted:
      SETTINGS.paperGuidesDotted = !SETTINGS.paperGuidesDotted;
      SETTINGS.saveToFile();
      rebuildRows();
      break;
    case Row::DashLength:
      showPixels(StrId::STR_RULE_DASH_LENGTH, &CrossPointSettings::paperGuidesDashLength, 1, 16, 1);
      break;
    case Row::DashGap:
      showPixels(StrId::STR_RULE_DASH_GAP, &CrossPointSettings::paperGuidesGap, 1, 16, 1);
      break;
  }
  requestUpdate();
}

bool PaperGuidesActivity::handleCustomInput() {
  return popup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void PaperGuidesActivity::render(RenderLock&& lock) {
  if (popup_.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
