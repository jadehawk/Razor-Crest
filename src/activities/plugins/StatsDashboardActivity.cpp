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
  const char* title = hasBookInfo && bookInfo.title[0] ? bookInfo.title : "No book selected";
  const auto titleLines = renderer.wrappedText(NOTOSANS_14_FONT_ID, title, availableWidth, 2);
  for (const auto& line : titleLines) {
    renderer.drawText(NOTOSANS_14_FONT_ID, kMargin, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_14_FONT_ID) + 2;
  }
  if (hasBookInfo && bookInfo.author[0]) {
    const std::string author = std::string("Author: ") + bookInfo.author;
    renderer.drawText(UI_12_FONT_ID, kMargin, y,
                      renderer.truncatedText(UI_12_FONT_ID, author.c_str(), availableWidth).c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID) + 3;
  }
  if (!seriesName.empty()) {
    const std::string series = "Series: " + seriesName + (seriesIndex.empty() ? "" : " #" + seriesIndex);
    renderer.drawText(UI_12_FONT_ID, kMargin, y,
                      renderer.truncatedText(UI_12_FONT_ID, series.c_str(), availableWidth).c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID) + 3;
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
  renderer.drawRect(x, y, w, height, true);
  const std::string label = renderer.truncatedText(UI_12_FONT_ID, widget.label.c_str(), std::max(1, w - 16));
  renderer.drawText(UI_12_FONT_ID, x + 8, y + 10, label.c_str());
  const std::string value = metricValue(widget);
  const int valueFont = NOTOSANS_14_FONT_ID;
  const std::string fitted = renderer.truncatedText(valueFont, value.c_str(), std::max(1, w - 16));
  const int valueY = y + std::min(height - renderer.getLineHeight(valueFont) - 7,
                                  std::max(renderer.getLineHeight(UI_10_FONT_ID) + 15, height * 5 / 9));
  renderer.drawText(valueFont, x + 8, valueY, fitted.c_str(), true, EpdFontFamily::BOLD);
}

void StatsDashboardActivity::drawWeek(const Widget& widget, const int x, const int y, const int w, const int h) const {
  renderer.drawRect(x, y, w, h, true);
  const std::string label = renderer.truncatedText(UI_10_FONT_ID, widget.label.c_str(), std::max(1, w - 16));
  renderer.drawText(UI_10_FONT_ID, x + 8, y + 5, label.c_str());
  const auto& stats = statsFor(widget);
  const int chartTop = y + renderer.getLineHeight(UI_10_FONT_ID) + 12;
  const int chartHeight = std::max(6, h - (chartTop - y) - 16);
  const int barGap = 4;
  const int chartWidth = std::max(1, w - 28);
  const int barWidth = std::max(2, (chartWidth - 6 * barGap) / 7);
  uint32_t high = 0;
  for (uint32_t i = 0; i < 7; ++i)
    if (stats.latestDay > i) high = std::max(high, stats.secondsOnDay(stats.latestDay - i));
  for (uint32_t i = 0; i < 7; ++i) {
    const uint32_t seconds = stats.latestDay > 6 - i ? stats.secondsOnDay(stats.latestDay - (6 - i)) : 0;
    const int height = high ? static_cast<int>((static_cast<uint64_t>(chartHeight) * seconds) / high) : 0;
    if (height > 0)
      renderer.fillRect(x + 10 + static_cast<int>(i) * (barWidth + barGap), chartTop + chartHeight - height, barWidth,
                        height, true);
  }
}

void StatsDashboardActivity::drawHeatmap(const Widget& widget, const int x, const int y, const int w,
                                         const int h) const {
  renderer.drawRect(x, y, w, h, true);
  const std::string label = renderer.truncatedText(UI_10_FONT_ID, widget.label.c_str(), std::max(1, w - 16));
  renderer.drawText(UI_10_FONT_ID, x + 8, y + 5, label.c_str());
  const auto& stats = statsFor(widget);
  const int top = y + renderer.getLineHeight(UI_10_FONT_ID) + 12;
  const int gap = 4;
  const int cellW = std::max(5, (w - 24 - gap * 6) / 7);
  const int cellH = std::max(5, (h - (top - y) - 12 - gap * 3) / 4);
  for (int day = 0; day < 28; ++day) {
    const int col = day % 7;
    const int row = day / 7;
    const int cellX = x + 10 + col * (cellW + gap);
    const int cellY = top + row * (cellH + gap);
    const uint32_t seconds =
        stats.latestDay > static_cast<uint32_t>(27 - day) ? stats.secondsOnDay(stats.latestDay - (27 - day)) : 0;
    renderer.drawRect(cellX, cellY, cellW, cellH, true);
    if (seconds >= 2700)
      renderer.fillRect(cellX + 2, cellY + 2, std::max(1, cellW - 4), std::max(1, cellH - 4), true);
    else if (seconds >= 900)
      renderer.fillRect(cellX + 2, cellY + 2, std::max(1, (cellW - 4) / 2), std::max(1, cellH - 4), true);
    else if (seconds > 0)
      renderer.fillRect(cellX + 2, cellY + 2, std::max(1, (cellW - 4) / 3), std::max(1, cellH - 4), true);
  }
}

void StatsDashboardActivity::drawLegacyImport(const int x, const int y, const int w, const int h) const {
  renderer.drawRect(x, y, w, h, true);
  const int inset = 10;
  int line = y + 8;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID) + 5;
  auto textLine = [&](const char* text, bool emphasized = false) {
    if (line + lineHeight > y + h - 6) return;
    renderer.drawText(UI_12_FONT_ID, x + inset, line,
                      renderer.truncatedText(UI_12_FONT_ID, text, w - inset * 2).c_str(), true,
                      emphasized ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    line += lineHeight;
  };
  auto source = [&](const char* label, const readingstats::legacy::Preview& preview, bool showTurns) {
    textLine(label, true);
    if (!preview.present) {
      textLine("No source file found");
      return;
    }
    if (!preview.supported) {
      textLine(preview.truncated ? "Source too large for preview" : "Unsupported or damaged source");
      return;
    }
    char value[96];
    if (showTurns) {
      snprintf(value, sizeof(value), "%lluh %lum | %lu sessions",
               static_cast<unsigned long long>(preview.totals.totalSeconds / 3600),
               static_cast<unsigned long>((preview.totals.totalSeconds / 60) % 60),
               static_cast<unsigned long>(preview.totals.sessions));
      textLine(value);
      snprintf(value, sizeof(value), "%lu page turns (global)", static_cast<unsigned long>(preview.totals.turns));
    } else {
      snprintf(value, sizeof(value), "%lu books | %lu sessions", static_cast<unsigned long>(preview.books),
               static_cast<unsigned long>(preview.totals.sessions));
      textLine(value);
      snprintf(value, sizeof(value), "%lluh %llum | no turn history",
               static_cast<unsigned long long>(preview.totals.totalSeconds / 3600),
               static_cast<unsigned long long>((preview.totals.totalSeconds / 60) % 60));
    }
    textLine(value);
  };
  if (!legacyPreviewLoaded) {
    textLine("Checking legacy SD sources...");
    return;
  }
  source("CrossInk  -  global_stats.bin", crossInkPreview, true);
  line += 8;
  source("vCodex  -  reading_stats.json", vCodexPreview, false);
  line += 12;
  textLine("Preview only; no files are changed.");
  textLine("Select: review / import history");
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
    renderer.drawRect(x, y, w, height, true);
    renderer.drawText(UI_12_FONT_ID, x + 10, y + 10, "Browse recorded books", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, x + 10, y + 10 + renderer.getLineHeight(UI_12_FONT_ID),
                      "Tap or press Select to open");
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
  // Use most of the X3 display rather than a compact strip at the top.
  // A bounded per-row budget also keeps small-screen and touch layouts safe.
  const int budget = std::max(0, (bottom - y) * 3 / 4 - kGap * std::max(0, widgetRows - 1));
  const int slotHeight = std::max(32, std::min(widgetRows >= 3 ? 190 : 270, budget / std::max(1, widgetRows)));
  const int contentWidth = width - kMargin * 2;
  const int halfWidth = (contentWidth - kGap) / 2;
  for (int i = 0; i < page.count && y < bottom - 35; ++i) {
    const auto& widget = page.widgets[i];
    if (widget.type == "metric") {
      const int h = std::max(0, std::min({slotHeight, bottom - y}));
      if (i + 1 < page.count && page.widgets[i + 1].type == "metric") {
        drawWidget(widget, kMargin, y, halfWidth, h);
        drawWidget(page.widgets[++i], kMargin + halfWidth + kGap, y, contentWidth - halfWidth - kGap, h);
      } else {
        drawWidget(widget, kMargin, y, contentWidth, h);
      }
      y += h + kGap;
    } else {
      const int h = std::max(0, std::min({350, slotHeight, bottom - y}));
      drawWidget(widget, kMargin, y, contentWidth, h);
      y += h + kGap;
    }
  }
  const char* confirmLabel = (booksPage() || importPage()) ? "Open" : "Refresh";
  const auto hints = mappedInput.mapLabels("Back", confirmLabel, "Previous", "Next");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
