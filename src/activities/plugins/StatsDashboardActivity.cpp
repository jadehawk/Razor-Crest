#include "StatsDashboardActivity.h"

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
#include <ctime>
#include <iterator>
#include <utility>
#include <vector>

#include "CrossPointState.h"
#include "DashboardCalendar.h"
#include "MappedInputManager.h"
#include "PluginCatalogActivity.h"
#include "RecentBooksStore.h"
#include "StatsImportActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/ReadingStatsBookMedia.h"
#include "util/ReadingStatsSnapshot.h"
#include "util/ReadingStatsStore.h"

namespace {
constexpr char kPersistentSyncToken[] = "/.crosspoint/reading-statistics-token.json";
constexpr char kUnifiedPluginName[] = "reading-statistics";

// Copy legacy companion credentials only if the unified token is absent.
// Do not remove or overwrite old credentials: users can verify the migration
// before uninstalling the separate companion.
void migrateLegacySyncState(const std::string& manifestPath) {
  const std::string dir = PluginLocations::findPluginDir(kUnifiedPluginName);
  if (dir.empty() || manifestPath != dir + "/device.json") return;
  const std::string newQueue = dir + "/events.jsonl";
  for (const char* root : PluginLocations::ROOTS) {
    const std::string legacyDir = std::string(root) + "/reading-stats-sync";
    std::string legacyToken;
    const bool validLegacy =
        pluginhttp::loadTokenFromFile(legacyDir + "/token.json", "token", legacyToken) && !legacyToken.empty();
    if (!Storage.exists(kPersistentSyncToken) && validLegacy &&
        pluginhttp::saveTokenToFile(kPersistentSyncToken, "token", legacyToken)) {
      LOG_INF("STATS", "Copied legacy Sync pairing to persistent storage");
    }
    std::string unifiedToken;
    const bool sameAccount = validLegacy &&
                             pluginhttp::loadTokenFromFile(kPersistentSyncToken, "token", unifiedToken) &&
                             legacyToken == unifiedToken;
    // Never move one guest's queued activity under another guest's token.
    // When the unified queue is empty, retain same-account pending sessions.
    // The old queue remains untouched and the server deduplicates event IDs.
    if (sameAccount && !Storage.exists(newQueue.c_str())) {
      std::string pending;
      if (Storage.readFileToString("STATS", legacyDir + "/events.jsonl", 4096, pending) && !pending.empty() &&
          Storage.writeFile(newQueue.c_str(), String(pending.c_str()))) {
        LOG_INF("STATS", "Copied same-account pending reading sessions");
      }
    }
    if (Storage.exists(kPersistentSyncToken) && Storage.exists(newQueue.c_str())) break;
  }
}
constexpr int kMargin = 12;
constexpr int kGap = 7;
// Statistic content keeps a consistent built-in font under SD UI themes.
constexpr int kStatsFont = NOTOSANS_12_FONT_ID;
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

struct CalendarContext {
  dashboardcalendar::Month summary{};
  unsigned month = 0;
  int year = 0;
};

CalendarContext currentCalendar(const readingstats::Snapshot& stats) {
  CalendarContext calendar;
  const time_t now = time(nullptr);
  struct tm local{};
  if (now <= 0 || !localtime_r(&now, &local) || local.tm_year + 1900 < 2020) return calendar;
  calendar.year = local.tm_year + 1900;
  calendar.month = static_cast<unsigned>(local.tm_mon + 1);
  calendar.summary =
      dashboardcalendar::summarizeMonth(stats, calendar.year, calendar.month, static_cast<unsigned>(local.tm_mday));
  return calendar;
}

Color calendarColor(uint8_t intensity) {
  switch (intensity) {
    case 1:
      return Color::LightGray;
    case 2:
      return Color::DarkGray;
    case 3:
      return Color::Black;
    default:
      return Color::White;
  }
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
  return strcmp(source, "reading-stats.v1") == 0 || strcmp(source, "cards.v1") == 0;
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
  genericCardDashboard = strcmp(source, "cards.v1") == 0;
  if (!genericCardDashboard && strcmp(source, "reading-stats.v1") != 0) return false;
  const char* header = doc["title"] | "Dashboard";
  title = header;
  const JsonArrayConst pageArray = doc["dashboard"]["pages"].as<JsonArrayConst>();
  if (pageArray.isNull() || pageArray.size() == 0 || pageArray.size() > pages.size() ||
      (genericCardDashboard && pageArray.size() != 1))
    return false;

  for (const JsonVariantConst pageJson : pageArray) {
    const JsonArrayConst widgets = pageJson["widgets"].as<JsonArrayConst>();
    if (widgets.isNull() || widgets.size() == 0 || widgets.size() > pages[0].widgets.size() ||
        (genericCardDashboard && widgets.size() != 1))
      return false;
    // The action page cannot be mixed with ordinary statistics widgets.
    bool syncWidget = false;
    for (const JsonVariantConst item : widgets)
      if (strcmp(item["type"] | "", "server_sync") == 0) syncWidget = true;
    if (syncWidget && widgets.size() != 1) return false;
    if (syncWidget) {
      const char* displayUrl = doc["auth"]["display_url"] | "";
      if (strlen(displayUrl) <= 128) pairingDisplayUrl = displayUrl;
    }
    Page& page = pages[pageCount++];
    page.title = pageJson["title"] | "Reading";
    for (const JsonVariantConst widgetJson : widgets) {
      Widget& widget = page.widgets[page.count++];
      widget.type = widgetJson["type"] | "";
      if (genericCardDashboard && widget.type != "card_carousel") return false;
      if (widget.type == "card_carousel") {
        if (carouselPage != 0xFF || page.count != 1) return false;  // one shared pager
        carouselPage = pageCount - 1;
        const JsonArrayConst cards = widgetJson["cards"].as<JsonArrayConst>();
        if (cards.isNull() || cards.size() == 0 || cards.size() > carouselCards.size()) return false;
        carouselPager = {cards.size(), 0, true};
        size_t index = 0;
        for (const JsonVariantConst raw : cards) {
          if (!raw.is<JsonObjectConst>()) return false;
          const char* title = raw["title"] | "";
          const char* subtitle = raw["subtitle"] | "";
          const char* value = raw["value"] | "";
          if (!*title || strlen(title) > 70 || strlen(subtitle) > 80 || strlen(value) > 48) return false;
          const int progress = raw["progress"] | -1;
          if (progress < -1 || progress > 100) return false;
          carouselCards[index++] = {title, subtitle, value, progress};
        }
      }
      widget.field = widgetJson["field"] | "";
      widget.showValue = widgetJson["show_value"] | true;
      if (!widgetJson["compact"].isNull() && (!widgetJson["compact"].is<bool>() || widget.type != "metric"))
        return false;
      widget.compact = widgetJson["compact"] | false;
      if (!widgetJson["period"].isNull() && !widgetJson["period"].is<const char*>()) return false;
      const char* period = widgetJson["period"] | "all";
      if (widget.type == "time_of_day") {
        if (strcmp(period, "all") != 0 && strcmp(period, "week") != 0) return false;
        widget.recentWeek = strcmp(period, "week") == 0;
      } else if (!widgetJson["period"].isNull()) {
        return false;
      }
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
      if (widget.type == "reading_snapshot" && widget.currentBook) return false;
      if (widget.type != "card_carousel" && widget.type != "metric" && widget.type != "week_bars" &&
          widget.type != "month_calendar" && widget.type != "month_insights" && widget.type != "book_list" &&
          widget.type != "legacy_import" && widget.type != "server_sync" && widget.type != "hero" &&
          widget.type != "reading_snapshot" && widget.type != "time_of_day")
        return false;
    }
  }
  return true;
}

void StatsDashboardActivity::resolveBook() {
  if (selectedBook) return;
  hasBookInfo = false;
  bookInfo = {};
  // Prefer the active reader path, then the persisted most-recent book.
  const auto& recents = RECENT_BOOKS.getBooks();
  const std::string& path =
      APP_STATE.openEpubPath.empty() && !recents.empty() ? recents.front().path : APP_STATE.openEpubPath;
  if (path.empty()) return;
  const RecentBook* recent = nullptr;
  for (const auto& item : recents)
    if (item.path == path) {
      recent = &item;
      break;
    }

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
  readingstats::loadTimeGlobal(timeGlobal);
  bookStatsAvailable =
      hasBookInfo && (bookInfo.hash() != 0 ? readingstats::loadBookHash(bookInfo.hash(), bookStats)
                                           : readingstats::loadBook(APP_STATE.openEpubPath.c_str(), bookStats));
  if (hasBookInfo) {
    if (bookInfo.hash() != 0)
      readingstats::loadTimeBookHash(bookInfo.hash(), timeBook);
    else
      readingstats::loadTimeBook(APP_STATE.openEpubPath.c_str(), timeBook);
  } else {
    timeBook = readingstats::TimeBuckets{};
  }
}

void StatsDashboardActivity::onEnter() {
  Activity::onEnter();
  if (!loadManifest()) {
    LOG_ERR("STATS", "Invalid local dashboard plugin: %s", manifestPath.c_str());
    finish();
    return;
  }
  if (genericCardDashboard) {
    requestUpdate();
    return;
  }
  const std::string statsPluginDir = PluginLocations::findPluginDir(kUnifiedPluginName);
  serverSyncAvailable = !statsPluginDir.empty() && manifestPath == statsPluginDir + "/device.json";
  if (serverSyncAvailable) migrateLegacySyncState(manifestPath);
  std::string savedToken;
  syncPaired = pluginhttp::loadTokenFromFile(kPersistentSyncToken, "token", savedToken) && !savedToken.empty();
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

bool StatsDashboardActivity::serverSyncPage() const {
  if (activePage >= pageCount || pages[activePage].count != 1 || pages[activePage].widgets[0].type != "server_sync")
    return false;
  return serverSyncAvailable;
}

void StatsDashboardActivity::openServerSync() {
  if (!serverSyncPage()) return;
  auto view = makeUniqueNoThrow<PluginCatalogActivity>(renderer, mappedInput, false, kUnifiedPluginName);
  if (!view) {
    LOG_ERR("STATS", "Out of memory opening server pairing");
    return;
  }
  startActivityForResult(std::move(view), [this](const ActivityResult&) {
    std::string token;
    syncPaired = pluginhttp::loadTokenFromFile(kPersistentSyncToken, "token", token) && !token.empty();
    requestUpdate();
  });
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
  return metrics.topPadding + metrics.headerHeight + 7;
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
  if (!booksPage() || !recordedBookCount || bookCursor < visibleStart || bookCursor >= visibleStart + visibleCount)
    return;
  const auto& book = visibleBooks[bookCursor - visibleStart].entry;
  const uint64_t hash = book.hash();
  if (!hash) return;
  const std::string subject = book.title[0] ? book.title : "Unnamed Book";
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, "Remove reading history?",
      "Remove saved statistics for " + subject + "? The book file and global totals stay intact.", "Cancel", "Remove",
      true);
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
    visibleStart = recordedBookCount
                       ? (bookCursor / static_cast<uint32_t>(listCapacity())) * static_cast<uint32_t>(listCapacity())
                       : 0;
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
    if (carouselPageActive()) {
      cyclePluginCard(1);
      return;
    }
    if (booksPage()) {
      openBooks();
      return;
    }
    if (importPage()) {
      openImport();
      return;
    }
    if (serverSyncPage()) {
      openServerSync();
      return;
    }
    refresh();
    currentProgress = hasBookInfo ? readingstats::media::progress(bookInfo) : -1;
    currentAuthor = hasBookInfo ? readingstats::media::author(bookInfo) : std::string();
    if (hasBookInfo) readingstats::media::ensureCover(bookInfo, 142);
    requestUpdate();
    return;
  }
  if (carouselPageActive() && carouselPager.canCycle()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      cyclePluginCard(1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      cyclePluginCard(-1);
      return;
    }
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
    if (y >= top && x >= kMargin && x < renderer.getScreenWidth() - kMargin && row >= 0 && row < visibleCount &&
        (y - top) % kBookRowStep < kBookRowStep - 4) {
      bookCursor = visibleStart + row;
      promptRemoveBook();
      return;
    }
  }
  const auto swipe = mappedInput.wasSwipe(&x, &y);
  if (carouselPageActive() && carouselPager.canCycle() && swipe != MappedInputManager::SwipeDir::None &&
      y >= dashboardContentTop() && x >= 44 && x < renderer.getScreenWidth() - 20 && !mappedInput.wasBackGesture()) {
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
      cyclePluginCard(swipe == MappedInputManager::SwipeDir::Left ? 1 : -1);
      return;
    }
  }
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
  if ((booksPage() || importPage() || serverSyncPage()) && mappedInput.wasScreenTapped(x, y)) {
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
      } else if (serverSyncPage()) {
        openServerSync();
        return;
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
  const int smallStep = renderer.getLineHeight(kStatsFont) + 2;
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
    drawLines(kStatsFont, std::string("Author: ") + bookInfo.author, 2, EpdFontFamily::REGULAR, smallStep);
  }
  if (!seriesName.empty()) {
    const std::string series = "Series: " + seriesName + (seriesIndex.empty() ? "" : " #" + seriesIndex);
    drawLines(kStatsFont, series, 3, EpdFontFamily::REGULAR, smallStep);
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
      drawn = renderer.drawBitmap(bitmap, rect.x + (rect.width - fittedW) / 2, rect.y + (rect.height - fittedH) / 2,
                                  rect.width, rect.height, 0.0f, 0.0f, false, true);
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
      renderer.drawText(kStatsFont, x + inset, bodyY, "No book selected");
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
    const int titleLine = renderer.getLineHeight(kStatsFont);
    for (const auto& line : renderer.wrappedText(kStatsFont, bookTitle, textW, 2, EpdFontFamily::BOLD)) {
      if (cursor + titleLine > textBottom) break;
      renderer.drawText(kStatsFont, textX, cursor, line.c_str(), true, EpdFontFamily::BOLD);
      cursor += titleLine + 2;
    }
    const std::string& bookAuthor = currentAuthor;
    const int authorLine = renderer.getLineHeight(kStatsFont);
    if (!bookAuthor.empty() && cursor + authorLine <= textBottom) {
      renderer.drawText(kStatsFont, textX, cursor,
                        renderer.truncatedText(kStatsFont, bookAuthor.c_str(), textW).c_str());
      cursor += authorLine + 2;
    }
    const std::string series = seriesName + (seriesIndex.empty() ? "" : " #" + seriesIndex);
    if (!seriesName.empty() && cursor + renderer.getLineHeight(kStatsFont) <= textBottom) {
      renderer.drawText(kStatsFont, textX, cursor, renderer.truncatedText(kStatsFont, series.c_str(), textW).c_str());
    }

    const int barX = x + inset;
    const int barY = y + h - 24;
    const int barW = std::max(20, w - 2 * inset - 59);
    renderer.drawRect(barX, barY, barW, 11, true);
    if (currentProgress > 0) renderer.fillRect(barX + 2, barY + 2, (barW - 4) * currentProgress / 100, 7, true);
    char percentage[12];
    if (currentProgress >= 0)
      snprintf(percentage, sizeof(percentage), "%d%%", currentProgress);
    else
      snprintf(percentage, sizeof(percentage), "--");
    // Text is taller than the 11px bar; raise it to align the visual centers.
    const int textY = barY - (renderer.getLineHeight(kStatsFont) - 11) / 2 - 6;
    renderer.drawText(kStatsFont, barX + barW + 9, textY, percentage, true, EpdFontFamily::BOLD);
  } else if (widget.showValue) {
    const std::string value = metricValue(widget);
    renderer.drawText(
        NOTOSANS_14_FONT_ID, x + inset, bodyY + 5,
        renderer.truncatedText(NOTOSANS_14_FONT_ID, value.c_str(), w - 2 * inset, EpdFontFamily::BOLD).c_str(), true,
        EpdFontFamily::BOLD);
  }
}

void StatsDashboardActivity::drawReadingSnapshot(const Widget& widget, const int x, const int y, const int w,
                                                 const int h) const {
  if (w <= 0 || h < 100) return;
  drawCardBorder(widget, x, y, w, h);

  // Monthly totals are relative to the device's local calendar, not the last
  // recorded reading day; the calendar can advance without new sessions.
  const time_t now = time(nullptr);
  struct tm local{};
  uint32_t today = 0, firstDay = 0;
  if (now > 0 && localtime_r(&now, &local) && local.tm_year + 1900 >= 2020) {
    const int year = local.tm_year + 1900;
    const unsigned month = static_cast<unsigned>(local.tm_mon + 1);
    today = readingstats::snapshot::civilDay(year, month, static_cast<unsigned>(local.tm_mday));
    firstDay = readingstats::snapshot::civilDay(year, month, 1);
  }
  const auto summary = readingstats::snapshot::summarize(stats, today, firstDay);
  const auto center = [&](const int font, const std::string& value, const int centerX, const int top,
                          const int maxWidth, const EpdFontFamily::Style style) {
    const std::string fitted = renderer.truncatedText(font, value.c_str(), maxWidth, style);
    renderer.drawText(font, centerX - renderer.getTextWidth(font, fitted.c_str(), style) / 2, top, fitted.c_str(), true,
                      style);
  };
  center(kStatsFont, "THIS MONTH", x + w / 2, y + 12, w - 20, EpdFontFamily::BOLD);
  center(NOTOSANS_18_FONT_ID, summary.dateKnown ? duration(summary.monthSeconds) : "--", x + w / 2,
         y + 19 + renderer.getLineHeight(kStatsFont), w - 20, EpdFontFamily::BOLD);
  if (!summary.dateKnown)
    center(kStatsFont, "Set device date to see monthly activity", x + w / 2, y + h / 2 - 14, w - 20,
           EpdFontFamily::REGULAR);

  // Keep the three labels comfortably inside the card's bottom border on
  // both X3 and Picco. The old 13px/5px offsets let the final text line touch
  // the rounded border even though the row has enough room above it.
  const int dividerY = y + h / 2 + 8;
  renderer.drawLine(x + 12, dividerY, x + w - 12, dividerY);
  const int columnWidth = (w - 24) / 3;
  const int valueY = dividerY + 8;
  const int labelY = valueY + renderer.getLineHeight(NOTOSANS_14_FONT_ID) + 2;
  const std::string values[] = {summary.dateKnown ? std::to_string(summary.streak) : "--",
                                summary.dateKnown ? std::to_string(summary.activeDays) : "--",
                                std::to_string(recordedBookCount)};
  const char* labels[] = {"Day Streak", "Active Days", "Books"};
  const int labelWidth = columnWidth - 4;
  for (int i = 1; i < 3; ++i) {
    const int dividerX = x + 12 + i * columnWidth;
    renderer.drawLine(dividerX, dividerY + 10, dividerX, y + h - 10);
  }
  for (int i = 0; i < 3; ++i) {
    const int cx = x + 12 + i * columnWidth + columnWidth / 2;
    center(NOTOSANS_14_FONT_ID, values[i], cx, valueY, labelWidth, EpdFontFamily::BOLD);
    center(kStatsFont, labels[i], cx, labelY, labelWidth, EpdFontFamily::REGULAR);
  }
}

void StatsDashboardActivity::drawMetric(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (height <= 0 || w <= 0) return;
  drawCardBorder(widget, x, y, w, height);
  if (widget.compact) {
    // Give compact labels the full text column; reserve a common two-line
    // heading slot so values align even when one heading wraps.
    const int dividerX = x + std::min(w * 3 / 10, 76);
    const int iconSide = std::max(0, std::min({height - 16, dividerX - x - 14, 68}));
    if (widget.iconIndex >= 0 && iconSide > 0)
      drawCardIcon(widget, x + (dividerX - x - iconSide) / 2, y + (height - iconSide) / 2, iconSide);
    renderer.drawLine(dividerX, y + 12, dividerX, y + height - 12);
    const int textX = dividerX + 5, textWidth = std::max(1, x + w - textX - 5);
    const bool labelFits = renderer.getTextWidth(kStatsFont, widget.label.c_str(), EpdFontFamily::BOLD) <= textWidth;
    const auto labelLines =
        labelFits ? std::vector<std::string>{widget.label}
                  : renderer.wrappedText(kStatsFont, widget.label.c_str(), textWidth, 2, EpdFontFamily::BOLD);
    const std::string value = metricValue(widget);
    const int valueFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, value.c_str(), EpdFontFamily::BOLD) <= textWidth
                              ? NOTOSANS_14_FONT_ID
                              : kStatsFont;
    const std::string fitted = renderer.truncatedText(valueFont, value.c_str(), textWidth, EpdFontFamily::BOLD);
    const int labelH = renderer.getLineHeight(kStatsFont);
    const int valueH = renderer.getLineHeight(valueFont);
    const int labelSlotH = labelH * 2 + 2;
    const int top = y + std::max(4, (height - labelSlotH - valueH - 4) / 2);
    const int labelTop = top + (labelLines.size() == 1 ? (labelSlotH - labelH) / 2 : 0);
    for (size_t i = 0; i < labelLines.size(); ++i) {
      const std::string& line = labelLines[i];
      renderer.drawText(kStatsFont,
                        textX + (textWidth - renderer.getTextWidth(kStatsFont, line.c_str(), EpdFontFamily::BOLD)) / 2,
                        labelTop + static_cast<int>(i) * (labelH + 2), line.c_str(), true, EpdFontFamily::BOLD);
    }
    renderer.drawText(valueFont,
                      textX + (textWidth - renderer.getTextWidth(valueFont, fitted.c_str(), EpdFontFamily::BOLD)) / 2,
                      top + labelSlotH + 4, fitted.c_str(), true, EpdFontFamily::BOLD);
    return;
  }
  if (widget.iconIndex >= 0) {
    constexpr int inset = 10;
    constexpr int valueGap = 9;
    constexpr int minValueWidth = 64;
    const int labelHeight = renderer.getLineHeight(kStatsFont);
    const int labelY = y + 11;
    const std::string label =
        renderer.truncatedText(kStatsFont, widget.label.c_str(), w - 2 * inset, EpdFontFamily::BOLD);
    const int labelWidth = renderer.getTextWidth(kStatsFont, label.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(kStatsFont, x + (w - labelWidth) / 2, labelY, label.c_str(), true, EpdFontFamily::BOLD);

    // The centered heading owns the card's top row. The icon fills as much
    // of the remaining height as possible, leaving room for a readable value.
    const int bodyTop = labelY + labelHeight + 8;
    const int bodyHeight = std::max(0, y + height - inset - bodyTop);
    const int iconSide = std::max(0, std::min({bodyHeight, w - 2 * inset - valueGap - minValueWidth,
                                               widget.iconSize ? static_cast<int>(widget.iconSize) : 96}));
    if (iconSide > 0) drawCardIcon(widget, x + inset, bodyTop + (bodyHeight - iconSide) / 2, iconSide);
    const int valueX = x + inset + iconSide + valueGap;
    const int valueWidth = std::max(12, x + w - inset - valueX);
    const std::string value = metricValue(widget);
    const int valueFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, value.c_str(), EpdFontFamily::BOLD) <= valueWidth
                              ? NOTOSANS_14_FONT_ID
                              : kStatsFont;
    const std::string fitted = renderer.truncatedText(valueFont, value.c_str(), valueWidth, EpdFontFamily::BOLD);
    const int fittedWidth = renderer.getTextWidth(valueFont, fitted.c_str(), EpdFontFamily::BOLD);
    const int fittedHeight = renderer.getLineHeight(valueFont);
    if (bodyHeight >= fittedHeight)
      renderer.drawText(valueFont, valueX + (valueWidth - fittedWidth) / 2, bodyTop + (bodyHeight - fittedHeight) / 2,
                        fitted.c_str(), true, EpdFontFamily::BOLD);
    return;
  }
  constexpr int inset = 8;
  const int labelStep = renderer.getLineHeight(kStatsFont) + 1;
  const auto labels = renderer.wrappedText(kStatsFont, widget.label.c_str(), std::max(1, w - 2 * inset), 2);
  int labelY = y + 8;
  for (const auto& line : labels) {
    const int lineWidth = renderer.getTextWidth(kStatsFont, line.c_str());
    renderer.drawText(kStatsFont, x + (w - lineWidth) / 2, labelY, line.c_str());
    labelY += labelStep;
  }
  const std::string value = metricValue(widget);
  const int valueFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, value.c_str(), EpdFontFamily::BOLD) <= w - 2 * inset
                            ? NOTOSANS_14_FONT_ID
                            : kStatsFont;
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

void StatsDashboardActivity::drawTimeOfDay(const Widget& widget, const int x, const int y, const int w,
                                           const int h) const {
  if (w < 130 || h < 82) return;
  drawCardBorder(widget, x, y, w, h);
  const std::string heading = widget.label.empty() ? "Time of Day" : widget.label;
  const std::string title = renderer.truncatedText(kStatsFont, heading.c_str(), w - 20, EpdFontFamily::BOLD);
  renderer.drawText(kStatsFont, x + (w - renderer.getTextWidth(kStatsFont, title.c_str(), EpdFontFamily::BOLD)) / 2,
                    y + 7, title.c_str(), true, EpdFontFamily::BOLD);
  const int titleBottom = y + 9 + renderer.getLineHeight(kStatsFont);
  renderer.drawLine(x + 10, titleBottom, x + w - 10, titleBottom);

  const auto& buckets = widget.currentBook ? timeBook : timeGlobal;
  uint64_t seconds[4] = {};
  if (widget.recentWeek) {
    // The companion seven-day chart ends on the last recorded reading day.
    const uint32_t latestDay = statsFor(widget).latestDay;
    for (int i = 0; i < 4; ++i) seconds[i] = buckets.secondsInLastDays(latestDay, static_cast<uint8_t>(i));
  } else {
    for (int i = 0; i < 4; ++i) seconds[i] = buckets.total[i];
  }
  const uint64_t maxSeconds = *std::max_element(std::begin(seconds), std::end(seconds));
  if (!maxSeconds) {
    const char* empty = widget.recentWeek ? "No timed sessions in 7 days" : "No timed sessions yet";
    renderer.drawText(kStatsFont, x + (w - renderer.getTextWidth(kStatsFont, empty)) / 2, y + (h + titleBottom - y) / 2,
                      empty);
    return;
  }
  constexpr const char* labels[] = {"Morning", "Afternoon", "Evening", "Night"};
  int labelWidth = 0;
  for (const char* label : labels) labelWidth = std::max(labelWidth, renderer.getTextWidth(kStatsFont, label));
  const int labelX = x + 11;
  const int barX = labelX + labelWidth + 11;
  const int barWidth = std::max(1, x + w - 12 - barX);
  const int chartTop = titleBottom + 7;
  const int rowHeight = std::max(12, (y + h - 8 - chartTop) / 4);
  const int barHeight = std::min(12, std::max(4, rowHeight / 2));
  for (int i = 0; i < 4; ++i) {
    const int centerY = chartTop + i * rowHeight + rowHeight / 2;
    if (centerY + barHeight / 2 > y + h - 5) break;
    renderer.drawText(kStatsFont, labelX, centerY - renderer.getLineHeight(kStatsFont) / 2, labels[i]);
    const int filled = static_cast<int>((seconds[i] * barWidth) / maxSeconds);
    if (filled > 0) renderer.fillRect(barX, centerY - barHeight / 2, filled, barHeight, true);
  }
}

void StatsDashboardActivity::drawWeek(const Widget& widget, const int x, const int y, const int w, const int h) const {
  if (w <= 24 || h <= 52) return;
  drawCardBorder(widget, x, y, w, h);
  const std::string label = renderer.truncatedText(kStatsFont, widget.label.c_str(), w - 16);
  renderer.drawText(kStatsFont, x + (w - renderer.getTextWidth(kStatsFont, label.c_str())) / 2, y + 7, label.c_str());
  const auto& stats = statsFor(widget);
  const int chartTop = y + renderer.getLineHeight(kStatsFont) + 17;
  const int baseline = y + h - renderer.getLineHeight(kStatsFont) - 12;
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
      renderer.drawText(kStatsFont, centerX - renderer.getTextWidth(kStatsFont, name) / 2, baseline + 5, name);
    }
  }
}

void StatsDashboardActivity::drawMonthCalendar(const Widget& widget, const int x, const int y, const int w,
                                               const int h) const {
  if (w < 180 || h < 190) return;
  drawCardBorder(widget, x, y, w, h);
  const CalendarContext calendar = currentCalendar(statsFor(widget));
  if (!calendar.summary.valid()) {
    renderer.drawText(kStatsFont, x + 12, y + 20, "Set device date to see calendar");
    return;
  }
  constexpr const char* months[] = {"January", "February", "March",     "April",   "May",      "June",
                                    "July",    "August",   "September", "October", "November", "December"};
  constexpr const char* weekdays[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  constexpr const char* legend[] = {"None", "<15m", "15-44m", "45m+"};
  char heading[32];
  snprintf(heading, sizeof(heading), "%s %d", months[calendar.month - 1], calendar.year);
  const int fontH = renderer.getLineHeight(kStatsFont);
  const int headingY = y + 6;
  const int headingW = renderer.getTextWidth(kStatsFont, heading, EpdFontFamily::BOLD);
  renderer.drawText(kStatsFont, x + (w - headingW) / 2, headingY, heading, true, EpdFontFamily::BOLD);
  const int dividerY = headingY + fontH + 3;
  renderer.drawLine(x + 10, dividerY, x + w - 10, dividerY);

  const int headerY = dividerY + 5;
  const int gridTop = headerY + fontH + 2;
  const int legendY = y + h - fontH - 6;
  constexpr int gap = 3;
  const int cellW = (w - 24 - 6 * gap) / 7;
  const int spaceH = legendY - gridTop - 4 - (calendar.summary.rows - 1) * gap;
  const int cellH = std::min(cellW, spaceH / calendar.summary.rows);
  if (cellW < 21 || cellH < fontH) return;

  const int gridW = cellW * 7 + 6 * gap;
  const int gridX = x + (w - gridW) / 2;
  for (int col = 0; col < 7; ++col) {
    const int cx = gridX + col * (cellW + gap) + cellW / 2;
    renderer.drawText(kStatsFont, cx - renderer.getTextWidth(kStatsFont, weekdays[col]) / 2, headerY, weekdays[col]);
  }
  const auto& month = calendar.summary;
  for (int date = 1; date <= month.days; ++date) {
    const int index = month.offset + date - 1;
    const int boxX = gridX + (index % 7) * (cellW + gap);
    const int boxY = gridTop + (index / 7) * (cellH + gap);
    const uint32_t seconds = date <= month.elapsedDays ? statsFor(widget).secondsOnDay(month.firstDay + date - 1) : 0;
    const uint8_t shade = dashboardcalendar::intensity(seconds);
    if (shade != 0) renderer.fillRectDither(boxX + 2, boxY + 2, cellW - 4, cellH - 4, calendarColor(shade));
    renderer.drawRect(boxX, boxY, cellW, cellH, date == month.elapsedDays ? 2 : 1, true);
    char day[4];
    snprintf(day, sizeof(day), "%d", date);
    renderer.drawText(kStatsFont, boxX + (cellW - renderer.getTextWidth(kStatsFont, day)) / 2,
                      boxY + (cellH - fontH) / 2, day, shade < 2);
  }

  const int groupW = (w - 20) / 4;
  for (int i = 0; i < 4; ++i) {
    const int legendX = x + 10 + groupW * i;
    const int boxY = legendY + (fontH - 13) / 2;
    if (i != 0) renderer.fillRectDither(legendX, boxY, 13, 13, calendarColor(i));
    renderer.drawRect(legendX, boxY, 13, 13);
    renderer.drawText(kStatsFont, legendX + 17, legendY, legend[i]);
  }
}

void StatsDashboardActivity::drawMonthInsights(const Widget& widget, const int x, const int y, const int w,
                                               const int h) const {
  if (w < 180 || h < 120) return;
  const CalendarContext calendar = currentCalendar(statsFor(widget));
  const auto& month = calendar.summary;
  const int gap = kGap;
  const int halfW = (w - gap) / 2;
  const int labelH = renderer.getLineHeight(kStatsFont);
  const int valueH = renderer.getLineHeight(NOTOSANS_14_FONT_ID);
  const int fullBlockH = labelH * 2 + valueH + 8;
  const int minBottomH = labelH * 2 + 30;
  const int topH = std::min(fullBlockH + 12, std::max(72, h - minBottomH - gap));
  const int bottomY = y + topH + gap;
  const int bottomH = h - topH - gap;
  const bool fullLabels = topH >= fullBlockH + 10;
  const int blockH = fullLabels ? fullBlockH : labelH + valueH + 4;
  const int cardY = y + std::max(4, (topH - blockH) / 2);
  const int valueY = cardY + labelH + 4;

  const auto centered = [&](const int font, const std::string& value, const int left, const int top, const int width,
                            const EpdFontFamily::Style style) {
    const std::string fitted = renderer.truncatedText(font, value.c_str(), width - 12, style);
    renderer.drawText(font, left + (width - renderer.getTextWidth(font, fitted.c_str(), style)) / 2, top,
                      fitted.c_str(), true, style);
  };
  constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char bestDate[24];
  if (month.bestDay != 0)
    snprintf(bestDate, sizeof(bestDate), "%s %u", months[calendar.month - 1], static_cast<unsigned>(month.bestDay));
  else
    snprintf(bestDate, sizeof(bestDate), "No reading yet");

  drawCardBorder(widget, x, y, halfW, topH);
  const int rightX = x + halfW + gap;
  const int rightW = w - halfW - gap;
  drawCardBorder(widget, rightX, y, rightW, topH);
  centered(kStatsFont, "Best Day", x, cardY, halfW, EpdFontFamily::BOLD);
  centered(kStatsFont, "Daily Average", rightX, cardY, rightW, EpdFontFamily::BOLD);

  if (fullLabels) {
    centered(NOTOSANS_14_FONT_ID, month.bestDay ? duration(month.bestSeconds) : "--", x, valueY, halfW,
             EpdFontFamily::BOLD);
    centered(kStatsFont, month.valid() ? bestDate : "--", x, valueY + valueH + 4, halfW, EpdFontFamily::REGULAR);
    centered(NOTOSANS_14_FONT_ID, month.activeDays ? duration(month.averageSeconds()) : "--", rightX, valueY, rightW,
             EpdFontFamily::BOLD);
    centered(kStatsFont, "Per active day", rightX, valueY + valueH + 4, rightW, EpdFontFamily::REGULAR);
  } else {
    // Two lines fit even in the shorter X3 cards; no caption is painted
    // across the adjacent consistency card.
    const std::string best = month.bestDay ? std::string(bestDate) + " - " + duration(month.bestSeconds) : "--";
    const int bestFont = renderer.getTextWidth(NOTOSANS_14_FONT_ID, best.c_str(), EpdFontFamily::BOLD) <= halfW - 12
                             ? NOTOSANS_14_FONT_ID
                             : kStatsFont;
    centered(bestFont, best, x, valueY, halfW, EpdFontFamily::BOLD);
    const std::string average = month.activeDays ? duration(month.averageSeconds()) + " / read day" : "--";
    centered(kStatsFont, average, rightX, valueY, rightW, EpdFontFamily::BOLD);
  }

  if (bottomH < 65) return;
  drawCardBorder(widget, x, bottomY, w, bottomH);
  const int contentH = labelH * 2 + 17;
  const int contentY = bottomY + std::max(3, (bottomH - contentH) / 2);
  renderer.drawText(kStatsFont, x + 13, contentY, "Reading Consistency", true, EpdFontFamily::BOLD);
  const std::string percent = month.valid() ? std::to_string(month.consistencyPercent()) + "%" : "--";
  renderer.drawText(NOTOSANS_14_FONT_ID,
                    x + w - 13 - renderer.getTextWidth(NOTOSANS_14_FONT_ID, percent.c_str(), EpdFontFamily::BOLD),
                    contentY - 2, percent.c_str(), true, EpdFontFamily::BOLD);
  const int barX = x + 13, barW = w - 26, barY = contentY + labelH + 4;
  renderer.drawRect(barX, barY, barW, 10);
  const int filled = month.valid() ? (barW - 4) * month.consistencyPercent() / 100 : 0;
  if (filled > 0) renderer.fillRect(barX + 2, barY + 2, filled, 6, true);
  char note[48];
  if (month.valid())
    snprintf(note, sizeof(note), "%u of %u days read", static_cast<unsigned>(month.activeDays),
             static_cast<unsigned>(month.elapsedDays));
  else
    snprintf(note, sizeof(note), "Set device date");
  centered(kStatsFont, note, x, barY + 13, w, EpdFontFamily::REGULAR);
}

void StatsDashboardActivity::drawLegacyImport(const int x, const int y, const int w, const int h) const {
  if (h <= 0) return;
  renderer.drawRect(x, y, w, h, true);
  constexpr int inset = 9;
  const int step = renderer.getLineHeight(kStatsFont) + 3;
  const int rowHeight = std::max(54, (h - 3 * inset - 2 * step - 12) / 2);
  auto source = [&](int top, const char* name, const char* filename, const readingstats::legacy::Preview& preview,
                    bool showTurns) {
    renderer.drawRect(x + inset, top, w - 2 * inset, rowHeight, true);
    int line = top + 7;
    auto textLine = [&](const char* text, bool bold = false) {
      if (line + step > top + rowHeight - 2) return;
      const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const std::string fitted = renderer.truncatedText(kStatsFont, text, w - 4 * inset, style);
      renderer.drawText(kStatsFont, x + inset * 2, line, fitted.c_str(), true, style);
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
    renderer.drawText(kStatsFont, x + inset, footer, "Preview only; sources are unchanged.");
    renderer.drawText(kStatsFont, x + inset, footer + step, "Select to review / import history.");
  }
}

void StatsDashboardActivity::drawBookList(int top, int bottom) const {
  const int width = renderer.getScreenWidth();
  if (!recordedBookCount) {
    renderer.drawText(kStatsFont, kMargin, top + 20, "No recorded books yet");
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
    for (const auto& line : renderer.wrappedText(kStatsFont, name, textW, 2, EpdFontFamily::BOLD)) {
      if (cursor + renderer.getLineHeight(kStatsFont) > y + h - 31) break;
      renderer.drawText(kStatsFont, textX, cursor, line.c_str(), true, EpdFontFamily::BOLD);
      cursor += renderer.getLineHeight(kStatsFont);
    }
    if (row.displayAuthor[0] && cursor + renderer.getLineHeight(kStatsFont) <= y + h - 19)
      renderer.drawText(kStatsFont, textX, cursor,
                        renderer.truncatedText(kStatsFont, row.displayAuthor, textW).c_str());
    const std::string details =
        duration(row.seconds) + " read  |  " + (row.progress >= 0 ? std::to_string(row.progress) + "%" : "--");
    renderer.drawText(kStatsFont, textX, y + h - renderer.getLineHeight(kStatsFont) - 6,
                      renderer.truncatedText(kStatsFont, details.c_str(), textW).c_str());
  }
  char count[56];
  snprintf(count, sizeof(count), "%lu-%lu of %lu books", static_cast<unsigned long>(visibleStart + 1),
           static_cast<unsigned long>(visibleStart + visibleCount), static_cast<unsigned long>(recordedBookCount));
  const int footer = top + visibleCount * kBookRowStep + 3;
  const int noteHeight = renderer.getLineHeight(kStatsFont);
  if (footer + noteHeight < bottom) renderer.drawText(kStatsFont, kMargin, footer, count);
  if (footer + noteHeight * 2 + 4 < bottom)
    renderer.drawText(kStatsFont, kMargin, footer + noteHeight + 4, "Hold Open to remove selected book history");
}

void StatsDashboardActivity::cyclePluginCard(const int direction) {
  if (!carouselPageActive() || !carouselPager.canCycle()) return;
  carouselPager.move(direction);
  requestUpdate();
}

void StatsDashboardActivity::drawPluginCard(const Widget& widget, const int x, const int y, const int w,
                                            const int h) const {
  if (!carouselPageActive() || h < 65) return;
  const auto& item = carouselCards[carouselPager.current()];
  drawCardBorder(widget, x, y, w, h);
  const int left = x + 17, maxText = std::max(10, w - 34);
  renderer.drawText(kStatsFont, left, y + 20,
                    renderer.truncatedText(kStatsFont, item.title.c_str(), maxText, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  if (!item.subtitle.empty())
    renderer.drawText(kStatsFont, left, y + 54,
                      renderer.truncatedText(kStatsFont, item.subtitle.c_str(), maxText).c_str(), true);
  if (!item.value.empty())
    renderer.drawText(kStatsFont, left, y + 88, renderer.truncatedText(kStatsFont, item.value.c_str(), maxText).c_str(),
                      true);
  if (item.progress >= 0) {
    const int barW = std::max(20, w - 104), barY = y + h - 38;
    renderer.drawRect(left, barY, barW, 12, true);
    if (item.progress > 0) renderer.fillRect(left + 2, barY + 2, (barW - 4) * item.progress / 100, 8, true);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", item.progress);
    renderer.drawText(kStatsFont, left + barW + 8, barY - 4, percent);
  }
  if (carouselPager.canCycle()) {
    // Thick focus rails frame ONE complete card; never partial neighbors.
    renderer.fillRect(x + 2, y + 4, 7, h - 8, true);
    renderer.fillRect(x + w - 9, y + 4, 7, h - 8, true);
    char count[24];
    snprintf(count, sizeof(count), "%u / %u", static_cast<unsigned>(carouselPager.current() + 1),
             static_cast<unsigned>(carouselPager.size));
    renderer.drawText(kStatsFont, x + w - 76, y + h - 25, count);
  }
}

void StatsDashboardActivity::drawServerSync(const int x, const int y, const int w, const int h) const {
  if (h <= 0) return;
  renderer.drawRoundedRect(x, y, w, h, 1, 9, true);
  const int left = x + 14;
  const int textWidth = std::max(20, w - 28);
  const int step = renderer.getLineHeight(kStatsFont) + 9;
  int top = y + 13;
  const char* lines[] = {
      "Techy-Notes Reading Sync",
      pairingDisplayUrl.empty() ? nullptr : pairingDisplayUrl.c_str(),
      syncPaired ? "Reader paired" : "Reader not paired",
      "KOSync is unchanged",
      "Select Open to manage server sync",
  };
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]) && top + step <= y + h; ++i) {
    if (!lines[i]) continue;
    const auto weight = i == 0 || i == 2 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string fitted = renderer.truncatedText(kStatsFont, lines[i], textWidth, weight);
    renderer.drawText(kStatsFont, left, top, fitted.c_str(), true, weight);
    top += step;
  }
}

void StatsDashboardActivity::drawWidget(const Widget& widget, const int x, const int y, const int w,
                                        const int height) const {
  if (widget.type == "metric")
    drawMetric(widget, x, y, w, height);
  else if (widget.type == "hero")
    drawHero(widget, x, y, w, height);
  else if (widget.type == "reading_snapshot")
    drawReadingSnapshot(widget, x, y, w, height);
  else if (widget.type == "card_carousel")
    drawPluginCard(widget, x, y, w, height);
  else if (widget.type == "week_bars")
    drawWeek(widget, x, y, w, height);
  else if (widget.type == "time_of_day")
    drawTimeOfDay(widget, x, y, w, height);
  else if (widget.type == "month_calendar")
    drawMonthCalendar(widget, x, y, w, height);
  else if (widget.type == "month_insights")
    drawMonthInsights(widget, x, y, w, height);
  else if (widget.type == "legacy_import")
    drawLegacyImport(x, y, w, height);
  else if (widget.type == "server_sync")
    drawServerSync(x, y, w, height);
}

void StatsDashboardActivity::drawDashboardPage(bool interactive) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  // Sleep is a noninteractive wallpaper: no navigation header, Back control,
  // or bottom button-hint bar. Use their space for the actual statistics.
  const int bottom = renderer.getScreenHeight() - (interactive ? metrics.buttonHintsHeight : 0) - 6;
  if (!pageCount) return;
  const Page& page = pages[activePage];
  // The interactive page title belongs to the native header; the normal
  // theme still paints its emblem/Back target and the clock/battery strip.
  // Sleep has no header, so retain its centered title above the widgets.
  int y = metrics.topPadding + (interactive ? metrics.headerHeight : 0) + 7;
  if (interactive) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, page.title.c_str(), nullptr,
                   true, kStatsFont);
  } else {
    const std::string pageTitle =
        renderer.truncatedText(kStatsFont, page.title.c_str(), width - kMargin * 2, EpdFontFamily::BOLD);
    renderer.drawText(kStatsFont,
                      (width - renderer.getTextWidth(kStatsFont, pageTitle.c_str(), EpdFontFamily::BOLD)) / 2, y,
                      pageTitle.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(kStatsFont) + 9;
  }
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
  bool hasFeaturedCard = false;
  for (uint8_t i = 0; i < page.count; ++i)
    if (page.widgets[i].type == "hero" || page.widgets[i].type == "reading_snapshot") hasFeaturedCard = true;
  if (isBookMetrics && !hasFeaturedCard) y = drawBookIdentity(y, width);
  const bool monthPage =
      page.count == 2 && page.widgets[0].type == "month_calendar" && page.widgets[1].type == "month_insights";
  if (!statsAvailable && !isBookMetrics && !importPage() && !serverSyncPage() && !genericCardDashboard && !monthPage) {
    renderer.drawText(kStatsFont, kMargin, y, "No saved reading sessions");
    y += renderer.getLineHeight(kStatsFont) + 5;
  }
  int widgetRows = 0;
  for (int i = 0; i < page.count; ++i) {
    ++widgetRows;
    if (page.widgets[i].type == "metric" && i + 1 < page.count && page.widgets[i + 1].type == "metric") ++i;
  }
  const int available = std::max(0, bottom - y);
  const int budget = std::max(0, available - kGap * std::max(0, widgetRows - 1));
  // Share a 195-pixel featured card between the overview and book detail,
  // leaving the remaining height for metrics and the seven-day chart.
  constexpr int kFeaturedHeight = 195;
  const int otherRows = std::max(0, widgetRows - (hasFeaturedCard ? 1 : 0));
  const int featuredHeight = hasFeaturedCard ? std::min(kFeaturedHeight, budget) : 0;
  const int otherBudget = std::max(0, budget - featuredHeight);
  const int slotHeight = std::max(0, std::min(widgetRows >= 3 ? 150 : 180, otherBudget / std::max(1, otherRows)));
  const bool chartAndMetric = page.count == 2 && page.widgets[0].type != "metric" && page.widgets[1].type == "metric";
  const int contentWidth = width - kMargin * 2;
  const int halfWidth = (contentWidth - kGap) / 2;
  const int monthHeight = monthPage ? std::max(0, std::min({available * 59 / 100, available - 160 - kGap, 360})) : 0;
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
      const int desired = widget.type == "month_calendar"                              ? monthHeight
                          : widget.type == "month_insights"                            ? bottom - y
                          : widget.type == "card_carousel"                             ? 195
                          : widget.type == "hero" || widget.type == "reading_snapshot" ? featuredHeight
                          : chartAndMetric                 ? std::min(260, std::max(0, available - 150 - kGap))
                          : widget.type == "legacy_import" ? 390
                          : hasFeaturedCard                ? slotHeight
                                                           : 160;
      const int h = std::max(0, std::min(desired, bottom - y));
      drawWidget(widget, kMargin, y, contentWidth, h);
      y += h + kGap;
    }
  }
  const char* confirmLabel = carouselPageActive()                                ? "Next Card"
                             : (booksPage() || importPage() || serverSyncPage()) ? "Open"
                                                                                 : "Refresh";
  const auto hints = mappedInput.mapLabels("Back", confirmLabel, "Previous", "Next");
  if (interactive) GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void StatsDashboardActivity::render(RenderLock&&) {
  drawDashboardPage(true);
  renderer.displayBuffer();
}
