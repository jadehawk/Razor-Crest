#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ReadingStatsBookIndex.h"
#include "util/ReadingStatsData.h"
#include "util/ReadingStatsLegacyPreview.h"

// A generic local, declarative plugin dashboard. No plugin C++/JavaScript
// executes on the device. This host understands only bounded widget primitives.
class StatsDashboardActivity final : public Activity {
 public:
  StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest);
  StatsDashboardActivity(GfxRenderer& renderer, MappedInputManager& input, std::string manifest,
                         readingstats::BookEntry selected);

  static bool supportsManifest(const std::string& path);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Widget {
    std::string type;
    std::string label;
    std::string field;
    bool currentBook = false;
  };
  struct Page {
    std::string title;
    std::array<Widget, 6> widgets;
    uint8_t count = 0;
  };

  std::string manifestPath;
  std::string title = "Dashboard";
  std::array<Page, 6> pages;
  uint8_t pageCount = 0;
  uint8_t activePage = 0;
  readingstats::Snapshot stats;
  readingstats::Snapshot bookStats;
  readingstats::BookEntry bookInfo{};
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

  bool loadManifest();
  void refresh();
  void resolveBook();
  void loadSeries();
  int drawBookIdentity(int y, int width) const;
  void openBooks();
  void openImport();
  bool booksPage() const;
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
