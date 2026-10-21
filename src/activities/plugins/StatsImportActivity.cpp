#include "StatsImportActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"

namespace fui = freeink::ui;

StatsImportActivity::StatsImportActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("StatsImport", renderer, input) {}

void StatsImportActivity::onEnter() {
  UiListActivity::onEnter();
  previews_[0] = readingstats::legacy::inspect(readingstats::legacy::Source::CrossInk);
  previews_[1] = readingstats::legacy::inspect(readingstats::legacy::Source::VCodex);
  status_ =
      "Choose a legacy source above. Native statistics are preserved, and the original legacy files will not be "
      "modified.";
  refreshRows();
  requestUpdate();
}

void StatsImportActivity::refreshRows() {
  labels_[0] = armed_ == 0 ? "Confirm CrossInk import?" : "CrossInk reading history";
  labels_[1] = armed_ == 1 ? "Confirm vCodex import?" : "vCodex reading history";
  for (int i = 0; i < 2; ++i) {
    const auto& preview = previews_[i];
    if (!preview.present)
      values_[i] = "No source file found";
    else if (!preview.supported)
      values_[i] = "Unsupported or damaged source";
    else {
      char value[98];
      snprintf(value, sizeof(value), "%lluh %lum, %lu sessions",
               static_cast<unsigned long long>(preview.totals.totalSeconds / 3600),
               static_cast<unsigned long long>((preview.totals.totalSeconds / 60) % 60),
               static_cast<unsigned long>(preview.totals.sessions));
      values_[i] = value;
      if (i == 1) values_[i] += " | " + std::to_string(preview.books) + " books";
    }
  }
  // Source names and their summaries each get a full-width line. The
  // long status/confirmation text belongs to a separate, non-selectable
  // multiline panel below the list, never the list's value column.
  for (int i = 0; i < 2; ++i) {
    items_[i].label = labels_[i].c_str();
    items_[i].subtitle = values_[i].c_str();
    items_[i].value = nullptr;
    items_[i].actionValue = static_cast<int16_t>(i);
  }
}

void StatsImportActivity::activateIndex(const int index) {
  if (index < 0 || index >= 2) return;
  const auto source = index == 0 ? readingstats::legacy::Source::CrossInk : readingstats::legacy::Source::VCodex;
  if (!previews_[index].supported) {
    status_ = previews_[index].present ? "This source is unsupported or damaged. No statistics were imported."
                                       : "No source file found. No statistics were imported.";
    armed_ = -1;
  } else if (armed_ != index) {
    armed_ = index;
    status_ =
        "Import will add historical reading statistics to the native BIN database. Select the same source again to "
        "confirm, or press Back to cancel.";
  } else {
    // Never import automatically on page entry. The second action is a
    // deliberate confirmation for a potentially long SD-card migration.
    armed_ = -1;
    HalPowerManager::Lock keepDeviceAwake;
    const auto result = readingstats::legacy::importSource(source);
    char message[224];
    if (result.success)
      snprintf(message, sizeof(message),
               "Import complete. %lu new records; %lu already imported. Legacy files were not modified.",
               static_cast<unsigned long>(result.imported), static_cast<unsigned long>(result.alreadyImported));
    else if (result.otherSourceAlreadyImported)
      snprintf(message, sizeof(message),
               "Another legacy source was already imported. This source was blocked to prevent possible "
               "double-counting of reading history.");
    else
      snprintf(message, sizeof(message),
               "Import incomplete (%lu errors). Existing data was preserved; the same source can be retried safely.",
               static_cast<unsigned long>(result.errors));
    status_ = message;
    previews_[0] = readingstats::legacy::inspect(readingstats::legacy::Source::CrossInk);
    previews_[1] = readingstats::legacy::inspect(readingstats::legacy::Source::VCodex);
  }
  refreshRows();
  requestUpdate();
}

void StatsImportActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Keep the two selectable sources in the normal FreeInkUI list (touch,
  // button focus and swipe all use its published interaction table). Cap the
  // list to two rows so it cannot consume the space reserved for the notice.
  fui::ListProps props;
  props.items = items_.data();
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  props.rowHeight = static_cast<int16_t>(std::max<int>(108, std::min<int>(136, screen.theme().rowHeight * 2)));
  props.rowGap = static_cast<int16_t>(std::max<int>(8, screen.theme().listRowGap));
  props.partialTrailingRow = false;
  syncListViewport(screen, props);
  const auto listHeight = static_cast<int16_t>(props.rowHeight * 2 + props.rowGap + 4);
  screen.list(props, listHeight);

  // Status/confirmation is not a third selectable list item and must never
  // use ListItem::value (the SDK value slot renders just one line).
  // The remaining full-width body supports multiple wrapped lines on X3/X4Pro.
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing * 2));
  const auto remaining = screen.contentRect();
  constexpr int kSideInset = 14;
  const int textWidth = remaining.width - 2 * kSideInset;
  if (textWidth <= 0 || remaining.height <= 0) return;
  const auto heading = [&] {
    auto style = screen.theme().smallText;
    style.bold = true;
    return style;
  }();
  const int headingHeight = screen.target().lineHeight(heading.font);
  if (remaining.height <= headingHeight + 12) return;
  const fui::Rect headingRect{static_cast<int16_t>(remaining.x + kSideInset), remaining.y,
                              static_cast<int16_t>(textWidth), static_cast<int16_t>(headingHeight)};
  screen.target().text(headingRect, "Import status", heading);

  auto bodyStyle = screen.theme().bodyText;
  bodyStyle.maxLines = static_cast<uint8_t>(std::min<int>(
      12, (remaining.height - headingHeight - 12) / std::max<int>(1, screen.target().lineHeight(bodyStyle.font))));
  if (bodyStyle.maxLines == 0) return;
  const auto measured =
      fui::measureWrappedText(screen.target(), status_.c_str(), bodyStyle, static_cast<int16_t>(textWidth));
  const int messageHeight = std::min<int>(remaining.height - headingHeight - 12, measured.height);
  if (messageHeight <= 0) return;
  // The Gfx-backed text target vertically centers inside the supplied rect.
  // Give it the measured multi-line height, not the entire empty page.
  const fui::Rect messageRect{static_cast<int16_t>(remaining.x + kSideInset),
                              static_cast<int16_t>(remaining.y + headingHeight + 12), static_cast<int16_t>(textWidth),
                              static_cast<int16_t>(messageHeight)};
  screen.target().text(messageRect, status_.c_str(), bodyStyle);
}
