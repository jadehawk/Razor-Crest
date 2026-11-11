#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibrarySeriesFormat.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ProgressComparison.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "clippings/ClippingSync.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"  // list icons for the compare rows
#include "fontIds.h"
#include "network/WifiPowerSaveGuard.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/ReadingStatsStore.h"

namespace fui = freeink::ui;

namespace {
// One action id for both interactive states: the compare rows (SHOWING_RESULT)
// and the upload button (NO_REMOTE_PROGRESS) never coexist, so state
// disambiguates them in the handler.
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_CARD = 2;

// Comparison-card layout adapted from Julia Nguyen's CrossInk (MIT license).
// Source: https://github.com/uxjulia/CrossInk, commit 1b1156027.
// CrossInk license notice: docs/attribution/crossink-koreader-sync.md.
constexpr int CARD_BORDER_WIDTH = 2;
constexpr int CARD_PADDING = 12;
constexpr int CARD_GAP = 6;
constexpr int BAR_HEIGHT = 10;
constexpr int BADGE_PADDING = 6;
constexpr int DETAIL_GAP = 4;

struct SyncDetailRow {
  const char* label;
  const char* value;
  const char* subtitle = nullptr;
};

struct SyncProgressCard {
  const char* title;
  const char* device;
  const char* page;
  const char* chapter;
  float percentage;
  bool ahead;
  bool selected;
  const SyncDetailRow* details;
  int detailCount;
  int visibleDetails;
};

int syncCardHeight(const GfxRenderer& renderer) {
  return CARD_PADDING * 2 + renderer.getLineHeight(UI_10_FONT_ID) + CARD_GAP + renderer.getLineHeight(UI_12_FONT_ID) +
         CARD_GAP + BAR_HEIGHT + CARD_GAP + renderer.getLineHeight(UI_10_FONT_ID);
}

int detailRowHeight(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) + CARD_GAP; }

struct SyncComparisonLayout {
  Rect cards[2]{};
  Rect bookTitle{};
  int headingY = 0;
  int seriesY = -1;
  int authorY = -1;
  int summaryY = -1;
  int visibleDetails = 0;
};

SyncComparisonLayout comparisonLayout(const GfxRenderer& renderer, const Rect& area, const ThemeMetrics& metrics,
                                      const fui::ThemeTokens& theme, const bool touch, const char* bookTitle,
                                      const bool hasSeries, const bool hasAuthor) {
  SyncComparisonLayout layout;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int cardX = area.x + metrics.contentSidePadding;
  const int cardWidth = std::max(1, area.width - metrics.contentSidePadding * 2);
  layout.headingY = area.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int y = layout.headingY + lineH + metrics.verticalSpacing;
  if (bookTitle && bookTitle[0]) {
    const int lines = renderer.getTextWidth(UI_10_FONT_ID, bookTitle, EpdFontFamily::BOLD) > cardWidth ? 2 : 1;
    layout.bookTitle = Rect{cardX, y, cardWidth, lines * lineH};
    y += lines * lineH + DETAIL_GAP;
  }
  if (hasSeries) {
    layout.seriesY = y;
    y += smallH + DETAIL_GAP;
  }
  if (hasAuthor) {
    layout.authorY = y;
    y += smallH + DETAIL_GAP;
  }
  const int gap = std::max(8, metrics.verticalSpacing - 4);
  y += gap;

  // On touch readers the cards themselves are the actions; only button-driven
  // devices need room reserved for the two bottom selection rows.
  const int actionsHeight =
      touch ? 0 : metrics.listRowHeight * 2 + std::max(theme.listRowGap, theme.listTouchRowGap) + theme.spaceSm + 12;
  const int available = area.y + area.height - actionsHeight - gap - y;
  const bool portrait = area.width <= area.height;
  const int baseHeight = syncCardHeight(renderer);
  const int detailHeight = detailRowHeight(renderer);
  // A two-line duration label belongs to the local card. Fit both card
  // heights together so the added line cannot overlap the bottom actions.
  for (int count = 1; count <= 2; ++count) {
    const int remoteHeight = baseHeight + count * detailHeight;
    const int localHeight = remoteHeight + smallH;
    if ((portrait ? remoteHeight + gap + localHeight : localHeight) > available) break;
    layout.visibleDetails = count;
  }
  const int remoteHeight = baseHeight + layout.visibleDetails * detailHeight;
  const int localHeight = remoteHeight + (layout.visibleDetails > 0 ? smallH : 0);
  if (portrait) {
    layout.cards[0] = Rect{cardX, y, cardWidth, remoteHeight};
    layout.cards[1] = Rect{cardX, y + remoteHeight + gap, cardWidth, localHeight};
  } else {
    const int half = std::max(1, (cardWidth - gap) / 2);
    layout.cards[0] = Rect{cardX, y, half, remoteHeight};
    layout.cards[1] = Rect{cardX + cardWidth - half, y, half, localHeight};
  }
  const int afterCards = portrait ? layout.cards[1].y + localHeight : y + localHeight;
  if (afterCards + gap + smallH < area.y + area.height - actionsHeight) layout.summaryY = afterCards + gap;
  return layout;
}

void syncBaselineText(const GfxRenderer& renderer, int font, int x, int y, int rowFont, const char* text) {
  renderer.drawText(font, x, y + renderer.getFontAscenderSize(rowFont) - renderer.getFontAscenderSize(font), text);
}

void drawSyncProgressCard(const GfxRenderer& renderer, const Rect& card, const SyncProgressCard& info) {
  // The thicker focused outline identifies the card selected by hardware keys.
  renderer.drawRect(card.x, card.y, card.width, card.height, CARD_BORDER_WIDTH + (info.selected ? 2 : 0), true);
  const int x = card.x + CARD_PADDING;
  const int contentWidth = std::max(1, card.width - CARD_PADDING * 2);
  const int right = x + contentWidth;
  const int gap = CARD_GAP * 2;
  int y = card.y + CARD_PADDING;

  int titleRight = right;
  if (info.ahead) {
    const char* badge = tr(STR_SYNC_AHEAD);
    const int badgeWidth = renderer.getTextWidth(SMALL_FONT_ID, badge) + BADGE_PADDING * 2;
    const int badgeX = right - badgeWidth;
    const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.fillRect(badgeX, y, badgeWidth, titleHeight, true);
    renderer.drawText(SMALL_FONT_ID, badgeX + BADGE_PADDING,
                      y + (titleHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2, badge, false);
    titleRight = badgeX - gap;
  }

  const std::string title =
      renderer.truncatedText(UI_10_FONT_ID, info.title, std::max(1, titleRight - x), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (info.device && info.device[0]) {
    const int deviceX = x + renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD) + gap;
    if (titleRight > deviceX) {
      const std::string device = renderer.truncatedText(SMALL_FONT_ID, info.device, titleRight - deviceX);
      syncBaselineText(renderer, SMALL_FONT_ID, deviceX, y, UI_10_FONT_ID, device.c_str());
    }
  }
  y += renderer.getLineHeight(UI_10_FONT_ID) + CARD_GAP;

  char percent[16];
  snprintf(percent, sizeof(percent), "%.1f%%", info.percentage * 100.0f);
  renderer.drawText(UI_12_FONT_ID, x, y, percent, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + CARD_GAP;

  renderer.drawRect(x, y, contentWidth, BAR_HEIGHT, true);
  const float progress = std::min(1.0f, std::max(0.0f, info.percentage));
  const int filled = static_cast<int>((contentWidth - 4) * progress);
  if (filled > 0) renderer.fillRect(x + 2, y + 2, filled, BAR_HEIGHT - 4, true);
  y += BAR_HEIGHT + CARD_GAP;

  const std::string page = renderer.truncatedText(UI_10_FONT_ID, info.page, contentWidth);
  const int pageWidth = renderer.getTextWidth(UI_10_FONT_ID, page.c_str());
  const int chapterWidth = contentWidth - pageWidth - gap;
  if (chapterWidth > 0) {
    const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, info.chapter, chapterWidth);
    renderer.drawText(UI_10_FONT_ID, x, y, chapter.c_str());
  }
  renderer.drawText(UI_10_FONT_ID, right - pageWidth, y, page.c_str());
  y += renderer.getLineHeight(UI_10_FONT_ID) + CARD_GAP;
  for (int i = 0; i < std::min(info.detailCount, info.visibleDetails); ++i) {
    const SyncDetailRow& row = info.details[i];
    if (!row.value || !row.value[0]) continue;
    if (row.subtitle && row.subtitle[0]) {
      const std::string heading = renderer.truncatedText(SMALL_FONT_ID, row.label, contentWidth, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, x, y, heading.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(SMALL_FONT_ID);
    }
    const int valueWidth = renderer.getTextWidth(SMALL_FONT_ID, row.value);
    if (valueWidth >= contentWidth) {
      renderer.drawText(SMALL_FONT_ID, x, y, renderer.truncatedText(SMALL_FONT_ID, row.value, contentWidth).c_str());
    } else {
      const char* labelText = row.subtitle && row.subtitle[0] ? row.subtitle : row.label;
      const std::string label =
          renderer.truncatedText(SMALL_FONT_ID, labelText, std::max(1, contentWidth - valueWidth - CARD_GAP));
      renderer.drawText(SMALL_FONT_ID, x, y, label.c_str());
      renderer.drawText(SMALL_FONT_ID, right - valueWidth, y, row.value);
    }
    y += detailRowHeight(renderer);
  }
}

void formatDuration(const uint64_t seconds, char* out, const size_t capacity) {
  const unsigned long long hours = static_cast<unsigned long long>(seconds / 3600);
  const unsigned long long minutes = static_cast<unsigned long long>((seconds % 3600) / 60);
  if (hours > 0) {
    snprintf(out, capacity, "%lluh %llum", hours, minutes);
  } else if (minutes > 0) {
    snprintf(out, capacity, "%llum", minutes);
  } else {
    snprintf(out, capacity, "%llus", static_cast<unsigned long long>(seconds));
  }
}

bool formatUpdatedTime(const int64_t timestamp, char* out, const size_t capacity) {
  if (timestamp < 1577836800LL || timestamp > 4102444800LL) return false;
  const time_t value = static_cast<time_t>(timestamp);
  struct tm local{};
  if (!localtime_r(&value, &local)) return false;
  snprintf(out, capacity, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           local.tm_hour, local.tm_min);
  return true;
}

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod alternateMatchMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

}  // namespace

KOReaderSyncActivity::KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& epubPath, CrossPointPosition localPosition,
                                           SavedProgressPosition localKoPos, std::string localChapterName,
                                           const bool pluginEventsOnly, const uint32_t sessionSeconds,
                                           const uint32_t sessionForwardTurns)
    : Activity("KOReaderSync", renderer, mappedInput),
      UiAppHost(renderer),
      epubPath(epubPath),
      localChapterName(std::move(localChapterName)),
      localPosition(localPosition),
      sessionSeconds(sessionSeconds),
      sessionForwardTurns(sessionForwardTurns),
      remoteProgress{},
      remotePosition{},
      localProgress(std::move(localKoPos)),
      pluginEventsOnly(pluginEventsOnly) {}

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  std::optional<uint32_t> offset;
  if (remotePosition.hasVisibleTextOffset && remotePosition.spineIndex == spineIndex) {
    offset = remotePosition.visibleTextOffset;
  }
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0, offset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Keep the station fully awake for the short sync transaction. The web server
  // does the same because ESP32 modem sleep can introduce multi-second network
  // stalls that surface as HTTP timeouts. WiFi is torn down when this activity exits.
  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  if (pluginEventsOnly) {
    {
      RenderLock lock(*this);
      state = SYNCING;
      statusMessage = tr(STR_SYNC_PROGRESS);
    }
    requestUpdate(true);

    pluginevents::drain(&renderer);
    completeAlreadySynced();
    return;
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  // KOSync requests from CrossPoint do not include a client timestamp.
  performSync();
}

void KOReaderSyncActivity::performSync() {
  WifiPowerSaveGuard psGuard;
  const DocumentMatchMethod primaryMethod = KOREADER_STORE.getMatchMethod();
  documentHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  const std::string primaryHash = documentHash;

  LOG_DBG("KOSync", "Document hash (%s): %s", matchMethodName(primaryMethod), documentHash.c_str());

  if (KOREADER_STORE.getSyncClippings()) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_SYNCING_CLIPPINGS);
    }
    requestUpdateAndWait();
    bool clippingsSynced;
    {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
#endif
      clippingsSynced = clippingSync::run(epubPath, documentHash);
    }
    if (!clippingsSynced) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_CLIPPING_SYNC_FAILED);
      }
      requestUpdate(true);
      return;
    }
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress. In smart mode, retain the alternate document-id
  // record until both records can be mapped after the Epub is reloaded.
  auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  LOG_DBG("KOSync", "Primary remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
          matchMethodName(primaryMethod), result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(),
          localProgress.percentage, remoteProgress.percentage, remoteProgress.progress.c_str());

  KOReaderProgress alternateProgress;
  bool hasAlternateProgress = false;
  if (smartSyncEnabled()) {
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    const std::string altHash = calculateDocumentHashForMethod(epubPath, altMethod);
    if (!altHash.empty() && altHash != documentHash) {
      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(altHash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s local=%.6f remote=%.6f xpath=%s",
              matchMethodName(altMethod), altResult, KOReaderSyncClient::lastHttpCode, altHash.c_str(),
              localProgress.percentage, altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK) {
        alternateProgress = std::move(altProgress);
        hasAlternateProgress = true;
      }
    }
  }

  if (result == KOReaderSyncClient::NOT_FOUND && hasAlternateProgress) {
    remoteProgress = std::move(alternateProgress);
    hasAlternateProgress = false;
    result = KOReaderSyncClient::OK;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("KOSync", "Smart sync: no remote progress found for known document hashes; uploading local %.6f",
              localProgress.percentage);
      performUpload();
      return;
    }

    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);

    const auto mapRemoteProgress = [&](const KOReaderProgress& progress) {
      // The standard KOReader progress XPath is the authoritative content anchor.
      // The CrossPoint server's existing rich page hints remain a legacy fallback.
      const SavedProgressPosition koPos = {progress.progress, progress.percentage};
      CrossPointPosition mapped =
          ProgressMapper::toCrossPoint(epub, koPos, renderer, localPosition.spineIndex, localPosition.totalPages);
      if (!mapped.hasVisibleTextOffset && progress.position.has_value()) {
        // toCrossPoint above already tried koPos.xpath; if the rich position carries the same XPath,
        // tell fromRichPosition to skip re-resolving it and use its page hints directly.
        const bool sameXPath = progress.position->xpath == progress.progress;
        if (const auto richMapped = ProgressMapper::fromRichPosition(epub, *progress.position, renderer, sameXPath)) {
          mapped = *richMapped;
        }
      }
      return mapped;
    };

    remotePosition = mapRemoteProgress(remoteProgress);
    if (hasAlternateProgress) {
      const CrossPointPosition alternatePosition = mapRemoteProgress(alternateProgress);
      if (selectRemoteRecord(remotePosition, remoteProgress.percentage, alternatePosition,
                             alternateProgress.percentage) == RemoteRecordChoice::Alternate) {
        remoteProgress = std::move(alternateProgress);
        remotePosition = alternatePosition;
        LOG_DBG("KOSync", "Selected alternate remote record after mapped-position comparison");
      } else {
        LOG_DBG("KOSync", "Kept primary remote record after mapped-position comparison");
      }
    }
  }

  Epub::SyncMetadata bookMetadata;
  if (epub->loadSyncMetadata(bookMetadata)) {
    seriesLabel = library::formatSeriesLabel(bookMetadata.series, bookMetadata.seriesIndex);
  }

  const ProgressComparison comparison =
      compareProgress(localPosition, localProgress.percentage, remotePosition, remoteProgress.percentage);
  remoteAhead = comparison == ProgressComparison::RemoteAhead;
  localAhead = comparison == ProgressComparison::LocalAhead;
  if (smartSyncEnabled()) {
    LOG_DBG("KOSync", "Smart decision: doc=%s result=%d local=%.6f remote=%.6f remoteXpath=%s mapped=%d/%d",
            primaryHash.c_str(), static_cast<int>(comparison), localProgress.percentage, remoteProgress.percentage,
            remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
    switch (comparison) {
      case ProgressComparison::Synchronized:
        completeAlreadySynced();
        return;
      case ProgressComparison::LocalAhead:
        performUpload();
        return;
      case ProgressComparison::RemoteAhead:
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
        return;
      case ProgressComparison::Unknown:
        LOG_DBG("KOSync", "Smart sync comparison unknown; opening manual selection");
        break;
    }
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    selectedOption = comparison == ProgressComparison::LocalAhead ? 1 : 0;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Rich position for server profiles that explicitly support the CrossPoint extension.
  // The HTTP client enforces the same boundary before serializing the extension.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(localPosition.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(localPosition.pageNumber);
    pos.totalPages = static_cast<uint16_t>(localPosition.totalPages > 0 ? localPosition.totalPages : 1);
    if (localPosition.hasParagraphIndex) {
      pos.paragraphIndex = localPosition.paragraphIndex;
    }
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading from NO_REMOTE_PROGRESS the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (epub->loadSyncMetadata(syncMetadata)) {
          meta.isbn = std::move(syncMetadata.isbn);
          meta.asin = std::move(syncMetadata.asin);
          meta.series = std::move(syncMetadata.series);
          meta.seriesIndex = syncMetadata.seriesIndex;
        } else {
          LOG_DBG("KOSync", "Could not read extended EPUB metadata; sending core metadata only");
        }
      }
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    // Plugin sidecar fields ("<book>.meta.json", written at download time via
    // the catalog sidecar mechanism or /api/plugin-fs) ride along so a custom
    // sync server can route progress by a service book id.
    // A missing sidecar (the common case) leaves extraJson empty.
    Storage.readFileToString("KOSync", epubPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  KOReaderSyncClient::Error result;
  {
    // Restore power-save before esp_wifi_stop below; set_ps on a stopped radio fails.
    WifiPowerSaveGuard psGuard;
    result = KOReaderSyncClient::updateProgress(progress);
  }

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  resetUi();
  if (!pluginEventsOnly) {
    readingstats::Snapshot savedStats;
    hasLocalBookStats = readingstats::loadBook(epubPath.c_str(), savedStats);
    if (hasLocalBookStats) {
      localBookSeconds = savedStats.totalSeconds();
    }
  }
  app.on(ACTION_ROW, &KOReaderSyncActivity::onResultRow, this);
  app.on(ACTION_CARD, &KOReaderSyncActivity::onResultRow, this);
  app.setScreen(&KOReaderSyncActivity::resultScreen, this);

  // Check for credentials first (plugin-events-only mode needs none).
  if (!pluginEventsOnly && !KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::requestResultConfirmation(const int option) {
  if (state != SHOWING_RESULT || (option != 0 && option != 1)) return;
  selectedOption = option;
  char message[144];
  const double localPct = static_cast<double>(localProgress.percentage) * 100.0;
  const double remotePct = static_cast<double>(remoteProgress.percentage) * 100.0;
  if (option == 0) {
    snprintf(message, sizeof(message), "Move from %.1f%% local to %.1f%% remote?", localPct, remotePct);
  } else {
    snprintf(message, sizeof(message), "Replace server progress (%.1f%%) with local (%.1f%%)?", remotePct, localPct);
  }
  const char* buttons[] = {tr(STR_CANCEL), tr(STR_CONFIRM)};
  confirmationPopup.showMessage(option == 0 ? tr(STR_APPLY_REMOTE) : tr(STR_UPLOAD_LOCAL), message, buttons, 2, 0,
                                [this, option](const int choice) {
                                  if (choice == 1) pendingConfirmedOption = option;
                                });
  requestUpdate(true);
}

void KOReaderSyncActivity::chooseResultOption() {
  if (selectedOption == 0) {
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
  } else {
    performUpload();
  }
}

void KOReaderSyncActivity::startUpload() {
  if (documentHash.empty()) {
    documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                       ? KOReaderDocumentId::calculateFromFilename(epubPath)
                       : KOReaderDocumentId::calculate(epubPath);
  }
  performUpload();
}

void KOReaderSyncActivity::onResultRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KOReaderSyncActivity*>(user);
  // Clear tap feedback before opening the apply/upload confirmation.
  self->app.clearTapFlash();
  if (self->state == SHOWING_RESULT) {
    if (event.value < 0 || event.value > 1) return;
    self->selectedOption = event.value;
    self->requestResultConfirmation(self->selectedOption);
  } else if (self->state == NO_REMOTE_PROGRESS) {
    self->startUpload();
  }
}

void KOReaderSyncActivity::resultScreen(UiScreen& screen, void* user) {
  static_cast<KOReaderSyncActivity*>(user)->buildResultScreen(screen);
}

void KOReaderSyncActivity::buildResultScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The comparison cards use the theme's own side padding. Non-touch action
  // rows retain their themed FreeInkUI layout at the bottom.
  const int bottomHintHeight = state == SHOWING_RESULT && mappedInput.hasTouch() ? 0 : metrics.buttonHintsHeight;
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(bottomHintHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == SHOWING_RESULT) {
    if (mappedInput.hasTouch()) {
      // The comparison cards themselves are the two touch targets. Hardware
      // navigation still selects a card and Confirm opens the same dialog.
      const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
      const SyncComparisonLayout layout =
          comparisonLayout(renderer, area, metrics, screen.theme(), true, epub->getTitle().c_str(),
                           !seriesLabel.empty(), !epub->getAuthor().empty());
      for (int i = 0; i < 2; ++i) {
        const Rect& card = layout.cards[i];
        screen.frame().hit(fui::Rect{static_cast<int16_t>(card.x), static_cast<int16_t>(card.y),
                                     static_cast<int16_t>(card.width), static_cast<int16_t>(card.height)},
                           ACTION_CARD, static_cast<int16_t>(i), fui::InputTouch);
      }
      return;
    }

    // Non-touch devices retain the existing physical-button action rows.
    fui::ListItem actions[2];
    actions[0].label = tr(STR_APPLY_REMOTE);
    actions[0].icon = fui::bitmapFromIcon(icon_download_24);
    actions[0].actionValue = 0;
    actions[1].label = tr(STR_UPLOAD_LOCAL);
    actions[1].icon = fui::bitmapFromIcon(icon_upload_24);
    actions[1].actionValue = 1;
    fui::ListProps actionProps;
    actionProps.items = actions;
    actionProps.count = 2;
    actionProps.selectedIndex = static_cast<int16_t>(selectedOption);
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    const int16_t actionRowHeight = static_cast<int16_t>(metrics.listRowHeight);
    actionProps.rowHeight = actionRowHeight;
    const auto actionsBand =
        static_cast<int16_t>(actionRowHeight * 2 + screen.theme().listRowGap + screen.theme().spaceSm);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    auto centered = screen.theme().bodyText;
    centered.align = fui::TextAlign::Center;
    auto centeredBold = centered;
    centeredBold.bold = true;
    const int16_t lineH = screen.target().lineHeight(centered.font);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceSm), tr(STR_NO_REMOTE_MSG), centeredBold);
    screen.target().text(screen.takeTop(lineH, screen.theme().spaceMd), tr(STR_UPLOAD_PROMPT), centered);

    // Single themed action row anchored to the bottom, matching the lists used
    // everywhere else (inherits the theme's row radius + selection style).
    fui::ListItem action;
    action.label = tr(STR_UPLOAD_LOCAL);
    action.actionValue = 0;
    fui::ListProps actionProps;
    actionProps.items = &action;
    actionProps.count = 1;
    actionProps.selectedIndex = 0;
    actionProps.action = ACTION_ROW;
    actionProps.inputMask = fui::InputTouch;
    actionProps.scrollIndicator = false;
    // See the equivalent override above; keeps actionsBand in sync with the
    // row height actually used on non-touch hardware (X3/X4).
    int16_t actionRowHeight = screen.theme().rowHeight;
    if (!mappedInput.hasTouch()) {
      actionRowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
      actionProps.rowHeight = actionRowHeight;
    }
    const auto actionsBand = static_cast<int16_t>(actionRowHeight + screen.theme().spaceMd);
    screen.list(actionProps, actionsBand, fui::LayoutAnchor::Bottom);
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const bool showButtonHints = state != SHOWING_RESULT || !mappedInput.hasTouch();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, showButtonHints, false);

  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, showButtonHints, false),
                 tr(STR_KOREADER_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    renderUi();
    const SyncComparisonLayout layout =
        comparisonLayout(renderer, screen, metrics, app.theme(), mappedInput.hasTouch(), epub->getTitle().c_str(),
                         !seriesLabel.empty(), !epub->getAuthor().empty());
    renderer.drawCenteredText(UI_10_FONT_ID, layout.headingY, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);
    if (layout.bookTitle.height > 0) {
      UITheme::drawCenteredWrappedText(renderer, layout.bookTitle, UI_10_FONT_ID, epub->getTitle().c_str(), 2, true,
                                       EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::TOP);
    }
    const int bookWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
    if (layout.seriesY >= 0) {
      const std::string series = renderer.truncatedText(SMALL_FONT_ID, seriesLabel.c_str(), bookWidth);
      UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, layout.seriesY, series.c_str());
    }
    if (layout.authorY >= 0) {
      const std::string author = renderer.truncatedText(SMALL_FONT_ID, epub->getAuthor().c_str(), bookWidth);
      UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID, layout.authorY, author.c_str());
    }

    const int remoteToc = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        remoteToc >= 0 ? epub->getTocItem(remoteToc).title
                       : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        localChapterName.empty() ? (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(localPosition.spineIndex + 1))
                                 : localChapterName;

    char remotePage[48];
    snprintf(remotePage, sizeof(remotePage), tr(STR_SYNC_PAGE_FORMAT), remotePosition.pageNumber + 1);
    char localPage[48];
    snprintf(localPage, sizeof(localPage), tr(STR_SYNC_PAGE_TOTAL_FORMAT), localPosition.pageNumber + 1,
             localPosition.totalPages);
    char updated[32] = {};
    char serverDuration[24] = {};
    SyncDetailRow remoteDetails[2]{};
    int remoteDetailCount = 0;
    if (formatUpdatedTime(remoteProgress.timestamp, updated, sizeof(updated))) {
      remoteDetails[remoteDetailCount++] = {"Updated", updated};
    }
    if (remoteProgress.readingTimeSeconds.has_value()) {
      formatDuration(*remoteProgress.readingTimeSeconds, serverDuration, sizeof(serverDuration));
      remoteDetails[remoteDetailCount++] = {"Server reading time", serverDuration};
    }

    char sessionDuration[24] = {};
    char bookDuration[24] = {};
    char turnCount[16] = {};
    char combinedTime[64] = {};
    formatDuration(sessionSeconds, sessionDuration, sizeof(sessionDuration));
    snprintf(turnCount, sizeof(turnCount), "%lu", static_cast<unsigned long>(sessionForwardTurns));
    SyncDetailRow localDetails[2] = {
        {"Reading Duration", sessionDuration, "This Session"},
        {"Forward turns", turnCount},
    };
    if (hasLocalBookStats) {
      formatDuration(localBookSeconds, bookDuration, sizeof(bookDuration));
      snprintf(combinedTime, sizeof(combinedTime), "%s / %s", sessionDuration, bookDuration);
      localDetails[0].value = combinedTime;
      localDetails[0].subtitle = "This Session / Device Total";
    }
    constexpr int localDetailCount = 2;
    const bool showCardFocus = mappedInput.hasTouch() && cardFocusVisible;
    const SyncProgressCard cards[] = {
        {tr(STR_SYNC_REMOTE_TITLE), remoteProgress.device.c_str(), remotePage, remoteChapter.c_str(),
         remoteProgress.percentage, remoteAhead, showCardFocus && selectedOption == 0, remoteDetails, remoteDetailCount,
         layout.visibleDetails},
        {tr(STR_SYNC_LOCAL_TITLE), "This device", localPage, localChapter.c_str(), localProgress.percentage, localAhead,
         showCardFocus && selectedOption == 1, localDetails, localDetailCount, layout.visibleDetails},
    };
    drawSyncProgressCard(renderer, layout.cards[0], cards[0]);
    drawSyncProgressCard(renderer, layout.cards[1], cards[1]);

    if (layout.summaryY >= 0 && (remoteAhead || localAhead) && std::isfinite(remoteProgress.percentage) &&
        std::isfinite(localProgress.percentage)) {
      const float difference = std::fabs(remoteProgress.percentage - localProgress.percentage) * 100.0f;
      if (difference >= 0.05f) {
        char summary[64];
        snprintf(summary, sizeof(summary), "%.1f percentage points apart", static_cast<double>(difference));
        renderer.drawCenteredText(SMALL_FONT_ID, layout.summaryY, summary);
      }
    }

    if (confirmationPopup.processRender(renderer, mappedInput)) return;
    if (showButtonHints) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Prompt text + upload button render through the FreeInkApp.
    renderUi();

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (confirmationPopup.isActive()) {
      confirmationPopup.handleInput(mappedInput, [this] { requestUpdate(); });
      if (pendingConfirmedOption >= 0) {
        selectedOption = pendingConfirmedOption;
        pendingConfirmedOption = -1;
        chooseResultOption();
      }
      return;
    }
    // Touch goes through the FreeInkApp's card hit targets. Hardware keys
    // remain available to select and confirm either card.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow

    // Navigate the two options with physical buttons.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      if (mappedInput.hasTouch()) cardFocusVisible = true;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      requestResultConfirmation(selectedOption);
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    // Touch goes through the FreeInkApp: render() registered the upload button.
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;  // dispatched to onResultRow -> startUpload

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
