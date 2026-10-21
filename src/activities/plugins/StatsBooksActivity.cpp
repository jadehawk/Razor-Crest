#include "StatsBooksActivity.h"

#include <GfxRenderer.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "StatsDashboardActivity.h"
#include "components/UITheme.h"
#include "util/ReadingStatsStore.h"

namespace fui = freeink::ui;

StatsBooksActivity::StatsBooksActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifestPath)
    : UiListActivity("StatsBooks", renderer, input), manifestPath_(std::move(manifestPath)) {}

void StatsBooksActivity::onEnter() {
  UiListActivity::onEnter();
  total_ = readingstats::bookCount();
  loadPage();
  requestUpdate();
}

void StatsBooksActivity::loadPage() {
  shown_ = 0;
  rowCount_ = 0;
  prev_ = start_ != 0;
  next_ = false;
  if (prev_) {
    titles_[rowCount_] = "< Previous books";
    values_[rowCount_] = "";
    ++rowCount_;
  }
  firstBookRow_ = rowCount_;
  for (int i = 0; i < kPageSize && start_ + i < total_; ++i) {
    readingstats::BookEntry record{};
    if (!readingstats::bookAt(start_ + i, record)) continue;
    books_[shown_] = record;
    const std::string fallback = record.path[0] ? record.path : "Unnamed Book";
    titles_[rowCount_] = record.title[0] ? record.title : fallback;
    readingstats::Snapshot summary{};
    if (readingstats::loadBookHash(record.hash(), summary)) {
      char detail[52];
      snprintf(detail, sizeof(detail), "%lluh %lum | %lu turns",
               static_cast<unsigned long long>(summary.totalSeconds() / 3600),
               static_cast<unsigned long>((summary.totalSeconds() / 60) % 60),
               static_cast<unsigned long>(summary.pageTurns));
      values_[rowCount_] = detail;
    } else {
      values_[rowCount_] = "No recorded sessions";
    }
    ++shown_;
    ++rowCount_;
  }
  next_ = start_ + kPageSize < total_;
  if (next_) {
    titles_[rowCount_] = "Next books >";
    values_[rowCount_] = "";
    ++rowCount_;
  }
  if (rowCount_ == 0) {
    titles_[0] = "No recorded books";
    values_[0] = "Read a book to begin";
    rowCount_ = 1;
  }
  for (int i = 0; i < rowCount_; ++i) {
    items_[i].label = titles_[i].c_str();
    items_[i].value = values_[i].c_str();
    items_[i].actionValue = static_cast<int16_t>(i);
  }
  activeNav().reset();
}

void StatsBooksActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount_) return;
  if (prev_ && index == 0) {
    start_ -= kPageSize;
    loadPage();
    requestUpdate();
    return;
  }
  if (next_ && index == rowCount_ - 1) {
    start_ += kPageSize;
    loadPage();
    requestUpdate();
    return;
  }
  const int bookRow = index - firstBookRow_;
  if (bookRow < 0 || bookRow >= shown_) return;
  const auto& book = books_[bookRow];
  auto view = makeUniqueNoThrow<StatsDashboardActivity>(renderer, mappedInput, manifestPath_, book);
  if (!view) return;
  app.clearTapFlash();
  startActivityForResult(std::move(view), [](const ActivityResult&) {});
}

void StatsBooksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = items_.data();
  props.count = rowCount_;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.rowHeight = static_cast<int16_t>(std::min<int>(112, screen.theme().rowHeight * 3 / 2));
  syncListViewport(screen, props);
  screen.list(props);
}
