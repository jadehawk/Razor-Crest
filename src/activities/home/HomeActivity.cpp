#include "HomeActivity.h"

#include <Bitmap.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/home/HomeGridInteraction.h"
#include "components/home/HomeWidgetThemeAdapters.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/HomeMenuGeometry.h"
#include "util/RecentBookLimits.h"

namespace {
// Remembers only an SD path, not a decoded image or duplicated book list.
std::string selectedHomeCardPath;
}  // namespace

int HomeActivity::getMenuItemCount() const {
  if (!coverGridUi) return recentSlots() + static_cast<int>(homeButtons.size());
  int count = 4;  // Legacy cover-grid tabs (separate layout).
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (slotItem() != HomeMenuItem::NONE) {
    count++;
  }
  return count;
}

void HomeActivity::ensureHomeMenuSelectionVisible() {
  if (coverGridUi) return;
  const int base = UITheme::getInstance().getMetrics().homeContinueReadingInMenu ? 0 : recentSlots();
  homeMenuViewportTop = home_menu::scrollTopForSelection(homeMenuViewportTop, selectorIndex - base,
                                                         getMenuItemCount() - base, homeMenuViewportRows);
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Don't open EPUB files while enumerating recent candidates.
    // Completion and series metadata are checked on the selected card.
    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    // Completion is checked lazily when this candidate reaches the card.
    recentBooks.push_back(book);
    if (singleCardMode) continue;  // Lazy metadata and progress for one card.
    // RecentBooksStore can retain a legacy 1-bit thumb path. Resolve the
    // current native-sized 1-bit thumbnail contract on Home entry so
    // previous experimental grayscale covers are not reused.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (epub) {
        recentBooks.back().coverBmpPath = epub->getThumbBmpPath();
        // Parse the featured book once on Home entry, never on a repaint.
        // The small no-PSRAM boards should not open a ZIP for every arrow press.
        const auto* scene = UITheme::getInstance().activeExternalScene();
        if (recentBooks.size() == 1 && !coverGridUi && scene && scene->hasRegion(ThemeScene::Region::Home) &&
            FsHelpers::hasEpubExtension(book.path)) {
          Epub::SyncMetadata metadata;
          if (epub->loadSyncMetadata(metadata)) {
            recentBooks.back().series = std::move(metadata.series);
            recentBooks.back().seriesIndex = metadata.seriesIndex;
          }
        }
      }
    }
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (singleCardMode && &book != &recentBooks[cardPager.current()]) continue;
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          const bool cacheLoaded = epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / (singleCardMode ? 1 : recentBooks.size())));
          // Without a book cache (e.g. after "Delete Book Cache") there is no cached metadata,
          // so read the cover location straight from the EPUB.
          bool success =
              cacheLoaded ? epub.generateThumbBmp(thumbHeight) : epub.generateThumbBmpFromSource(thumbHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect,
                                  10 + progress * (90 / (singleCardMode ? 1 : recentBooks.size())));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::prepareCurrentCard(const int direction) {
  if (!singleCardMode) return;

  // Inspect only until a displayable book is found (normally one EPUB).
  // A run of completed candidates may require additional progress probes.
  size_t remaining = recentBooks.size();
  while (remaining-- > 0 && !recentBooks.empty()) {
    if (cardPager.index >= recentBooks.size()) cardPager.index = recentBooks.size() - 1;
    RecentBook& book = recentBooks[cardPager.index];
    if (cardPager.enabled) {
      if (book.carouselProgress == -2) book.carouselProgress = static_cast<int8_t>(loadBookProgress(book.path));
      if (book.carouselProgress >= 100) {
        recentBooks.erase(recentBooks.begin() + cardPager.index);
        if (direction < 0 && cardPager.index > 0) --cardPager.index;
        continue;
      }
    }

    if (!book.carouselMetadataLoaded) {
      book.carouselMetadataLoaded = true;
      if (FsHelpers::hasReflowableBookExtension(book.path)) {
        auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
        if (epub) {
          book.coverBmpPath = epub->getThumbBmpPath();
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub::SyncMetadata metadata;
            if (epub->loadSyncMetadata(metadata)) {
              book.series = std::move(metadata.series);
              book.seriesIndex = metadata.seriesIndex;
            }
          }
        }
      }
    }
    break;
  }
  cardPager.size = recentBooks.size();
  if (recentBooks.empty()) cardPager.index = 0;
}

void HomeActivity::cycleCard(int direction) {
  if (!singleCardMode || !cardPager.canMoveBounded(direction)) return;
  showCardAt(direction < 0 ? cardPager.current() - 1 : cardPager.current() + 1, direction);
}

void HomeActivity::showCardAt(size_t index, int direction) {
  if (!singleCardMode || index >= recentBooks.size()) return;
  cardPager.index = index;
  prepareCurrentCard(direction);
  selectedHomeCardPath = recentBooks.empty() ? std::string{} : recentBooks[cardPager.current()].path;
  // The previous cover snapshot contains the old title and cover. Never reuse
  // it for the newly selected book; load just one thumbnail on demand.
  freeCoverBuffer();
  coverRendered = false;
  recentsLoaded = false;
  requestUpdate();
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  const auto* scene = UITheme::getInstance().activeExternalScene();
  singleCardMode = !coverGridUi && scene && (scene->hasNativeCoverCard() || scene->supportsBookCarousel());
  const bool hasPsram = HalMemory::getPsramHeap().totalBytes > 0;
  const int carouselLimit = recentbooklimits::effective(SETTINGS.continueReadingBookLimit, hasPsram);
  const bool carouselEnabled = singleCardMode && scene->supportsBookCarousel() && SETTINGS.continueReadingCarousel;
  loadRecentBooks(coverGridUi      ? CoverGridHomeUi::MAX_BOOKS
                  : singleCardMode ? (carouselEnabled ? carouselLimit : 1)
                                   : metrics.homeRecentBooksCount);
  cardPager = {recentBooks.size(), 0, carouselEnabled};
  if (cardPager.enabled && !selectedHomeCardPath.empty()) {
    const auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                                 [](const RecentBook& b) { return b.path == selectedHomeCardPath; });
    if (it != recentBooks.end()) {
      cardPager.index = static_cast<size_t>(it - recentBooks.begin());
    }
  }
  if (singleCardMode) {
    prepareCurrentCard(1);
    selectedHomeCardPath = recentBooks.empty() ? std::string{} : recentBooks[cardPager.current()].path;
  }
  hasContinueReading = !recentBooks.empty();
  homeMenuHeroReclaim = 0;
  if (!coverGridUi) {
    homeMenuRegistry.load(hasPlugins, hasOpdsServers);
    homeButtons = homeMenuRegistry.visible();
    const int top = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
    const int rowHeight = GUI.getMenuRowHeight(renderer);
    const int hintsHeight = BoardConfig::hasTouch() ? 0 : metrics.buttonHintsHeight;
    const int footerHeight = scene ? scene->homeMenuFooterHeight(renderer) : 0;
    const int ordinaryRows =
        HomeMenuRegistry::capacityFor(renderer, top, rowHeight, metrics.menuSpacing, hintsHeight, footerHeight);
    const int totalRows =
        static_cast<int>(homeButtons.size()) + (metrics.homeContinueReadingInMenu && hasContinueReading ? 1 : 0);
    if (scene && BoardConfig::hasTouch() && footerHeight == 0 && totalRows > ordinaryRows) {
      // Reclaim only the few pixels needed for another complete row, while
      // retaining the 8px bottom safety margin and all footer-art reservations.
      const int available = home_menu::safeBottom(renderer.getScreenHeight(), hintsHeight, footerHeight) - top;
      homeMenuHeroReclaim = home_menu::reclaimForNextRow(available, rowHeight, metrics.menuSpacing, 16);
    }
    homeMenuViewportRows = HomeMenuRegistry::capacityFor(renderer, top - homeMenuHeroReclaim, rowHeight,
                                                         metrics.menuSpacing, hintsHeight, footerHeight);
    homeMenuViewportTop = 0;
  }
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    const HomeMenuItem slot = slotItem();
    coverGridUi->begin(recentBooks,
                       slot == HomeMenuItem::PLUGINS        ? CoverGridHomeUi::SlotTab::PLUGINS
                       : slot == HomeMenuItem::OPDS_BROWSER ? CoverGridHomeUi::SlotTab::OPDS
                                                            : CoverGridHomeUi::SlotTab::NONE,
                       hasContinueReading);
  }

  const auto base = recentSlots();
  selectorIndex = 0;
  if (initialMenuItem != HomeMenuItem::NONE) {
    if (coverGridUi)
      selectorIndex = base + menuItemToIndex(initialMenuItem, slotItem());
    else {
      const char* id = initialMenuItem == HomeMenuItem::FILE_BROWSER    ? "browse"
                       : initialMenuItem == HomeMenuItem::LIBRARY       ? "library"
                       : initialMenuItem == HomeMenuItem::OPDS_BROWSER  ? (hasPlugins ? "plugins" : "opds")
                       : initialMenuItem == HomeMenuItem::FILE_TRANSFER ? "transfer"
                                                                        : "settings";
      const auto match =
          std::find_if(homeButtons.begin(), homeButtons.end(), [id](const auto& button) { return button.id == id; });
      if (match != homeButtons.end()) selectorIndex = base + static_cast<int>(match - homeButtons.begin());
    }
  }

  ensureHomeMenuSelectionVisible();

  // Stage 4 cold-path only: verify the selected SD theme's declared grid
  // outside render()/loop(). Legacy Home remains the authoritative painter
  // and input handler until the grid's navigation path is integrated.
  homeGridCandidate.reset();
  if (!coverGridUi && scene && !scene->homeLayoutPath().empty()) {
    auto candidate = makeUniqueNoThrow<home_grid::session::State>();
    if (!candidate) {
      LOG_ERR("HOME", "OOM: Home layout descriptors; using legacy Home");
    } else {
      HalFile layoutFile;
      if (!Storage.openFileForRead("THEME", scene->homeLayoutPath(), layoutFile)) {
        LOG_ERR("HOME", "Home layout could not be opened; using legacy Home");
      } else {
        home_grid::json::Resources resources{};
        resources.owner = const_cast<ThemeScene*>(scene);
        resources.scene = [](void* owner, const char* name) -> uint16_t {
          return static_cast<const ThemeScene*>(owner)->homeSceneHandle(name);
        };
        resources.bitmap = [](void* owner, const char* role) -> uint16_t {
          const auto* active = static_cast<const ThemeScene*>(owner);
          return role && strcmp(role, "home_background") == 0 &&
                         !active->homeWallpaperPath().empty() ? 1 : 0;
        };
        // Optional providers return zero until Stage 5; their declared
        // fallback remains in the same bounds.
        const int totalRows = static_cast<int>(homeButtons.size()) +
            (metrics.homeContinueReadingInMenu && hasContinueReading ? 1 : 0);
        const int rowStep = std::max(1, GUI.getMenuRowHeight(renderer) + metrics.menuSpacing);
        resources.preferredMenuHeight = static_cast<uint16_t>(std::min(
            home_grid::kMaxScreenSize, std::max(0, totalRows) * rowStep));

        const Rect content = UITheme::getInstance().getContentArea(renderer);
        const int width = renderer.getScreenWidth();
        const int height = renderer.getScreenHeight();
        const int hintHeight = BoardConfig::hasTouch() ? 0 : metrics.buttonHintsHeight;
        const int footerHeight = scene->homeMenuFooterHeight(renderer);
        const home_grid::Screen safe{width, height, std::max(0, content.x),
                                    std::max(0, width - content.x - content.width),
                                    std::max(content.y, metrics.topPadding),
                                    std::max(height - content.y - content.height,
                                             hintHeight + footerHeight + 8)};
        const home_grid::activation::Policy policy{true, true, true, scene->supportsHomeBackground()};
        const auto status = home_grid::session::load(layoutFile, resources, safe, policy, *candidate);
        if (status == home_grid::json::Status::Ok) {
          home_grid::interaction::Regions areas;
          const bool mapped = home_grid::interaction::locate(
              candidate->parsed.widgets, candidate->active.rectangles, areas);
          const int nativeRowHeight = GUI.getMenuRowHeight(renderer);
          const int capacity = mapped ? home_grid::interaction::visibleMenuCapacity(
              areas.menu, nativeRowHeight, metrics.menuSpacing,
              scene->homeMenuFooterHeight(renderer) > 0) : 0;
          const bool rowHeightMatches = candidate->parsed.menuRowHeight == 0 ||
                                        candidate->parsed.menuRowHeight == nativeRowHeight;
          // The old Home carousel has one logical card and an indexed menu.
          // Defer layouts requiring multiple simultaneous book focus targets
          // until the native navigation model can represent them safely.
          const bool bookMatches = (recentSlots() == 0 || areas.bookWidget >= 0) &&
                                   (singleCardMode || recentSlots() <= 1);
          if (mapped && capacity > 0 && rowHeightMatches && bookMatches) {
            homeMenuViewportRows = capacity;
            homeMenuViewportTop = 0;
            ensureHomeMenuSelectionVisible();
            LOG_INF("HOME", "SD Home layout ready (%u rows, menu capacity %d)",
                    static_cast<unsigned>(candidate->parsed.layout.rowCount), capacity);
            homeGridCandidate = std::move(candidate);
          } else {
            LOG_ERR("HOME", "Home grid unsupported navigation/size; using legacy Home");
          }
        } else {
          LOG_ERR("HOME", "Home layout preflight failed (%d); using legacy Home", static_cast<int>(status));
        }
      }
    }
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();
  homeGridCandidate.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  coverBufferInverted = display.isInverted();
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  home_grid::interaction::Regions gridAreas;
  const bool gridActive = homeGridCandidate && homeGridCandidate->active.enabled &&
      home_grid::interaction::locate(homeGridCandidate->parsed.widgets,
                                     homeGridCandidate->active.rectangles, gridAreas);

  auto activateSelection = [this] {
    if (selectorIndex < recentSlots()) {
      onRecentBookSelected(recentBooks[singleCardMode ? cardPager.current() : selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - recentSlots();
    if (!coverGridUi) {
      if (menuIndex < 0 || menuIndex >= static_cast<int>(homeButtons.size())) return;
      const auto& shortcut = homeButtons[menuIndex];
      switch (shortcut.target) {
        case HomeMenuRegistry::Target::Browse:
          onFileBrowserOpen();
          break;
        case HomeMenuRegistry::Target::Library:
          onLibraryOpen();
          break;
        case HomeMenuRegistry::Target::Plugins:
          onPluginsOpen();
          break;
        case HomeMenuRegistry::Target::Opds:
          onOpdsBrowserOpen();
          break;
        case HomeMenuRegistry::Target::Transfer:
          onFileTransferOpen();
          break;
        case HomeMenuRegistry::Target::Dashboard:
          activityManager.goToPluginDashboard(shortcut.deviceManifest);
          break;
        case HomeMenuRegistry::Target::Settings:
          onSettingsOpen();
          break;
      }
      return;
    }
    switch (indexToMenuItem(menuIndex, slotItem())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::PLUGINS:
        onPluginsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  // List navigation wraps in both directions. Re-entering the carousel from
  // its final/first shortcut selects the matching first/last book.
  const auto moveHomeSelection = [this, menuCount](int direction) {
    if (menuCount <= 0) return;
    const int from = selectorIndex;
    selectorIndex =
        direction > 0 ? ButtonNavigator::nextIndex(from, menuCount) : ButtonNavigator::previousIndex(from, menuCount);
    if (singleCardMode && cardPager.canCycle() && selectorIndex == 0) {
      if (direction > 0 && from == menuCount - 1) {
        showCardAt(0, 1);
      } else if (direction < 0 && from == recentSlots()) {
        showCardAt(recentBooks.size() - 1, -1);
      }
    }
    ensureHomeMenuSelectionVisible();
    requestUpdate();
  };

  // When a carousel card has focus, side buttons switch books. Front
  // Left/Right still move between Home selections; one-book behavior stays intact.
  if (singleCardMode && selectorIndex == 0 && cardPager.canCycle()) {
    if (mappedInput.isPressed(MappedInputManager::Button::Up)) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        if (cardPager.canMoveBounded(-1))
          cycleCard(-1);
        else if (!homeButtons.empty())
          moveHomeSelection(-1);  // First book wraps to the final Home shortcut.
      }
      return;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Down)) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (cardPager.canMoveBounded(1)) {
          cycleCard(1);
        } else if (!homeButtons.empty()) {
          moveHomeSelection(1);  // Last book moves into the first Home shortcut.
        }
      }
      return;
    }
  }

  // Moving back from the first menu entry selects the last carousel book.
  if (singleCardMode && cardPager.canCycle() && selectorIndex == recentSlots() &&
      mappedInput.isPressed(MappedInputManager::Button::Up)) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) moveHomeSelection(-1);
    return;
  }

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  if (!coverGridUi) {
    buttonNavigator.onNext([&moveHomeSelection] { moveHomeSelection(1); });
    buttonNavigator.onPrevious([&moveHomeSelection] { moveHomeSelection(-1); });
  }

  int swipeStartX = 0;
  int swipeStartY = 0;
  const auto swipe = mappedInput.wasSwipe(&swipeStartX, &swipeStartY);
  const bool swipeInBook = gridActive
      ? (gridAreas.bookWidget >= 0 && home_grid::interaction::inside(gridAreas.book, swipeStartX, swipeStartY))
      : (swipeStartY >= metrics.homeTopPadding &&
         swipeStartY < metrics.homeTopPadding + metrics.homeCoverTileHeight - homeMenuHeroReclaim &&
         swipeStartX >= 44 && swipeStartX < renderer.getScreenWidth() - 24);
  if (singleCardMode && cardPager.canCycle() && mappedInput.hasTouch() &&
      swipeInBook && !mappedInput.wasBackGesture()) {
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
      selectorIndex = 0;
      cycleCard(swipe == MappedInputManager::SwipeDir::Left ? 1 : -1);
      return;
    }
  }
  if (!coverGridUi && mappedInput.hasTouch() &&
      (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down)) {
    const auto& content = UITheme::getInstance().getContentArea(renderer);
    const int top = gridActive ? gridAreas.menu.y :
        metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset - homeMenuHeroReclaim;
    const auto* scene = UITheme::getInstance().activeExternalScene();
    const int footer = scene ? scene->homeMenuFooterHeight(renderer) : 0;
    const int bottom = gridActive ? gridAreas.menu.y + gridAreas.menu.height :
        home_menu::safeBottom(renderer.getScreenHeight(), 0, footer);
    const int left = gridActive ? gridAreas.menu.x : content.x;
    const int right = gridActive ? gridAreas.menu.x + gridAreas.menu.width : content.x + content.width;
    const int menuBase = metrics.homeContinueReadingInMenu ? 0 : recentSlots();
    const int total = menuCount - menuBase;
    if (swipeStartX >= left && swipeStartX < right && swipeStartY >= top &&
        swipeStartY < bottom && total > homeMenuViewportRows) {
      const int step = std::max(1, homeMenuViewportRows - 1);
      const int nextTop = home_menu::scrollTopBy(
          homeMenuViewportTop, swipe == MappedInputManager::SwipeDir::Up ? step : -step, total, homeMenuViewportRows);
      if (nextTop != homeMenuViewportTop) {
        homeMenuViewportTop = nextTop;
        const int selectedRow = selectorIndex - menuBase;
        const int shown = home_menu::visibleRows(total, nextTop, homeMenuViewportRows);
        if (selectedRow < nextTop || selectedRow >= nextTop + shown)
          selectorIndex = menuBase + (swipe == MappedInputManager::SwipeDir::Up ? nextTop + shown - 1 : nextTop);
        requestUpdate();
      }
      return;
    }
  }
  if (swipe == MappedInputManager::SwipeDir::Up) {
    moveHomeSelection(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    moveHomeSelection(-1);
    return;
  }

  // Back/Resume opens the currently displayed carousel book, or the most
  // recent book on other layouts. Missing SD paths were already pruned.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onRecentBookSelected(recentBooks[singleCardMode ? cardPager.current() : 0].path);
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = singleCardMode ? 1 : std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = gridActive ? (gridAreas.bookWidget >= 0 ? std::min(1, recentSlots()) : 0)
                                     : std::min(recentSlots(), coverColumnCount);
  const int coverColumnWidth = gridActive ? gridAreas.book.width :
      (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  const int coverLeft = gridActive ? gridAreas.book.x : content.x + metrics.contentSidePadding;
  const int coverTop = gridActive ? gridAreas.book.y : metrics.homeTopPadding;
  const int coverBottom = gridActive ? gridAreas.book.y + gridAreas.book.height :
      metrics.homeTopPadding + metrics.homeCoverTileHeight - homeMenuHeroReclaim;
  int touchedBook = -1;
  const auto coverTouch = recentCount > 0 ?
      mappedInput.colTouch(touchedBook, coverLeft, coverColumnWidth, recentCount,
                           coverTop, coverBottom, coverColumnWidth)
      : MappedInputManager::RowTouch::None;
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const int menuTop = gridActive ? gridAreas.menu.y :
      metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset - homeMenuHeroReclaim;
  const int menuLeft = gridActive ? gridAreas.menu.x : content.x;
  const int menuRight = gridActive ? gridAreas.menu.x + gridAreas.menu.width : content.x + content.width;
  const int menuBase = metrics.homeContinueReadingInMenu ? 0 : recentSlots();
  const int renderedMenuCount = home_menu::visibleRows(menuCount - menuBase, homeMenuViewportTop, homeMenuViewportRows);
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              menuLeft, menuRight, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex = menuBase + homeMenuViewportTop + menuRow;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

bool HomeActivity::renderGridHome() {
  if (!homeGridCandidate || !homeGridCandidate->active.enabled) return false;
  const auto& state = *homeGridCandidate;
  home_grid::interaction::Regions areas;
  if (!home_grid::interaction::locate(state.parsed.widgets, state.active.rectangles, areas))
    return false;
  auto& theme = UITheme::getInstance();
  const ThemeScene* scene = theme.activeExternalScene();
  if (!scene) return false;

  // The Book snapshot belongs to the resolved card rectangle, never the
  // full Home canvas. A previous theme/orientation must not reuse old pixels.
  if (areas.bookWidget >= 0) {
    if (coverRectX != areas.book.x || coverRectY != areas.book.y ||
        coverRectW != areas.book.width || coverRectH != areas.book.height) {
      freeCoverBuffer();
      coverRendered = false;
    }
    coverRectX = areas.book.x;
    coverRectY = areas.book.y;
    coverRectW = areas.book.width;
    coverRectH = areas.book.height;
  }
  // Region snapshots copy whole physical bytes: split-row or rotated
  // Book cards could overwrite pixels belonging to neighboring widgets.
  const bool snapshotAllowed = areas.bookCacheSafe &&
      renderer.getOrientation() == GfxRenderer::Orientation::Portrait;
  if (coverBufferStored && (!snapshotAllowed || coverBufferInverted != display.isInverted())) {
    freeCoverBuffer();
    coverRendered = false;
  }

  const auto& metrics = theme.getMetrics();
  const bool menuHasResume = metrics.homeContinueReadingInMenu && !recentBooks.empty();
  const int menuBase = menuHasResume ? 0 : recentSlots();
  const int totalRows = static_cast<int>(homeButtons.size()) + (menuHasResume ? 1 : 0);
  const int top = home_menu::scrollTopBy(homeMenuViewportTop, 0, totalRows, homeMenuViewportRows);
  const int shown = home_menu::visibleRows(totalRows, top, homeMenuViewportRows);
  const int selection = selectorIndex - menuBase - top;

  std::function<std::string(int)> menuLabel = [this, menuHasResume, top](int row) {
    const int index = top + row;
    return menuHasResume && index == 0 ? std::string(tr(STR_CONTINUE_READING))
                                      : homeButtons[index - (menuHasResume ? 1 : 0)].label;
  };
  std::function<UIIcon(int)> menuIcon = [this, menuHasResume, top](int row) {
    const int index = top + row;
    return menuHasResume && index == 0 ? Book : homeButtons[index - (menuHasResume ? 1 : 0)].icon;
  };
  std::function<bool()> storeSnapshot = [this, snapshotAllowed] {
    return snapshotAllowed && storeCoverBuffer();
  };
  std::function<bool()> restoreSnapshot = [this, snapshotAllowed] {
    return snapshotAllowed && restoreCoverBuffer();
  };
  bool bufferRestored = false;

  home_widgets::firmware::View view{theme, renderer};
  view.recentBooks = &recentBooks;
  view.wallpaperPath = &scene->homeWallpaperPath();
  view.headerTitle = menuHasResume && !recentBooks.empty()
                         ? recentBooks[singleCardMode ? cardPager.current() : 0].title.c_str() : nullptr;
  view.selectorIndex = selectorIndex;
  view.currentBook = cardPager.current();
  view.useExternalBookCard = areas.bookWidget >= 0 && scene->hasNativeCoverCard() &&
                             scene->hasRegion(ThemeScene::Region::Home);
  view.externalBookScene = areas.bookScene;
  view.allowCachedBook = snapshotAllowed;
  view.coverRendered = &coverRendered;
  view.coverBufferStored = &coverBufferStored;
  view.bufferRestored = &bufferRestored;
  view.storeCoverBuffer = &storeSnapshot;
  view.restoreCoverBuffer = &restoreSnapshot;
  view.visibleMenuRows = shown;
  view.selectedMenuRow = selection >= 0 && selection < shown ? selection : -1;
  view.menuLabel = &menuLabel;
  view.menuIcon = &menuIcon;
  // Stage 5 supplies validated statistics sources; absent providers use the
  // existing bounded "NO DATA" placeholder rather than guessing values.
  view.readouts.clock = [](void*, char* value, size_t capacity) -> bool {
    return SETTINGS.clockHasBeenSynced && halClock.isAvailable() &&
           halClock.formatTime(value, capacity, SETTINGS.clockFormat == 1);
  };

  auto surface = home_widgets::firmware::makeSurface(view);
  const auto report = home_widgets::paint(state.parsed.widgets, state.parsed.layout,
                                          state.active.rectangles, home_widgets::native::hooks(surface));
  if (report.status != home_widgets::PaintStatus::Ok || report.failedWidgets ||
      report.failedUnderlays || report.backgroundFallback) {
    LOG_ERR("HOME", "Grid paint failed (widgets=%u, scenes=%u, backdrop=%d); using legacy Home",
            static_cast<unsigned>(report.failedWidgets), static_cast<unsigned>(report.failedUnderlays),
            static_cast<int>(report.backgroundFallback));
    return false;
  }

  // Reuse the theme's existing footer-art renderer outside the menu widget's
  // clip, inside the firmware-reserved footer band and above physical hints.
  if (scene->homeMenuFooterHeight(renderer) > 0)
    scene->drawMenu(renderer, home_widgets::firmware::toThemeRect(areas.menu),
                    0, 0, menuLabel, menuIcon);

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME),
                                             tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                 : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
  return true;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);

  renderer.clearScreen();
  if (coverGridUi) {
    const auto drawGrid = [&] {
      renderer.clearScreen();
      coverGridUi->setSelection(selectorIndex);
      UITheme::getInstance().drawCoverGridHome(*coverGridUi);
      // Front Left/Right walk the tabs, so their hints read Left/Right; the
      // side page buttons (unhinted) walk the covers.
      const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                                tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    };
    drawGrid();
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Resolve the covers before the
    // panel is touched and redraw into the framebuffer, so Home paints once
    // instead of three e-ink refreshes (placeholder, placeholder, covers).
    if (coverGridUi->takeThumbHeightChanged()) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      drawGrid();
      if (coverGridUi->takeThumbHeightChanged()) {
        coverGridUi->refreshCoverPaths();
        recentsLoaded = false;
        requestUpdate();
      }
    }
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    firstRenderDone = true;
    return;
  }
  if (homeGridCandidate && homeGridCandidate->active.enabled) {
    if (renderGridHome()) return;
    // Painting may have partially modified the framebuffer or the cached
    // book tile. Never retain a broken grid nor reuse invalid snapshot bytes.
    homeGridCandidate.reset();
    freeCoverBuffer();
    coverRendered = false;
    const auto* fallbackScene = UITheme::getInstance().activeExternalScene();
    const int hints = BoardConfig::hasTouch() ? 0 : metrics.buttonHintsHeight;
    const int footer = fallbackScene ? fallbackScene->homeMenuFooterHeight(renderer) : 0;
    const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight +
                        metrics.homeMenuTopOffset - homeMenuHeroReclaim;
    homeMenuViewportRows = HomeMenuRegistry::capacityFor(
        renderer, menuTop, GUI.getMenuRowHeight(renderer), metrics.menuSpacing, hints, footer);
    ensureHomeMenuSelectionVisible();
    renderer.clearScreen();
    LOG_ERR("HOME", "Grid render disabled; restored built-in Home fallback");
  }

  // A cover snapshot from the other output polarity holds the other byte form (night mode keeps images pre-inverted so
  // they stay positive): pasting it back after a theme switch showed the cover as a negative. Redraw it instead.
  if (coverBufferStored && coverBufferInverted != display.isInverted()) {
    freeCoverBuffer();
    coverRendered = false;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty()
                     ? recentBooks[singleCardMode ? cardPager.current() : 0].title.c_str()
                     : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight - homeMenuHeroReclaim;

  const Rect tile{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight - homeMenuHeroReclaim};
  const auto& uiTheme = UITheme::getInstance();
  const ThemeScene* external = uiTheme.activeExternalScene();
  const bool externalHome = external && external->hasRegion(ThemeScene::Region::Home);
  bool themedHome = false;
  if (externalHome) {
    bool backgroundReady = bufferRestored;
    if (!backgroundReady) {
      backgroundReady = uiTheme.renderExternalScene(ThemeScene::Region::Home, renderer, tile, nullptr, &recentBooks,
                                                    selectorIndex, ThemeScene::Phase::Background, cardPager.current());
      if (backgroundReady) {
        coverBufferStored = storeCoverBuffer();
        coverRendered = coverBufferStored;
      }
    }
    if (backgroundReady)
      themedHome = uiTheme.renderExternalScene(ThemeScene::Region::Home, renderer, tile, nullptr, &recentBooks,
                                               selectorIndex, ThemeScene::Phase::Foreground, cardPager.current());
  }
  if (!themedHome) {
    if (externalHome) {
      coverRendered = false;
      coverBufferStored = false;
      bufferRestored = false;
      renderer.fillRect(tile.x, tile.y, tile.width, tile.height, false);
    }

    GUI.drawRecentBookCover(renderer, tile, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                            bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));
  }

  if (singleCardMode && cardPager.canCycle() && selectorIndex == 0 && external && external->hasNativeCoverCard()) {
    // Native cover-card rails; outlined SD cards retain their original styling.
    // Front buttons continue to navigate Home independently.
    const int cardW = std::max(1, tile.width - 28);
    const int cardH = std::min(tile.height, 166);
    const int x = tile.x + 14;
    const int y = tile.y + tile.height - cardH - 7;
    const int rail = external ? external->bookCarouselRailWidth() : 8;
    renderer.fillRect(x, y + 3, rail, cardH - 6, true);
    renderer.fillRect(x + cardW - rail, y + 3, rail, cardH - 6, true);
  }

  // Render only the visible window, so every enabled shortcut is reachable
  // without drawing rows under the footer or allocating a second menu list.
  const bool menuHasResume = metrics.homeContinueReadingInMenu && !recentBooks.empty();
  const int menuBase = menuHasResume ? 0 : recentSlots();
  const int totalRows = static_cast<int>(homeButtons.size()) + (menuHasResume ? 1 : 0);
  const int viewportTop = home_menu::scrollTopBy(homeMenuViewportTop, 0, totalRows, homeMenuViewportRows);
  const int shownRows = home_menu::visibleRows(totalRows, viewportTop, homeMenuViewportRows);
  const int selectedRow = selectorIndex - menuBase - viewportTop;
  const int menuTop =
      metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset - homeMenuHeroReclaim;
  const int hintsHeight = BoardConfig::hasTouch() ? 0 : metrics.buttonHintsHeight;
  const int footerHeight = external ? external->homeMenuFooterHeight(renderer) : 0;
  const int menuHeight = std::max(1, home_menu::safeBottom(pageHeight, hintsHeight, footerHeight) - menuTop);
  const Rect menuRect{content.x, menuTop, content.width, menuHeight};

  GUI.drawButtonMenu(
      renderer, menuRect, shownRows, selectedRow >= 0 && selectedRow < shownRows ? selectedRow : -1,
      [this, menuHasResume, viewportTop](int row) {
        const int index = viewportTop + row;
        return menuHasResume && index == 0 ? std::string(tr(STR_CONTINUE_READING))
                                           : homeButtons[index - (menuHasResume ? 1 : 0)].label;
      },
      [this, menuHasResume, viewportTop](int row) {
        const int index = viewportTop + row;
        return menuHasResume && index == 0 ? Book : homeButtons[index - (menuHasResume ? 1 : 0)].icon;
      });

  if (totalRows > homeMenuViewportRows) {
    const int barWidth = std::max(2, metrics.scrollBarWidth);
    const int barX = menuRect.x + menuRect.width - metrics.scrollBarRightOffset - barWidth;
    const int thumbHeight = std::min(menuHeight, std::max(12, menuHeight * shownRows / totalRows));
    const int travel = menuHeight - thumbHeight;
    const int maxTop = totalRows - homeMenuViewportRows;
    renderer.fillRect(barX, menuTop + (viewportTop * travel) / maxTop, barWidth, thumbHeight, true);
  }

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  firstRenderDone = true;
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    // A themed Home still needs real cover thumbnails. Previously an
    // external scene short-circuited this entire generation path, leaving
    // its native cover card stuck on the placeholder forever.
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(); }
