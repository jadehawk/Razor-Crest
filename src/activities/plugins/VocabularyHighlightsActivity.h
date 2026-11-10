#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "util/VocabularyHighlightsState.h"

// Firmware-rendered, offline browser for the shared v1 vocabulary JSONL and
// CrossPoint clipping export. Its SD-plugin marker does not execute JavaScript.
class VocabularyHighlightsActivity final : public UiListActivity {
 public:
  VocabularyHighlightsActivity(GfxRenderer& renderer, MappedInputManager& input);
  static bool supportsManifest(const std::string& deviceManifest);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 private:
  enum class Screen : uint8_t { Sections, Books, Entries };
  enum class Source : uint8_t { Vocabulary, Highlights };
  enum class Sort : uint8_t { Alphabetical, Recent, ReadingOrder };

  struct Book {
    uint64_t key = 0;
    std::string title;
    std::string author;
    uint32_t count = 0;
    uint32_t latest = 0;
  };
  struct Entry {
    uint64_t offset = 0;
    uint32_t length = 0;
    uint32_t timestamp = 0;
    uint32_t startOffset = UINT32_MAX;
    uint16_t spine = UINT16_MAX;
    uint16_t page = 0;
    std::string label;
    std::string chapter;
    std::string id;
    VocabularyHighlightsState::Status status = VocabularyHighlightsState::Status::New;
    bool hidden = false;
  };

#if FREEINK_DEVICE_X3
  static constexpr size_t kMaxBooks = 64;
  static constexpr size_t kMaxEntries = 192;
#else
  static constexpr size_t kMaxBooks = 128;
  static constexpr size_t kMaxEntries = 512;
#endif
  static constexpr size_t kVisibleRows = 8;

  Screen screen = Screen::Sections;
  Source source = Source::Vocabulary;
  Sort bookSort = Sort::Alphabetical;
  Sort entrySort = Sort::Alphabetical;
  std::vector<Book> books;
  std::vector<Entry> entries;
  uint64_t selectedBook = 0;
  bool booksOverflow = false;
  bool entriesOverflow = false;
  bool showDeleted = false;
  bool pendingSortDialog = false;
  bool pendingErrorDialog = false;
  std::string pendingDeleteId;
  VocabularyHighlightsState learningState;
  std::array<std::string, kVisibleRows> labels{};
  std::array<std::string, kVisibleRows> subtitles{};
  std::array<freeink::ui::ListItem, kVisibleRows> rowItems{};
  OptionPopup sortPopup;

  void scanBooks();
  void scanEntries();
  void sortBooks();
  void sortEntries();
  void openDetail(int index);
  void showSortOptions();
  void showEntryOptions(int index);
  void showStateError();
  void showDeleteConfirmation(const std::string& id);
  void refreshWindow(int start);

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;
};
