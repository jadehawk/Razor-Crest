#include "StatsDashboardActivity.h"
#include "DashboardCalendar.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
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
#include <vector>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "StatsImportActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReadingStatsBookMedia.h"
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
                                               const readingstats::BookEntry& selected)
    : Activity("PluginDashboard", renderer, input),
      manifestPath(std::move(manifest)),
      bookInfo(selected),
      selectedBook(true),
      hasBookInfo(true) {}

int8_t StatsDashboardActivity::loadIcon(const std::string& name) {
  for (uint8_t i = 0; i < iconCount; ++i) {
    if (iconAssets[i].name == name) return static_cast<int8_t>(i);
  }
  if (iconCount >= iconAssets.size()) {
    LOG_ERR("STATS", "Dashboard icon count exceeds limit");
    return -1;
  }
  const auto slash = manifestPath.find_last_of('/');
  if (slash == std::string::npos) return -1;
  const std::string path = manifestPath.substr(0, slash) + "/icons/" + name + ".pbm";
  std::string raw;
  dashboardcards::Icon icon;
  if (!Storage.readFileToString("STATS", path, 1400, raw) || !dashboardcards::decodePbm(raw, icon)) {
    LOG_ERR("STATS", "Invalid dashboard icon asset: %s", name.c_str());
    return -1;
  }
  IconAsset& asset = iconAssets[iconCount];
  asset.name = name;
  asset.bitmap = icon;
  return static_cast<int8_t>(iconCount++);
}

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
      widget.showValue = widgetJson["show_value"] | true;
      widget.label = widgetJson["label"] | "";
      const char* border = widgetJson["border"] | (widget.type == "hero" ? "rounded" : "square");
      if (!dashboardcards::parseBorder(border, widget.border)) return false;
      const int radius = widgetJson["corner_radius"] | 8;
      if (radius < 0 || radius > 16) return false;
      widget.cornerRadius = static_cast<uint8_t>(radius);
      const int iconSize = widgetJson["icon_size"] | 0;
      if (iconSize < 0 || iconSize > 128) return false;
      widget.iconSize = static_cast<uint8_t>(iconSize);
      const char* icon = widgetJson["icon"] | "";
      if (*icon) {
        if (!dashboardcards::validIconName(icon)) return false;
        widget.iconIndex = loadIcon(icon);
      }
      const char* scope = widgetJson["scope"] | "global";
      if (strcmp(scope, "global") != 0 && strcmp(scope, "current-book") != 0) return false;
      widget.currentBook = strcmp(scope, "current-book") == 0;
      if ((widget.type == "hero" || widget.type == "metric") && !knownMetric(widget.field)) return false;
      if (widget.type != "metric" && widget.type != "week_bars" && widget.type != "heatmap_28" &&
          widget.type != "book_list" && widget.type != "legacy_import" && widget.type != "hero")
        return false;
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
  const RecentBook* recent = nullptr;
  for (const auto& item : RECENT_BOOKS.getBooks())
    if (item.path == path) { recent = &item; break; }

  // Prefer an exact record. If a book was renamed since importing its
  // history, bind the uniquely matching title/author to that existing record.
  // A duplicate title is ambiguous and must never join unrelated histories.
  auto candidate = makeUniqueNoThrow<readingstats::BookEntry>();
  int titleMatches = 0;
  if (candidate) {
    for (uint32_t i = 0, total = readingstats::bookCount(); i < total; ++i) {
      if (!readingstats::bookAt(i, *candidate)) continue;
      if (path == candidate->path) {
        bookInfo = *candidate;
        hasBookInfo = true;
        return;
      }
      if (recent && !recent->title.empty() && recent->title == candidate->title &&
          (candidate->author[0] == 0 || recent->author == candidate->author)) {
        ++titleMatches;
        if (titleMatches == 1) bookInfo = *candidate;
      }
    }
  }
  if (titleMatches == 1) {
    hasBookInfo = true;
    return;
  }

  // A book without a stats index still deserves Home metadata and cover.
  bookInfo = {};
  bookInfo.setHash(readingstats::media::hashSourcePath(path.c_str()));
  snprintf(bookInfo.path, sizeof(bookInfo.path), "%s", path.c_str());
  const size_t split = path.find_last_of("/\\");
  const std::string fallback = split == std::string::npos ? path : path.substr(split + 1);
  snprintf(bookInfo.title, sizeof(bookInfo.title), "%s", fallback.c_str());
  if (recent) {
    if (!recent->title.empty()) snprintf(bookInfo.title, sizeof(bookInfo.title), "%s", recent->title.c_str());
    if (!recent->author.empty()) snprintf(bookInfo.author, sizeof(bookInfo.author), "%s", recent->author.c_str());
  }
  hasBookInfo = true;
}

void StatsDashboardActivity::loadSeries() {
  seriesName.clear();
  seriesIndex.clear();
  if (!hasBookInfo || !bookInfo.path[0]) return;
  const std::string path = readingstats::media::bookSource(bookInfo);
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
      // Choose a fully book-scoped page; a mixed Overview also contains a book hero.
      bool bookPage = pages[i].count > 0;
      for (uint8_t j = 0; j < pages[i].count; ++j)
        if (!pages[i].widgets[j].currentBook) bookPage = false;
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
  currentProgress = hasBookInfo ? readingstats::media::progress(bookInfo) : -1;
  currentAuthor = hasBookInfo ? readingstats::media::author(bookInfo) : std::string();
  if (hasBookInfo) readingstats::media::ensureCover(bookInfo, 142);
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

int StatsDashboardActivity::dashboardContentTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + 7 +
         renderer.getLineHeight(UI_12_FONT_ID) + 9;
}

int StatsDashboardActivity::listCapacity() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = dashboardContentTop();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 6;
  return std::max(1, std::min(static_cast<int>(kMaxVisibleBooks), (bottom - top - 26) / kBookRowStep));
}

void StatsDashboardActivity::loadVisibleBooks() {
  visibleCount = 0;
  nextCoverIndex = 0;
  const int capacity = listCapacity();
  // A snapshot is ~1.5 KiB: keep one reusable instance off the X3 task stack.
  auto summary = makeUniqueNoThrow<readingstats::Snapshot>();
  for (int i = 0; i < capacity && visibleStart + i < recordedBookCount; ++i) {
    BookRow& row = visibleBooks[visibleCount];
    row = {};
    if (!readingstats::bookAt(visibleStart + i, row.entry)) continue;
    if (summary && readingstats::loadBookHash(row.entry.hash(), *summary)) row.seconds = summary->totalSeconds();
    row.progress = readingstats::media::progress(row.entry, false);
    const std::string author = readingstats::media::author(row.entry);
    snprintf(row.displayAuthor, sizeof(row.displayAuthor), "%s", author.c_str());
    ++visibleCount;
  }
}

void StatsDashboardActivity::moveBookCursor(int delta) {
  if (!recordedBookCount) return;
  const int64_t target = static_cast<int64_t>(bookCursor) + delta;
  bookCursor = static_cast<uint32_t>(std::clamp<int64_t>(target, 0, recordedBookCount - 1));
  const uint32_t capacity = static_cast<uint32_t>(listCapacity());
  const uint32_t newStart = (bookCursor / capacity) * capacity;
  if (newStart != visibleStart) {
    visibleStart = newStart;
    loadVisibleBooks();
  }
  requestUpdate();
}

void StatsDashboardActivity::openBooks() {
  if (!recordedBookCount || bookCursor < visibleStart || bookCursor >= visibleStart + visibleCount) return;
  const auto& book = visibleBooks[bookCursor - visibleStart].entry;
  auto view = makeUniqueNoThrow<StatsDashboardActivity>(renderer, mappedInput, manifestPath, book);
  if (!view) {
    LOG_ERR("STATS", "OOM opening book details");
    return;
  }
  startActivityForResult(std::move(view), [](const ActivityResult&) {});
}

void StatsDashboardActivity::promptRemoveBook() {
  if (!booksPage() || !recordedBookCount || bookCursor < visibleStart ||
      bookCursor >= visibleStart + visibleCount) return;
  const auto& book = visibleBooks[bookCursor - visibleStart].entry;
  const uint64_t hash = book.hash();
  if (!hash) return;
  const std::string subject = book.title[0] ? book.title : "Unnamed Book";
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, "Remove reading history?",
      "Remove saved statistics for " + subject + "? The book file and global totals stay intact.",
      "Cancel", "Remove", true);
  if (!confirmation) {
    LOG_ERR("STATS", "OOM: remove history confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, hash](const ActivityResult& result) {
    if (result.isCancelled) return;
    if (!readingstats::removeBookRecord(hash)) {
      LOG_ERR("STATS", "Could not remove book statistics %016llx", static_cast<unsigned long long>(hash));
      return;
    }
    refresh();
    if (recordedBookCount)
      bookCursor = std::min(bookCursor, recordedBookCount - 1);
    else
      bookCursor = 0;
    visibleStart = recordedBookCount ? (bookCursor / static_cast<uint32_t>(listCapacity())) *
                                          static_cast<uint32_t>(listCapacity()) : 0;
    loadVisibleBooks();
    requestUpdate();
  });
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
  LOG_DBG("STATS", "Dashboard page %u/%u (delta=%d)", static_cast<unsigned>(activePage + 1),
          static_cast<unsigned>(pageCount), delta);
  if (importPage() && !legacyPreviewLoaded) refreshLegacyPreviews();
  if (booksPage()) {
    refresh();
    loadVisibleBooks();
  }
  requestUpdate();
}

void StatsDashboardActivity::loop() {
  // A hold opens confirmation before its eventual release can open the book.
  if (booksPage() && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 900)) {
    promptRemoveBook();
    return;
  }
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
    currentProgress = hasBookInfo ? readingstats::media::progress(bookInfo) : -1;
    currentAuthor = hasBookInfo ? readingstats::media::author(bookInfo) : std::string();
    if (hasBookInfo) readingstats::media::ensureCover(bookInfo, 142);
    requestUpdate();
    return;
  }
  // Side Up/Down always change dashboard pages, including from All Books
  // to Import Statistics (and from the last page back to the first).
  // Front Left/Right select books *only* on All Books; elsewhere they also
  // turn dashboard pages. Keeping these axes separate avoids trapping users
  // on page five when the physical side buttons are their page controls.
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    advance(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    advance(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (booksPage())
      moveBookCursor(1);
    else
      advance(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (booksPage())
      moveBookCursor(-1);
    else
      advance(-1);
    return;
  }
#if FREEINK_CAP_TOUCH
  int x = 0, y = 0;
  if (booksPage() && mappedInput.wasScreenLongPress(x, y)) {
    const int top = dashboardContentTop();
    const int row = (y - top) / kBookRowStep;
    if (y >= top && x >= kMargin && x < renderer.getScreenWidth() - kMargin &&
        row >= 0 && row < visibleCount && (y - top) % kBookRowStep < kBookRowStep - 4) {
      bookCursor = visibleStart + row;
      promptRemoveBook();
      return;
    }
  }
  const auto swipe = mappedInput.wasSwipe(&x, &y);
  if (booksPage() && swipe == MappedInputManager::SwipeDir::Up) {
    moveBookCursor(listCapacity());
    return;
  }
  if (booksPage() && swipe == MappedInputManager::SwipeDir::Down) {
    moveBookCursor(-listCapacity());
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    advance(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    advance(-1);
    return;
  }
  if ((booksPage() || importPage()) && mappedInput.wasScreenTapped(x, y)) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int contentTop = dashboardContentTop();
    const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 6;
    if (y >= contentTop && y < contentBottom && x >= 0 && x < renderer.getScreenWidth()) {
      if (booksPage()) {
        const int row = (y - contentTop) / kBookRowStep;
        if (row >= 0 && row < visibleCount && (y - contentTop) % kBookRowStep < kBookRowStep - 4) {
          bookCursor = visibleStart + row;
          openBooks();
          return;
        }
      } else {
        openImport();
        return;
      }
    }
  }
#endif
  // Generate at most one visible thumbnail per loop; never during rendering.
  if (booksPage() && nextCoverIndex < visibleCount) {
    auto& row = visibleBooks[nextCoverIndex++];
    const int updated = readingstats::media::progress(row.entry);
    if (updated != row.progress) {
      row.progress = updated;
      requestUpdate();
    }
    const std::string path = readingstats::media::coverPath(row.entry.hash(), 68);
    if ((!Storage.exists(path.c_str()) || path.find(".list_bw_fill.bmp") == std::string::npos) &&
        readingstats::media::ensureCover(row.entry, 68))
      requestUpdate();
  }
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

void StatsDashboardActivity::drawCardBorder(const Widget& widget, int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  const int r = std::min({static_cast<int>(widget.cornerRadius), w / 4, h / 4});
  if (r == 0 || widget.border == dashboardcards::Border::Square) {
    renderer.drawRect(x, y, w, h);
  } else if (widget.border == dashboardcards::Border::Rounded) {
    renderer.drawRoundedRect(x, y, w, h, 1, r, true);
  } else {
    const int right = x + w - 1, bottom = y + h - 1;
    renderer.drawLine(x + r, y, right - r, y);
    renderer.drawLine(right - r, y, right, y + r);
    renderer.drawLine(right, y + r, right, bottom - r);
    renderer.drawLine(right, bottom - r, right - r, bottom);
    renderer.drawLine(right - r, bottom, x + r, bottom);
    renderer.drawLine(x + r, bottom, x, bottom - r);
    renderer.drawLine(x, bottom - r, x, y + r);
    renderer.drawLine(x, y + r, x + r, y);
  }
}

void StatsDashboardActivity::drawCardIcon(const Widget& widget, int x, int y, int size) const {
  if (widget.iconIndex < 0 || widget.iconIndex >= iconCount || size <= 0) return;
  const auto& icon = iconAssets[widget.iconIndex].bitmap;
  const int sourceSize = icon.dimension;
  // Crop transparent margins before integer scaling so small 24x24 sources
  // render at near full card height without stretching or extra image memory.
  int minX = sourceSize, minY = sourceSize, maxX = -1, maxY = -1;
  for (int yy = 0; yy < sourceSize; ++yy)
    for (int xx = 0; xx < sourceSize; ++xx)
      if (icon.at(xx, yy)) {
        minX = std::min(minX, xx);
        minY = std::min(minY, yy);
        maxX = std::max(maxX, xx);
        maxY = std::max(maxY, yy);
      }
  if (maxX < minX) return;
  const int inkWidth = maxX - minX + 1;
  const int inkHeight = maxY - minY + 1;
  const int span = std::max(inkWidth, inkHeight);
  const int visibleSide = std::max(1, size - 6);
  const int visibleWidth = std::max(1, visibleSide * inkWidth / span);
  const int visibleHeight = std::max(1, visibleSide * inkHeight / span);
  const int originX = x + (size - visibleWidth) / 2;
  const int originY = y + (size - visibleHeight) / 2;
  for (int yy = minY; yy <= maxY; ++yy) {
    const int top = originY + ((yy - minY) * visibleHeight) / inkHeight;
    const int bottom = originY + ((yy - minY + 1) * visibleHeight) / inkHeight;
    for (int xx = minX; xx <= maxX; ++xx) {
      if (!icon.at(xx, yy)) continue;
      const int left = originX + ((xx - minX) * visibleWidth) / inkWidth;
      const int right = originX + ((xx - minX + 1) * visibleWidth) / inkWidth;
      if (right > left && bottom > top) renderer.fillRect(left, top, right - left, bottom - top);
    }
  }
}

void StatsDashboardActivity::drawBookCover(uint64_t hash, Rect rect) const {
  // Reset this image slot to paper white. The selected book row is dithered
  // gray behind it, and light pixels must not inherit that background.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  bool drawn = false;
  const std::string path = readingstats::media::coverPath(hash, rect.height);
  HalFile file;
  if (!path.empty() && Storage.openFileForRead("RST", path, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
      const float scale = std::min(static_cast<float>(rect.width) / bitmap.getWidth(),
                                   static_cast<float>(rect.height) / bitmap.getHeight());
      const int fittedW = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
      const int fittedH = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
      drawn = renderer.drawBitmap(bitmap, rect.x + (rect.width - fittedW) / 2,
                                  rect.y + (rect.height - fittedH) / 2, rect.width, rect.height,
                                  0.0f, 0.0f, false, true);
    }
    file.close();
  }
  if (!drawn) GUI.drawCoverPlaceholder(renderer, rect);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
}

void StatsDashboardActivity::drawHero(const Widget& widget, int x, int y, int w, int h) const {
  if (w <= 0 || h < 72) return;
  drawCardBorder(widget, x, y, w, h);
  constexpr int inset = 13;
  // One centered page title is shown above the card. The book hero itself
  // stays title-free on both Overall Statistics and Current Book.
  const int bodyY = y + 12;
  if (widget.currentBook) {
    if (!hasBookInfo) {
      renderer.drawText(UI_12_FONT_ID, x + inset, bodyY, "No book selected");
      return;
    }

    // The cover owns the left column; title, author and series always use
    // one shared right-column origin. Keep the progress row below both columns.
    const int coverH = std::max(22, std::min(142, h - 49));
    const int coverW = std::max(22, coverH * 2 / 3);
    const Rect coverRect{x + inset, bodyY + 1, coverW, coverH};
    drawBookCover(bookInfo.hash(), coverRect);
    const int textX = coverRect.x + coverRect.width + 12;
    const int textW = std::max(20, x + w - inset - textX);
    const int textBottom = y + h - 34;
    int cursor = bodyY;

    const char* bookTitle = bookInfo.title[0] ? bookInfo.title : "Untitled";
    const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, bookTitle, textW, 2,
                                                 EpdFontFamily::BOLD)) {
      if (cursor + titleLine > textBottom) break;
      renderer.drawText(UI_12_FONT_ID, textX, cursor, line.c_str(), true, EpdFontFamily::BOLD);
      cursor += titleLine + 2;
    }
    const std::string& bookAuthor = currentAuthor;
    const int authorLine = renderer.getLineHeight(UI_12_FONT_ID);
    if (!bookAuthor.empty() && cursor + authorLine <= textBottom) {
      renderer.drawText(UI_12_FONT_ID, textX, cursor,
                        renderer.truncatedText(UI_12_FONT_ID, bookAuthor.c_str(), textW).c_str());
      cursor += authorLine + 2;
    }
    const std::string series = seriesName + (seriesIndex.empty() ? "" : " #" + seriesIndex);
    if (!seriesName.empty() && cursor + renderer.getLineHeight(UI_10_FONT_ID) <= textBottom) {
      renderer.drawText(UI_10_FONT_ID, textX, cursor,
                        renderer.truncatedText(UI_10_FONT_ID, series.c_str(), textW).c_str());
    }

    const int barX = x + inset;
    const int barY = y + h - 24;
    const int barW = std::max(20, w - 2 * inset - 59);
    renderer.drawRect(barX, barY, barW, 11, true);
    if (currentProgress > 0)
      renderer.fillRect(barX + 2, barY + 2, (barW - 4) * currentProgress / 100, 7, true);
    char percentage[12];
    if (currentProgress >= 0)
      snprintf(percentage, sizeof(percentage), "%d%%", currentProgress);
    else
      snprintf(percentage, sizeof(percentage), "--");
    // Text is taller than the 11px bar; raise it to align the visual centers.
    const int textY = barY - (renderer.getLineHeight(UI_12_FONT_ID) - 11) / 2 - 6;
    renderer.drawText(UI_12_FONT_ID, barX + barW + 9, textY, percentage, true, EpdFontFamily::BOLD);
  } else if (widget.showValue) {
    const std::string value = metricValue(widget);
    renderer.drawText(NOTOSANS_14_FONT_ID, x + inset, bodyY + 5,
                      renderer.truncatedText(NOTOSANS_14_FONT_ID, value.c_str(), w - 2 * inset,
                                             EpdFontFamily::BOLD).c_str(), true, EpdFontFamily::BOLD);
  }
}

void StatsDashboardActivity::drawMetric(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (height <= 0 || w <= 0) return;
  drawCardBorder(widget, x, y, w, height);
  if (widget.iconIndex >= 0) {
    constexpr int inset = 10;
    constexpr int valueGap = 9;
    constexpr int minValueWidth = 64;
    const int labelHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int labelY = y + 11;
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, widget.label.c_str(),
                                                      w - 2 * inset, EpdFontFamily::BOLD);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (w - labelWidth) / 2, labelY,
                      label.c_str(), true, EpdFontFamily::BOLD);

    // The centered heading owns the card's top row. The icon fills as much
    // of the remaining height as possible, leaving room for a readable value.
    const int bodyTop = labelY + labelHeight + 8;
    const int bodyHeight = std::max(0, y + height - inset - bodyTop);
    const int iconSide = std::max(0, std::min({bodyHeight, w - 2 * inset - valueGap - minValueWidth,
                                              widget.iconSize ? static_cast<int>(widget.iconSize) : 96}));
    if (iconSide > 0)
      drawCardIcon(widget, x + inset, bodyTop + (bodyHeight - iconSide) / 2, iconSide);
    const int valueX = x + inset + iconSide + valueGap;
    const int valueWidth = std::max(12, x + w - inset - valueX);
    const std::string value = metricValue(widget);
    const int valueFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, value.c_str(), EpdFontFamily::BOLD) <= valueWidth
                              ? NOTOSANS_14_FONT_ID : UI_12_FONT_ID;
    const std::string fitted = renderer.truncatedText(valueFont, value.c_str(), valueWidth,
                                                        EpdFontFamily::BOLD);
    const int fittedWidth = renderer.getTextWidth(valueFont, fitted.c_str(), EpdFontFamily::BOLD);
    const int fittedHeight = renderer.getLineHeight(valueFont);
    if (bodyHeight >= fittedHeight)
      renderer.drawText(valueFont, valueX + (valueWidth - fittedWidth) / 2,
                        bodyTop + (bodyHeight - fittedHeight) / 2, fitted.c_str(),
                        true, EpdFontFamily::BOLD);
    return;
  }
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
  renderer.drawText(UI_10_FONT_ID, x + (w - renderer.getTextWidth(UI_10_FONT_ID, label.c_str())) / 2,
                    y + 7, label.c_str());
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
  renderer.drawText(UI_10_FONT_ID, x + (w - renderer.getTextWidth(UI_10_FONT_ID, label.c_str())) / 2,
                    y + 7, label.c_str());
  const auto& stats = statsFor(widget);
  constexpr int gap = 4;
  const int legendHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int gridTop = y + renderer.getLineHeight(UI_10_FONT_ID) + 29;
  const int cellW = std::max(5, std::min(42, (w - 24 - gap * 6) / 7));
  const int cellH = std::max(5, std::min(cellW, (y + h - gridTop - legendHeight - 15 - gap * 3) / 4));
  const int gridWidth = cellW * 7 + gap * 6;
  const int gridX = x + (w - gridWidth) / 2;
  constexpr const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  const uint32_t firstDay = dashboardcalendar::fourWeekMonday(stats.latestDay);
  for (int col = 0; col < 7; ++col) {
    // Always display Monday-through-Sunday headers, even without dated sessions.
    const char* labelDay = days[col];
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
        (firstDay && firstDay + static_cast<uint32_t>(day) <= stats.latestDay)
            ? stats.secondsOnDay(firstDay + static_cast<uint32_t>(day)) : 0;
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

void StatsDashboardActivity::drawBookList(int top, int bottom) const {
  const int width = renderer.getScreenWidth();
  if (!recordedBookCount) {
    renderer.drawText(UI_12_FONT_ID, kMargin, top + 20, "No recorded books yet");
    return;
  }
  for (uint8_t i = 0; i < visibleCount; ++i) {
    const auto& row = visibleBooks[i];
    const int x = kMargin;
    const int y = top + i * kBookRowStep;
    const int w = width - 2 * kMargin;
    const int h = kBookRowStep - 4;
    if (y + h > bottom) break;
    const bool selected = visibleStart + i == bookCursor;
    if (selected) {
      // Light-gray dither is visible on monochrome X3 as well as X4Pro.
      // Paint BEFORE text and artwork so both retain their original contrast.
      renderer.fillRoundedRect(x + 1, y + 1, w - 2, h - 2, 6, Color::LightGray);
      renderer.fillRect(x + 3, y + 12, 4, h - 24, true);
    }
    renderer.drawRoundedRect(x, y, w, h, selected ? 2 : 1, 7, true);
    drawBookCover(row.entry.hash(), Rect{x + 8, y + 5, 46, 68});
    const int textX = x + 65;
    const int textW = std::max(20, w - 77);
    const char* name = row.entry.title[0] ? row.entry.title : "Unnamed Book";
    int cursor = y + 7;
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, name, textW, 2, EpdFontFamily::BOLD)) {
      if (cursor + renderer.getLineHeight(UI_12_FONT_ID) > y + h - 31) break;
      renderer.drawText(UI_12_FONT_ID, textX, cursor, line.c_str(), true, EpdFontFamily::BOLD);
      cursor += renderer.getLineHeight(UI_12_FONT_ID);
    }
    if (row.displayAuthor[0] && cursor + renderer.getLineHeight(UI_10_FONT_ID) <= y + h - 19)
      renderer.drawText(UI_10_FONT_ID, textX, cursor,
                        renderer.truncatedText(UI_10_FONT_ID, row.displayAuthor, textW).c_str());
    const std::string details = duration(row.seconds) + " read  |  " +
                                (row.progress >= 0 ? std::to_string(row.progress) + "%" : "--");
    renderer.drawText(UI_12_FONT_ID, textX, y + h - renderer.getLineHeight(UI_12_FONT_ID) - 6,
                      renderer.truncatedText(UI_12_FONT_ID, details.c_str(), textW).c_str());
  }
  char count[56];
  snprintf(count, sizeof(count), "%lu-%lu of %lu books",
           static_cast<unsigned long>(visibleStart + 1),
           static_cast<unsigned long>(visibleStart + visibleCount),
           static_cast<unsigned long>(recordedBookCount));
  const int footer = top + visibleCount * kBookRowStep + 3;
  const int noteHeight = renderer.getLineHeight(UI_10_FONT_ID);
  if (footer + noteHeight < bottom)
    renderer.drawText(UI_10_FONT_ID, kMargin, footer, count);
  if (footer + noteHeight * 2 + 4 < bottom)
    renderer.drawText(UI_10_FONT_ID, kMargin, footer + noteHeight + 4,
                      "Hold Open to remove selected book history");
}

void StatsDashboardActivity::drawWidget(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (widget.type == "metric")
    drawMetric(widget, x, y, w, height);
  else if (widget.type == "hero")
    drawHero(widget, x, y, w, height);
  else if (widget.type == "week_bars")
    drawWeek(widget, x, y, w, height);
  else if (widget.type == "heatmap_28")
    drawHeatmap(widget, x, y, w, height);
  else if (widget.type == "legacy_import")
    drawLegacyImport(x, y, w, height);
}

void StatsDashboardActivity::drawDashboardPage(bool interactive) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  // Sleep is a noninteractive wallpaper: no navigation header, Back control,
  // or bottom button-hint bar. Use their space for the actual statistics.
  const int bottom = renderer.getScreenHeight() - (interactive ? metrics.buttonHintsHeight : 0) - 6;
  if (interactive)
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title.c_str());
  if (!pageCount) return;
  const Page& page = pages[activePage];
  // Keep the centered page title in both modes; sleep moves it up into the
  // header-free area, while the interactive plugin keeps its normal chrome.
  const int titleY = metrics.topPadding + (interactive ? metrics.headerHeight : 0) + 7;
  const std::string pageTitle = renderer.truncatedText(UI_12_FONT_ID, page.title.c_str(),
                                                       width - kMargin * 2, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID,
                    (width - renderer.getTextWidth(UI_12_FONT_ID, pageTitle.c_str(),
                                                   EpdFontFamily::BOLD)) / 2,
                    titleY, pageTitle.c_str(), true, EpdFontFamily::BOLD);
  int y = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 9;
  if (booksPage()) {
    drawBookList(y, bottom);
    // Front Previous/Next move the selected book; side Up/Down switch pages.
    const auto hints = mappedInput.mapLabels("Back", "Open", "Prev Book", "Next Book");
    if (interactive) GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
    return;
  }
  const bool isBookMetrics = [&]() {
    for (uint8_t i = 0; i < page.count; ++i)
      if (page.widgets[i].currentBook) return true;
    return false;
  }();
  bool hasHero = false;
  for (uint8_t i = 0; i < page.count; ++i)
    if (page.widgets[i].type == "hero") hasHero = true;
  if (isBookMetrics && !hasHero) y = drawBookIdentity(y, width);
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
  // Both book heroes use the same height. On dense pages, the metric/chart
  // rows share what remains instead of making the overview hero shorter.
  constexpr int kBookHeroHeight = 195;
  const int otherRows = std::max(0, widgetRows - (hasHero ? 1 : 0));
  const int heroHeight = hasHero ? std::min(kBookHeroHeight, budget) : 0;
  const int otherBudget = std::max(0, budget - heroHeight);
  const int slotHeight = std::max(0, std::min(widgetRows >= 3 ? 150 : 180,
                                            otherBudget / std::max(1, otherRows)));
  const bool chartAndMetric = page.count == 2 && page.widgets[0].type != "metric" && page.widgets[1].type == "metric";
  const int contentWidth = width - kMargin * 2;
  const int halfWidth = (contentWidth - kGap) / 2;
  for (int i = 0; i < page.count && y < bottom; ++i) {
    const auto& widget = page.widgets[i];
    if (widget.type == "metric") {
      const int h = std::max(0, std::min({chartAndMetric ? 150 : slotHeight, bottom - y}));
      if (i + 1 < page.count && page.widgets[i + 1].type == "metric") {
        drawWidget(widget, kMargin, y, halfWidth, h);
        drawWidget(page.widgets[++i], kMargin + halfWidth + kGap, y, contentWidth - halfWidth - kGap, h);
      } else {
        drawWidget(widget, kMargin, y, contentWidth, h);
      }
      y += h + kGap;
    } else {
      const int desired = widget.type == "hero"             ? heroHeight
                          : chartAndMetric                   ? std::min(260, std::max(0, available - 150 - kGap))
                          : widget.type == "legacy_import" ? 390
                          : hasHero                         ? slotHeight
                                                           : 160;
      const int h = std::max(0, std::min(desired, bottom - y));
      drawWidget(widget, kMargin, y, contentWidth, h);
      y += h + kGap;
    }
  }
  const char* confirmLabel = (booksPage() || importPage()) ? "Open" : "Refresh";
  const auto hints = mappedInput.mapLabels("Back", confirmLabel, "Previous", "Next");
  if (interactive) GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void StatsDashboardActivity::render(RenderLock&&) {
  drawDashboardPage(true);
  renderer.displayBuffer();
}

bool StatsDashboardActivity::renderSleepOverview() {
  // Not entered as an activity: never modify navigation/ActivityManager state.
  // Reuse the exact page-one widget renderer, but do not draw input hints.
  if (!loadManifest()) return false;
  activePage = 0;
  resolveBook();
  loadSeries();
  refresh();
  currentProgress = hasBookInfo ? readingstats::media::progress(bookInfo) : -1;
  currentAuthor = hasBookInfo ? readingstats::media::author(bookInfo) : std::string();
  if (hasBookInfo) readingstats::media::ensureCover(bookInfo, 142);
  drawDashboardPage(false);
  return true;
}
