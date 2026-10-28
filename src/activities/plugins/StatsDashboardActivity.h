#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "DashboardCardAssets.h"
#include "activities/Activity.h"
#include "util/ReadingStatsBookIndex.h"
#include "util/ReadingStatsData.h"
#include "util/ReadingStatsLegacyPreview.h"
#include "util/SingleCardPager.h"

struct Rect;  // Defined by the active UI theme; only referenced in this header.

// A generic local, declarative plugin dashboard. No plugin C++/JavaScript
// executes on the device. This host understands only bounded widget primitives.
class StatsDashboardActivity final : public Activity {
 public:
  StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest);
  StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest,
                         const readingstats::BookEntry& selected);

  static bool supportsManifest(const std::string& path);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Paint only dashboard page one into the current framebuffer, without
  // activity navigation/footer or an intermediate screen refresh.
  bool renderSleepOverview();

 private:
  struct Widget {
    std::string type;
    std::string label;
    std::string field;
    bool currentBook = false;
    bool showValue = true;
    int8_t iconIndex = -1;
    dashboardcards::Border border = dashboardcards::Border::Square;
    uint8_t cornerRadius = 8;
    uint8_t iconSize = 0;  // 0 selects the largest safe size for the card.
  };
  struct Page {
    std::string title;
    std::array<Widget, 6> widgets;
    uint8_t count = 0;
  };

  struct IconAsset {
    std::string name;
    dashboardcards::Icon bitmap;
  };
  std::array<IconAsset, dashboardcards::kMaxIcons> iconAssets{};
  uint8_t iconCount = 0;
  std::string manifestPath;
  std::string title = "Dashboard";
  std::array<Page, 6> pages;
  uint8_t pageCount = 0;
  uint8_t activePage = 0;
  bool genericCardDashboard = false;
  uint8_t carouselPage = 0xFF;
  struct CarouselCard {
    std::string title, subtitle, value;
    int progress = -1;
  };
  std::array<CarouselCard, 8> carouselCards{};
  singlecard::Pager carouselPager{};
  readingstats::Snapshot stats;
  readingstats::Snapshot bookStats;
  readingstats::BookEntry bookInfo{};
  struct BookRow {
    readingstats::BookEntry entry{};
    char displayAuthor[48] = {};
    uint64_t seconds = 0;
    int progress = -1;
  };
  static constexpr uint8_t kMaxVisibleBooks = 5;
  static constexpr int kBookRowStep = 82;
  std::array<BookRow, kMaxVisibleBooks> visibleBooks{};
  uint8_t visibleCount = 0;
  uint8_t nextCoverIndex = 0;
  uint32_t visibleStart = 0;
  uint32_t bookCursor = 0;
  int currentProgress = -1;
  std::string currentAuthor;
  bool selectedBook = false;
  bool hasBookInfo = false;
  std::string seriesName;
  std::string seriesIndex;
  uint32_t recordedBookCount = 0;
  bool statsAvailable = false;
  bool bookStatsAvailable = false;
  readingstats::legacy::Preview crossInkPreview{};
  readingstats::legacy::Preview vCodexPreview{};
  bool legacyPreviewLoaded = false;

  void drawDashboardPage(bool interactive);
  bool loadManifest();
  int8_t loadIcon(const std::string& name);
  void drawCardBorder(const Widget& widget, int x, int y, int width, int height) const;
  void drawCardIcon(const Widget& widget, int x, int y, int size) const;
  void drawBookCover(uint64_t hash, Rect rect) const;
  void drawHero(const Widget& widget, int x, int y, int width, int height) const;
  void drawPluginCard(const Widget& widget, int x, int y, int width, int height) const;
  bool carouselPageActive() const { return activePage == carouselPage && carouselPager.size > 0; }
  void cyclePluginCard(int direction);
  void refresh();
  void resolveBook();
  void loadSeries();
  int drawBookIdentity(int y, int width) const;
  void openBooks();
  void promptRemoveBook();
  void openImport();
  bool booksPage() const;
  int dashboardContentTop() const;
  int listCapacity() const;
  void loadVisibleBooks();
  void moveBookCursor(int delta);
  void drawBookList(int top, int bottom) const;
  bool importPage() const;
  void refreshLegacyPreviews();
  void drawLegacyImport(int x, int y, int width, int height) const;
  void advance(int delta);
  void drawMetric(const Widget& widget, int x, int y, int width, int height) const;
  void drawWeek(const Widget& widget, int x, int y, int width, int height) const;
  void drawHeatmap(const Widget& widget, int x, int y, int width, int height) const;
  void drawWidget(const Widget& widget, int x, int y, int width, int height) const;
  const readingstats::Snapshot& statsFor(const Widget& widget) const;
  bool statsPresentFor(const Widget& widget) const;
  std::string metricValue(const Widget& widget) const;
};
