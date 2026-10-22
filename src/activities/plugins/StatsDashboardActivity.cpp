#include "StatsDashboardActivity.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "StatsBooksActivity.h"
#include "StatsImportActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReadingStatsStore.h"

namespace {
constexpr int kMargin = 12;
constexpr int kGap = 7;
constexpr size_t kManifestLimit = 8192;

std::string duration(uint64_t seconds) {
  char buffer[30];
  if (seconds >= 3600)
    snprintf(buffer, sizeof(buffer), "%lluh %llum", static_cast<unsigned long long>(seconds / 3600),
             static_cast<unsigned long long>((seconds / 60) % 60));
  else
    snprintf(buffer, sizeof(buffer), "%llum", static_cast<unsigned long long>(seconds / 60));
  return buffer;
}

bool knownMetric(const std::string& field) {
  return field == "total_seconds" || field == "sessions" || field == "qualified_turns" ||
         field == "latest_day_seconds" || field == "last_7_seconds" || field == "active_days" ||
         field == "seconds_per_turn";
}
}  // namespace

bool StatsDashboardActivity::supportsManifest(const std::string& path) {
  if (path.empty()) return false;
  std::string raw;
  if (!Storage.readFileToString("STATS", path, kManifestLimit, raw)) return false;
  JsonDocument filter;
  filter["dashboard"]["source"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  const char* source = doc["dashboard"]["source"] | "";
  return strcmp(source, "reading-stats.v1") == 0;
}

StatsDashboardActivity::StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest)
    : Activity("PluginDashboard", renderer, input), manifestPath(std::move(manifest)) {}

StatsDashboardActivity::StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest,
                                               readingstats::BookEntry selected)
    : Activity("PluginDashboard", renderer, input),
      manifestPath(std::move(manifest)),
      bookInfo(std::move(selected)),
      selectedBook(true),
      hasBookInfo(true) {}

bool StatsDashboardActivity::loadManifest() {
  std::string raw;
  if (!Storage.readFileToString("STATS", manifestPath, kManifestLimit, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  const char* source = doc["dashboard"]["source"] | "";
  if (strcmp(source, "reading-stats.v1") != 0) return false;
  const char* header = doc["title"] | "Dashboard";
  title = header;
  const JsonArrayConst pageArray = doc["dashboard"]["pages"].as<JsonArrayConst>();
  if (pageArray.isNull() || pageArray.size() == 0 || pageArray.size() > pages.size()) return false;

  for (const JsonVariantConst pageJson : pageArray) {
    const JsonArrayConst widgets = pageJson["widgets"].as<JsonArrayConst>();
    if (widgets.isNull() || widgets.size() == 0 || widgets.size() > pages[0].widgets.size()) return false;
    Page& page = pages[pageCount++];
    page.title = pageJson["title"] | "Reading";
    for (const JsonVariantConst widgetJson : widgets) {
      Widget& widget = page.widgets[page.count++];
      widget.type = widgetJson["type"] | "";
      widget.field = widgetJson["field"] | "";
      widget.label = widgetJson["label"] | "";
      const char* scope = widgetJson["scope"] | "global";
      if (strcmp(scope, "global") != 0 && strcmp(scope, "current-book") != 0) return false;
      widget.currentBook = strcmp(scope, "current-book") == 0;
      if (widget.type == "metric") {
        if (!knownMetric(widget.field)) return false;
      } else if (widget.type != "week_bars" && widget.type != "heatmap_28" && widget.type != "book_list" &&
                 widget.type != "legacy_import") {
        return false;
      }
    }
  }
  return true;
}

void StatsDashboardActivity::resolveBook() {
  if (selectedBook) return;
  hasBookInfo = false;
  bookInfo = {};
  if (APP_STATE.openEpubPath.empty()) return;
  const std::string& path = APP_STATE.openEpubPath;
  for (uint32_t i = 0, total = readingstats::bookCount(); i < total; ++i) {
    readingstats::BookEntry entry{};
    if (readingstats::bookAt(i, entry) && path == entry.path) {
      bookInfo = entry;
      hasBookInfo = true;
      return;
    }
  }
  // A book opened before this statistics version may not yet have an index entry.
  snprintf(bookInfo.path, sizeof(bookInfo.path), "%s", path.c_str());
  const size_t split = path.find_last_of("/\\");
  const std::string fallback = split == std::string::npos ? path : path.substr(split + 1);
  snprintf(bookInfo.title, sizeof(bookInfo.title), "%s", fallback.c_str());
  hasBookInfo = true;
}

void StatsDashboardActivity::loadSeries() {
  seriesName.clear();
  seriesIndex.clear();
  if (!hasBookInfo || !bookInfo.path[0]) return;
  const std::string path = bookInfo.path;
  if (path.size() < 5 || path.substr(path.size() - 5) != ".epub") return;
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) return;
  Epub::SyncMetadata metadata;
  if (!epub->loadSyncMetadata(metadata)) return;
  seriesName = std::move(metadata.series);
  if (metadata.seriesIndex) {
    char value[24];
    snprintf(value, sizeof(value), "%.2f", static_cast<double>(*metadata.seriesIndex));
    seriesIndex = value;
    while (!seriesIndex.empty() && seriesIndex.back() == '0') seriesIndex.pop_back();
    if (!seriesIndex.empty() && seriesIndex.back() == '.') seriesIndex.pop_back();
  }
}

void StatsDashboardActivity::refresh() {
  recordedBookCount = readingstats::bookCount();
  statsAvailable = readingstats::loadGlobal(stats);
  bookStatsAvailable =
      hasBookInfo && (bookInfo.hash() != 0 ? readingstats::loadBookHash(bookInfo.hash(), bookStats)
                                           : readingstats::loadBook(APP_STATE.openEpubPath.c_str(), bookStats));
}

void StatsDashboardActivity::onEnter() {
  Activity::onEnter();
  if (!loadManifest()) {
    LOG_ERR("STATS", "Invalid local dashboard plugin: %s", manifestPath.c_str());
    finish();
    return;
  }
  if (selectedBook) {
    for (uint8_t i = 0; i < pageCount; ++i) {
      bool bookPage = false;
      for (uint8_t j = 0; j < pages[i].count; ++j)
        if (pages[i].widgets[j].currentBook) bookPage = true;
      if (bookPage) {
        pages[0] = std::move(pages[i]);
        pages[0].title = "Book Statistics";
        pageCount = 1;
        break;
      }
    }
  }
  resolveBook();
  loadSeries();
  refresh();
  requestUpdate();
}

bool StatsDashboardActivity::booksPage() const {
  if (activePage >= pageCount) return false;
  const auto& page = pages[activePage];
  for (int i = 0; i < page.count; ++i)
    if (page.widgets[i].type == "book_list") return true;
  return false;
}

bool StatsDashboardActivity::importPage() const {
  if (selectedBook || activePage >= pageCount) return false;
  const auto& page = pages[activePage];
  for (uint8_t i = 0; i < page.count; ++i)
    if (page.widgets[i].type == "legacy_import") return true;
  return false;
}

void StatsDashboardActivity::refreshLegacyPreviews() {
  crossInkPreview = readingstats::legacy::inspect(readingstats::legacy::Source::CrossInk);
  vCodexPreview = readingstats::legacy::inspect(readingstats::legacy::Source::VCodex);
  legacyPreviewLoaded = true;
}

void StatsDashboardActivity::openBooks() {
  auto view = makeUniqueNoThrow<StatsBooksActivity>(renderer, mappedInput, manifestPath);
  if (!view) {
    LOG_ERR("STATS", "OOM opening book list");
    return;
  }
  startActivityForResult(std::move(view), [](const ActivityResult&) {});
}

void StatsDashboardActivity::openImport() {
  auto view = makeUniqueNoThrow<StatsImportActivity>(renderer, mappedInput);
  if (!view) {
    LOG_ERR("STATS", "OOM opening legacy import");
    return;
  }
  startActivityForResult(std::move(view), [this](const ActivityResult&) {
    refreshLegacyPreviews();
    refresh();
    requestUpdate();
  });
}

void StatsDashboardActivity::advance(const int delta) {
  if (!pageCount) return;
  activePage = static_cast<uint8_t>((activePage + pageCount + delta) % pageCount);
  if (importPage() && !legacyPreviewLoaded) refreshLegacyPreviews();
  requestUpdate();
}

void StatsDashboardActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (booksPage()) {
      openBooks();
      return;
    }
    if (importPage()) {
      openImport();
      return;
    }
    refresh();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    advance(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    advance(-1);
    return;
  }
#if FREEINK_CAP_TOUCH
  int x = 0, y = 0;
  const auto swipe = mappedInput.wasSwipe(&x, &y);
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up)
    advance(1);
  else if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down)
    advance(-1);
  else if ((booksPage() || importPage()) && mappedInput.wasScreenTapped(x, y)) {
    // Only the content band opens the child. The header Back control and
    // button-hint footer have their own meanings on touch devices.
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int contentTop = metrics.topPadding + metrics.headerHeight;
    const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
    if (y >= contentTop && y < contentBottom && x >= 0 && x < renderer.getScreenWidth()) {
      if (booksPage())
        openBooks();
      else
        openImport();
    }
  }
#endif
}

const readingstats::Snapshot& StatsDashboardActivity::statsFor(const Widget& widget) const {
  return widget.currentBook ? bookStats : stats;
}

bool StatsDashboardActivity::statsPresentFor(const Widget& widget) const {
  return widget.currentBook ? bookStatsAvailable : statsAvailable;
}

int StatsDashboardActivity::drawBookIdentity(int y, const int width) const {
  const int availableWidth = width - 2 * kMargin;
  const int smallStep = renderer.getLineHeight(UI_12_FONT_ID) + 2;
  const auto drawLines = [&](const int font, const std::string& text, const int maxLines,
                             const EpdFontFamily::Style style, const int step) {
    for (const auto& line : renderer.wrappedText(font, text.c_str(), availableWidth, maxLines, style)) {
      renderer.drawText(font, kMargin, y, line.c_str(), true, style);
      y += step;
    }
  };
  drawLines(NOTOSANS_14_FONT_ID, hasBookInfo && bookInfo.title[0] ? bookInfo.title : "No book selected", 3,
            EpdFontFamily::BOLD, renderer.getLineHeight(NOTOSANS_14_FONT_ID) + 2);
  if (hasBookInfo && bookInfo.author[0]) {
    drawLines(UI_12_FONT_ID, std::string("Author: ") + bookInfo.author, 2, EpdFontFamily::REGULAR, smallStep);
  }
  if (!seriesName.empty()) {
    const std::string series = "Series: " + seriesName + (seriesIndex.empty() ? "" : " #" + seriesIndex);
    drawLines(UI_12_FONT_ID, series, 3, EpdFontFamily::REGULAR, smallStep);
  }
  return y + 8;
}

std::string StatsDashboardActivity::metricValue(const Widget& widget) const {
  if (!statsPresentFor(widget)) return "--";
  const auto& stats = statsFor(widget);
  const auto& field = widget.field;
  if (field == "total_seconds") return duration(stats.totalSeconds());
  if (field == "sessions") return std::to_string(stats.sessions);
  if (field == "qualified_turns") return std::to_string(stats.pageTurns);
  if (field == "latest_day_seconds") return stats.latestDay ? duration(stats.secondsOnDay(stats.latestDay)) : "--";
  if (field == "last_7_seconds") {
    if (!stats.latestDay) return "--";
    uint64_t total = 0;
    for (uint32_t i = 0; i < 7 && i < stats.latestDay; ++i) total += stats.secondsOnDay(stats.latestDay - i);
    return duration(total);
  }
  if (field == "active_days") {
    uint32_t count = 0;
    for (uint32_t day = 0; day < readingstats::kHistoryDays && day < stats.latestDay; ++day)
      if (stats.secondsOnDay(stats.latestDay - day) > 0) ++count;
    return std::to_string(count);
  }
  if (field == "seconds_per_turn")
    return stats.pageTurns ? std::to_string(stats.totalSeconds() / stats.pageTurns) + "s" : "--";
  return "--";
}

void StatsDashboardActivity::drawMetric(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (height <= 0 || w <= 0) return;
  renderer.drawRect(x, y, w, height, true);
  constexpr int inset = 8;
  const int labelStep = renderer.getLineHeight(UI_12_FONT_ID) + 1;
  const auto labels = renderer.wrappedText(UI_12_FONT_ID, widget.label.c_str(), std::max(1, w - 2 * inset), 2);
  int labelY = y + 8;
  for (const auto& line : labels) {
    const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, x + (w - lineWidth) / 2, labelY, line.c_str());
    labelY += labelStep;
  }
  const std::string value = metricValue(widget);
  const int valueFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, value.c_str(), EpdFontFamily::BOLD) <= w - 2 * inset
                            ? NOTOSANS_14_FONT_ID
                            : UI_12_FONT_ID;
  const std::string fitted =
      renderer.truncatedText(valueFont, value.c_str(), std::max(1, w - 2 * inset), EpdFontFamily::BOLD);
  const int valueWidth = renderer.getTextWidth(valueFont, fitted.c_str(), EpdFontFamily::BOLD);
  const int valueHeight = renderer.getLineHeight(valueFont);
  const int valueTop = std::max(labelY + 4, y + (height * 2) / 5);
  const int valueY = std::max(labelY + 2, valueTop + std::max(0, (y + height - 7 - valueTop - valueHeight) / 2));
  if (valueY + valueHeight < y + height) {
    renderer.drawText(valueFont, x + (w - valueWidth) / 2, valueY, fitted.c_str(), true, EpdFontFamily::BOLD);
  }
}

void StatsDashboardActivity::drawWeek(const Widget& widget, const int x, const int y, const int w, const int h) const {
  if (w <= 24 || h <= 52) return;
  renderer.drawRect(x, y, w, h, true);
  const std::string label = renderer.truncatedText(UI_10_FONT_ID, widget.label.c_str(), w - 16);
  renderer.drawText(UI_10_FONT_ID, x + 8, y + 7, label.c_str());
  const auto& stats = statsFor(widget);
  const int chartTop = y + renderer.getLineHeight(UI_10_FONT_ID) + 17;
  const int baseline = y + h - renderer.getLineHeight(UI_10_FONT_ID) - 12;
  const int chartHeight = std::max(0, baseline - chartTop);
  const int slotWidth = (w - 20) / 7;
  const int barWidth = std::max(3, slotWidth / 2);
  uint32_t high = 0;
  for (uint32_t i = 0; i < 7; ++i) {
    if (stats.latestDay > i) high = std::max(high, stats.secondsOnDay(stats.latestDay - i));
  }
  renderer.fillRect(x + 8, baseline, w - 16, 1, true);
  constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (uint32_t i = 0; i < 7; ++i) {
    const uint32_t offset = 6 - i;
    const uint32_t day = stats.latestDay > offset ? stats.latestDay - offset : 0;
    const uint32_t seconds = day ? stats.secondsOnDay(day) : 0;
    const int barHeight = high ? static_cast<int>((static_cast<uint64_t>(chartHeight) * seconds) / high) : 0;
    const int centerX = x + 10 + static_cast<int>(i) * slotWidth + slotWidth / 2;
    if (barHeight > 0) {
      renderer.fillRect(centerX - barWidth / 2, baseline - barHeight, barWidth, barHeight, true);
    }
    if (day) {
      const char* name = days[(day + 3) % 7];  // Local-day ordinal: 1970-01-01 is Thursday.
      renderer.drawText(UI_10_FONT_ID, centerX - renderer.getTextWidth(UI_10_FONT_ID, name) / 2, baseline + 5, name);
    }
  }
}

void StatsDashboardActivity::drawHeatmap(const Widget& widget, const int x, const int y, const int w,
                                         const int h) const {
  if (w <= 24 || h <= 90) return;
  renderer.drawRect(x, y, w, h, true);
  const std::string label = renderer.truncatedText(UI_10_FONT_ID, widget.label.c_str(), w - 16);
  renderer.drawText(UI_10_FONT_ID, x + 8, y + 7, label.c_str());
  const auto& stats = statsFor(widget);
  constexpr int gap = 4;
  const int legendHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int gridTop = y + renderer.getLineHeight(UI_10_FONT_ID) + 29;
  const int cellW = std::max(5, std::min(42, (w - 24 - gap * 6) / 7));
  const int cellH = std::max(5, std::min(cellW, (y + h - gridTop - legendHeight - 15 - gap * 3) / 4));
  const int gridWidth = cellW * 7 + gap * 6;
  const int gridX = x + (w - gridWidth) / 2;
  constexpr const char* days[] = {"S", "M", "T", "W", "T", "F", "S"};
  const uint32_t firstDay = stats.latestDay > 27 ? stats.latestDay - 27 : 0;
  for (int col = 0; col < 7; ++col) {
    if (!firstDay) break;
    const char* labelDay = days[(firstDay + static_cast<uint32_t>(col) + 3) % 7];
    const int cellX = gridX + col * (cellW + gap);
    renderer.drawText(UI_10_FONT_ID, cellX + (cellW - renderer.getTextWidth(UI_10_FONT_ID, labelDay)) / 2,
                      gridTop - legendHeight - 3, labelDay);
  }
  for (int day = 0; day < 28; ++day) {
    const int col = day % 7;
    const int row = day / 7;
    const int cellX = gridX + col * (cellW + gap);
    const int cellY = gridTop + row * (cellH + gap);
    const uint32_t seconds =
        stats.latestDay > static_cast<uint32_t>(27 - day) ? stats.secondsOnDay(stats.latestDay - (27 - day)) : 0;
    renderer.drawRect(cellX, cellY, cellW, cellH, true);
    const int innerWidth = std::max(1, cellW - 4);
    if (seconds >= 2700)
      renderer.fillRect(cellX + 2, cellY + 2, innerWidth, std::max(1, cellH - 4), true);
    else if (seconds >= 900)
      renderer.fillRect(cellX + 2, cellY + 2, std::max(1, innerWidth / 2), std::max(1, cellH - 4), true);
    else if (seconds > 0)
      renderer.fillRect(cellX + 2, cellY + 2, std::max(1, innerWidth / 3), std::max(1, cellH - 4), true);
  }
  renderer.drawText(UI_10_FONT_ID, gridX, gridTop + 4 * cellH + 3 * gap + 6, "Less shading = less reading");
}

void StatsDashboardActivity::drawLegacyImport(const int x, const int y, const int w, const int h) const {
  if (h <= 0) return;
  renderer.drawRect(x, y, w, h, true);
  constexpr int inset = 9;
  const int step = renderer.getLineHeight(UI_12_FONT_ID) + 3;
  const int rowHeight = std::max(54, (h - 3 * inset - 2 * step - 12) / 2);
  auto source = [&](int top, const char* name, const char* filename, const readingstats::legacy::Preview& preview,
                    bool showTurns) {
    renderer.drawRect(x + inset, top, w - 2 * inset, rowHeight, true);
    int line = top + 7;
    auto textLine = [&](const char* text, bool bold = false) {
      if (line + step > top + rowHeight - 2) return;
      const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const std::string fitted = renderer.truncatedText(UI_12_FONT_ID, text, w - 4 * inset, style);
      renderer.drawText(UI_12_FONT_ID, x + inset * 2, line, fitted.c_str(), true, style);
      line += step;
    };
    textLine(name, true);
    textLine(filename);
    if (!legacyPreviewLoaded) {
      textLine("Checking source...");
    } else if (!preview.present) {
      textLine("Not found on SD card");
    } else if (!preview.supported) {
      textLine(preview.truncated ? "Too large for preview" : "Unsupported or damaged");
    } else {
      char value[96];
      snprintf(value, sizeof(value), "%lu sessions | %lluh %llum", static_cast<unsigned long>(preview.totals.sessions),
               static_cast<unsigned long long>(preview.totals.totalSeconds / 3600),
               static_cast<unsigned long long>((preview.totals.totalSeconds / 60) % 60));
      textLine(value);
      if (showTurns) {
        snprintf(value, sizeof(value), "%lu page turns", static_cast<unsigned long>(preview.totals.turns));
      } else {
        snprintf(value, sizeof(value), "%lu books; no turn history", static_cast<unsigned long>(preview.books));
      }
      textLine(value);
    }
  };
  source(y + inset, "CrossInk", "global_stats.bin", crossInkPreview, true);
  source(y + 2 * inset + rowHeight, "vCodex", "reading_stats.json", vCodexPreview, false);
  const int footer = y + 3 * inset + 2 * rowHeight;
  if (footer + 2 * step < y + h) {
    renderer.drawText(UI_10_FONT_ID, x + inset, footer, "Preview only; sources are unchanged.");
    renderer.drawText(UI_10_FONT_ID, x + inset, footer + step, "Select to review / import history.");
  }
}

void StatsDashboardActivity::drawWidget(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (widget.type == "metric")
    drawMetric(widget, x, y, w, height);
  else if (widget.type == "week_bars")
    drawWeek(widget, x, y, w, height);
  else if (widget.type == "heatmap_28")
    drawHeatmap(widget, x, y, w, height);
  else if (widget.type == "legacy_import")
    drawLegacyImport(x, y, w, height);
  else if (widget.type == "book_list") {
    if (height <= 0) return;
    renderer.drawRect(x, y, w, height, true);
    const std::string title = renderer.truncatedText(UI_12_FONT_ID, widget.label.c_str(), w - 16, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID,
                      x + (w - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD)) / 2, y + 13,
                      title.c_str(), true, EpdFontFamily::BOLD);
    char summary[48];
    const auto count = static_cast<unsigned long>(recordedBookCount);
    snprintf(summary, sizeof(summary), "%lu recorded %s", count, count == 1 ? "book" : "books");
    renderer.drawText(UI_12_FONT_ID, x + (w - renderer.getTextWidth(UI_12_FONT_ID, summary)) / 2,
                      y + height / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2, summary);
    const char* hint = "Tap or press Select to open";
    renderer.drawText(UI_10_FONT_ID, x + (w - renderer.getTextWidth(UI_10_FONT_ID, hint)) / 2,
                      y + height - renderer.getLineHeight(UI_10_FONT_ID) - 12, hint);
  }
}

void StatsDashboardActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 6;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title.c_str());
  int y = metrics.topPadding + metrics.headerHeight + 7;
  if (!pageCount) return;
  const Page& page = pages[activePage];
  char heading[85];
  snprintf(heading, sizeof(heading), "%s (%u/%u)", page.title.c_str(), static_cast<unsigned>(activePage + 1),
           static_cast<unsigned>(pageCount));
  renderer.drawText(UI_10_FONT_ID, kMargin, y,
                    renderer.truncatedText(UI_10_FONT_ID, heading, width - kMargin * 2).c_str(), true,
                    EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 9;
  const bool isBookMetrics = [&]() {
    for (uint8_t i = 0; i < page.count; ++i)
      if (page.widgets[i].currentBook) return true;
    return false;
  }();
  if (isBookMetrics) y = drawBookIdentity(y, width);
  if (!statsAvailable && !isBookMetrics && !importPage()) {
    renderer.drawText(UI_10_FONT_ID, kMargin, y, "No saved reading sessions");
    y += renderer.getLineHeight(UI_10_FONT_ID) + 5;
  }
  int widgetRows = 0;
  for (int i = 0; i < page.count; ++i) {
    ++widgetRows;
    if (page.widgets[i].type == "metric" && i + 1 < page.count && page.widgets[i + 1].type == "metric") ++i;
  }
  const int available = std::max(0, bottom - y);
  const int budget = std::max(0, available - kGap * std::max(0, widgetRows - 1));
  const int slotHeight = std::max(0, std::min(widgetRows >= 3 ? 150 : 180, budget / std::max(1, widgetRows)));
  const bool chartAndMetric = page.count == 2 && page.widgets[0].type != "metric" && page.widgets[1].type == "metric";
  const int contentWidth = width - kMargin * 2;
  const int halfWidth = (contentWidth - kGap) / 2;
  for (int i = 0; i < page.count && y < bottom; ++i) {
    const auto& widget = page.widgets[i];
    if (widget.type == "metric") {
      const int h = std::max(0, std::min({chartAndMetric ? 120 : slotHeight, bottom - y}));
      if (i + 1 < page.count && page.widgets[i + 1].type == "metric") {
        drawWidget(widget, kMargin, y, halfWidth, h);
        drawWidget(page.widgets[++i], kMargin + halfWidth + kGap, y, contentWidth - halfWidth - kGap, h);
      } else {
        drawWidget(widget, kMargin, y, contentWidth, h);
      }
      y += h + kGap;
    } else {
      const int desired = chartAndMetric                   ? std::min(260, std::max(0, available - 120 - kGap))
                          : widget.type == "legacy_import" ? 390
                                                           : 160;
      const int h = std::max(0, std::min(desired, bottom - y));
      drawWidget(widget, kMargin, y, contentWidth, h);
      y += h + kGap;
    }
  }
  const char* confirmLabel = (booksPage() || importPage()) ? "Open" : "Refresh";
  const auto hints = mappedInput.mapLabels("Back", confirmLabel, "Previous", "Next");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
