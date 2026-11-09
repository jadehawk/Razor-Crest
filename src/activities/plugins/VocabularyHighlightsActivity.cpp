#include "VocabularyHighlightsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <utility>

#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ClippingFingerprint.h"

namespace fui = freeink::ui;

namespace {
constexpr char kVocabularyPath[] = "/Vocabulary.txt";
constexpr char kClippingsPath[] = "/My Clippings.txt";
constexpr size_t kManifestLimit = 4096;
constexpr size_t kMaxRecordBytes = 6144;
constexpr size_t kChunkBytes = 256;

struct RecordSummary {
  std::string title;
  std::string author;
  std::string id;
  std::string fingerprint;
  std::string label;
  std::string chapter;
  uint32_t timestamp = 0;
  uint32_t startOffset = UINT32_MAX;
  uint16_t spine = UINT16_MAX;
  uint16_t page = 0;
};

uint64_t bookKey(const std::string& title, const std::string& author) {
  // Hash the complete values, not their shortened display labels.
  uint64_t h = UINT64_C(14695981039346656037);
  for (const unsigned char c : title) h = (h ^ c) * UINT64_C(1099511628211);
  h = (h ^ 0) * UINT64_C(1099511628211);
  for (const unsigned char c : author) h = (h ^ c) * UINT64_C(1099511628211);
  return h;
}

std::string shortLabel(const std::string& text, size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  // Never split a UTF-8 continuation byte when shortening a label.
  while (maxBytes && (static_cast<unsigned char>(text[maxBytes]) & 0xc0) == 0x80) --maxBytes;
  return text.substr(0, maxBytes);
}

bool beforeLabel(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) < std::tolower(y);
  });
}

// Natural ordering for chapter labels such as Chapter 9 / Chapter 10.
bool beforeChapter(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const unsigned char left = static_cast<unsigned char>(a[i]);
    const unsigned char right = static_cast<unsigned char>(b[j]);
    if (std::isdigit(left) && std::isdigit(right)) {
      size_t i0 = i, j0 = j;
      while (i0 < a.size() && a[i0] == '0') ++i0;
      while (j0 < b.size() && b[j0] == '0') ++j0;
      size_t ie = i0, je = j0;
      while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
      while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
      if (ie - i0 != je - j0) return ie - i0 < je - j0;
      const int cmp = a.compare(i0, ie - i0, b, j0, je - j0);
      if (cmp != 0) return cmp < 0;
      while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
      while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
      continue;
    }
    const int x = std::tolower(left), y = std::tolower(right);
    if (x != y) return x < y;
    ++i;
    ++j;
  }
  return a.size() < b.size();
}

// Scan one bounded line at a time. Incomplete EOF tails and overlong records
// are ignored, matching the firmware exporter's append-and-recovery model.
void scanLines(const char* path, const std::function<void(const std::string&, uint64_t, uint32_t)>& onLine) {
  HalFile file = Storage.open(path);
  if (!file || file.isDirectory()) return;
  char buffer[kChunkBytes];
  std::string line;
  line.reserve(320);
  uint64_t position = 0, start = 0;
  bool overflow = false;
  int bytes = 0;
  while ((bytes = file.read(buffer, sizeof(buffer))) > 0) {
    for (int i = 0; i < bytes; ++i) {
      const char c = buffer[i];
      ++position;
      if (c == '\n') {
        if (!overflow) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          onLine(line, start, static_cast<uint32_t>(position - start));
        }
        line.clear();
        overflow = false;
        start = position;
      } else if (!overflow && line.size() < kMaxRecordBytes) {
        line.push_back(c);
      } else {
        overflow = true;
      }
    }
  }
  // No callback for the final unterminated line: it may be a partial write.
}

bool vocabularySummary(const std::string& line, RecordSummary& out) {
  if (line.empty()) return false;
  JsonDocument filter;
  filter["v"] = true;
  filter["title"] = true;
  filter["author"] = true;
  filter["id"] = true;
  filter["headword"] = true;
  filter["chapter"] = true;
  filter["timestamp"] = true;
  filter["page"] = true;
  filter["spine"] = true;
  filter["start_offset"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, line, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  if ((doc["v"] | 0) != 1) return false;
  const char* title = doc["title"] | "";
  const char* word = doc["headword"] | "";
  const char* id = doc["id"] | "";
  if (!*title || !*word || strlen(id) != 32) return false;
  for (const char* p = id; *p; ++p)
    if (!std::isxdigit(static_cast<unsigned char>(*p))) return false;
  out.id = "vocab:";
  out.id += id;
  std::transform(out.id.begin() + 6, out.id.end(), out.id.begin() + 6,
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  out.title = title;
  out.author = doc["author"] | "";
  out.label = shortLabel(word, 90);
  out.chapter = shortLabel(doc["chapter"] | "", 90);
  out.timestamp = doc["timestamp"].is<uint32_t>() ? doc["timestamp"].as<uint32_t>() : 0;
  const uint32_t page = doc["page"] | 0UL;
  const uint32_t spine = doc["spine"].is<uint32_t>() ? doc["spine"].as<uint32_t>() : UINT16_MAX;
  out.page = static_cast<uint16_t>(std::min(page, static_cast<uint32_t>(UINT16_MAX)));
  out.spine = static_cast<uint16_t>(std::min(spine, static_cast<uint32_t>(UINT16_MAX)));
  out.startOffset = doc["start_offset"].is<uint32_t>() ? doc["start_offset"].as<uint32_t>() : UINT32_MAX;
  return true;
}

std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \r\n\t\v\f");
  if (first == std::string::npos) return {};
  const auto last = text.find_last_not_of(" \r\n\t\v\f");
  return text.substr(first, last - first + 1);
}

bool clippingSummary(const std::string& block, RecordSummary& out, const uint64_t selectedBook = 0) {
  const auto titleEnd = block.find('\n');
  if (titleEnd == std::string::npos) return false;
  const auto metaEnd = block.find('\n', titleEnd + 1);
  if (metaEnd == std::string::npos) return false;
  std::string heading = trim(block.substr(0, titleEnd));
  if (heading.compare(0, 3, "\xef\xbb\xbf") == 0) heading = trim(heading.substr(3));
  const std::string metadata = trim(block.substr(titleEnd + 1, metaEnd - titleEnd - 1));
  const auto contentStart = block.find("\n\n", titleEnd + 1);
  if (contentStart == std::string::npos) return false;
  const std::string body = trim(block.substr(contentStart + 2));
  if (body.empty()) return false;

  constexpr char marker[] = "- Your Highlight on Page ";
  if (metadata.compare(0, sizeof(marker) - 1, marker) != 0) return false;
  const char* digits = metadata.c_str() + sizeof(marker) - 1;
  if (!std::isdigit(static_cast<unsigned char>(*digits))) return false;
  unsigned long page = 0;
  while (*digits >= '0' && *digits <= '9') {
    page = std::min<unsigned long>(UINT16_MAX, page * 10 + (*digits++ - '0'));
  }
  const std::string suffix = trim(digits);
  if (!suffix.empty() && suffix.front() != '|') return false;
  out.page = static_cast<uint16_t>(page);

  const auto authorStart = heading.rfind(" (");
  if (authorStart != std::string::npos && !heading.empty() && heading.back() == ')') {
    out.title = heading.substr(0, authorStart);
    out.author = heading.substr(authorStart + 2, heading.size() - authorStart - 3);
  } else {
    out.title = heading;
    out.author.clear();
  }
  if (out.title.empty()) return false;
  const auto chapterStart = metadata.find(" | ");
  out.chapter.clear();
  if (chapterStart != std::string::npos) {
    const auto chapterEnd = metadata.find(" | ", chapterStart + 3);
    const std::string section = trim(metadata.substr(chapterStart + 3, chapterEnd - chapterStart - 3));
    if (section.compare(0, 8, "Added on") != 0) out.chapter = shortLabel(section, 90);
  }
  // A browser hash must never target the wrong clipping tombstone.
  if (selectedBook != 0 && bookKey(out.title, out.author) == selectedBook && heading.find('\r') == std::string::npos &&
      metadata.find('\r') == std::string::npos && body.find('\r') == std::string::npos &&
      !clippingfingerprint::hasUnsupportedTrimBoundary(heading) &&
      !clippingfingerprint::hasUnsupportedTrimBoundary(metadata) &&
      !clippingfingerprint::hasUnsupportedTrimBoundary(body))
    out.fingerprint = clippingfingerprint::fromUtf8(heading + "\n" + metadata + "\n" + body);
  out.label = shortLabel(body, 90);
  std::replace(out.label.begin(), out.label.end(), '\n', ' ');
  return true;
}

// Clippings are terminated by the exact separator, not just a blank line.
// A crash-partial trailing record is deliberately excluded.
void scanClippings(const std::function<void(const std::string&, uint64_t, uint32_t)>& onBlock) {
  std::string block;
  block.reserve(512);
  uint64_t start = 0;
  bool oversized = false;
  scanLines(kClippingsPath, [&](const std::string& line, uint64_t offset, uint32_t size) {
    if (line == "==========") {
      if (!oversized && !block.empty()) onBlock(block, start, static_cast<uint32_t>(offset + size - start));
      block.clear();
      oversized = false;
      start = offset + size;
      return;
    }
    if (block.empty() && !oversized) start = offset;
    if (oversized || block.size() + line.size() + 1 > kMaxRecordBytes) {
      oversized = true;
      return;
    }
    block += line;
    block.push_back('\n');
  });
}

std::string readRecord(const char* path, uint64_t offset, uint32_t length) {
  if (length == 0 || length > kMaxRecordBytes + 32 || offset > SIZE_MAX) return {};
  HalFile file = Storage.open(path);
  if (!file || !file.seek(static_cast<size_t>(offset))) return {};
  std::string raw;
  raw.resize(length);
  if (file.read(raw.data(), raw.size()) != static_cast<int>(raw.size())) return {};
  return raw;
}
}  // namespace

VocabularyHighlightsActivity::VocabularyHighlightsActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("VocabularyHighlights", renderer, input, true) {}

bool VocabularyHighlightsActivity::supportsManifest(const std::string& deviceManifest) {
  if (deviceManifest.empty()) return false;
  std::string raw;
  if (!Storage.readFileToString("VOCAB", deviceManifest, kManifestLimit, raw)) return false;
  JsonDocument filter;
  filter["local_library"]["source"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  return strcmp(doc["local_library"]["source"] | "", "vocabulary-highlights.v1") == 0;
}

void VocabularyHighlightsActivity::onEnter() {
  UiListActivity::onEnter();
  showDeleted = false;
  pendingSortDialog = false;
  pendingErrorDialog = false;
  pendingDeleteId.clear();
  learningState.load();
  screen = Screen::Sections;
  books.clear();
  entries.clear();
}

int VocabularyHighlightsActivity::listCount() const {
  if (screen == Screen::Sections) return 2;
  if (screen == Screen::Books) return static_cast<int>(books.size());
  return static_cast<int>(entries.size());
}

void VocabularyHighlightsActivity::scanBooks() {
  books.clear();
  entries.clear();
  booksOverflow = false;
  const auto onRecord = [this](const std::string& raw, uint64_t offset, uint32_t) {
    RecordSummary info;
    const bool valid = source == Source::Vocabulary ? vocabularySummary(raw, info) : clippingSummary(raw, info);
    if (!valid) return;
    const uint64_t key = bookKey(info.title, info.author);
    const auto it = std::find_if(books.begin(), books.end(), [key](const Book& book) { return book.key == key; });
    if (it != books.end()) {
      it->count++;
      it->latest = std::max(it->latest, info.timestamp ? info.timestamp : static_cast<uint32_t>(offset));
      return;
    }
    if (books.size() >= kMaxBooks) {
      booksOverflow = true;
      return;
    }
    Book book;
    book.key = key;
    book.title = shortLabel(info.title, 112);
    book.author = shortLabel(info.author, 80);
    book.count = 1;
    book.latest = info.timestamp ? info.timestamp : static_cast<uint32_t>(offset);
    books.push_back(std::move(book));
  };
  if (source == Source::Vocabulary)
    scanLines(kVocabularyPath, onRecord);
  else
    scanClippings(onRecord);
  sortBooks();
}

void VocabularyHighlightsActivity::scanEntries() {
  entries.clear();
  entriesOverflow = false;
  std::vector<std::pair<std::string, uint32_t>> clipOccurrences;
  const auto onRecord = [this, &clipOccurrences](const std::string& raw, uint64_t offset, uint32_t length) {
    RecordSummary info;
    const bool valid =
        source == Source::Vocabulary ? vocabularySummary(raw, info) : clippingSummary(raw, info, selectedBook);
    if (!valid || bookKey(info.title, info.author) != selectedBook) return;
    if (source == Source::Highlights && !info.fingerprint.empty()) {
      auto found = std::find_if(clipOccurrences.begin(), clipOccurrences.end(),
                                [&info](const auto& value) { return value.first == info.fingerprint; });
      if (found == clipOccurrences.end() && clipOccurrences.size() < kMaxEntries) {
        clipOccurrences.emplace_back(info.fingerprint, 0);
        found = std::prev(clipOccurrences.end());
      }
      if (found != clipOccurrences.end()) info.id = "clip:" + info.fingerprint + ":" + std::to_string(++found->second);
    }
    if (!showDeleted && !info.id.empty() && learningState.hidden(info.id)) return;
    if (entries.size() >= kMaxEntries) {
      entriesOverflow = true;
      return;
    }
    Entry entry;
    entry.offset = offset;
    entry.length = length;
    entry.timestamp = info.timestamp ? info.timestamp : static_cast<uint32_t>(offset);
    entry.startOffset = info.startOffset;
    entry.spine = info.spine;
    entry.page = info.page;
    entry.label = std::move(info.label);
    entry.chapter = std::move(info.chapter);
    entry.status = learningState.status(info.id);
    entry.hidden = learningState.hidden(info.id);
    if (entry.hidden && !showDeleted) return;
    entry.id = std::move(info.id);
    entries.push_back(std::move(entry));
  };
  if (source == Source::Vocabulary)
    scanLines(kVocabularyPath, onRecord);
  else
    scanClippings(onRecord);
  sortEntries();
}

void VocabularyHighlightsActivity::sortBooks() {
  std::stable_sort(books.begin(), books.end(), [this](const Book& a, const Book& b) {
    if (bookSort == Sort::Recent && a.latest != b.latest) return a.latest > b.latest;
    if (a.title != b.title) return beforeLabel(a.title, b.title);
    return beforeLabel(a.author, b.author);
  });
}

void VocabularyHighlightsActivity::sortEntries() {
  std::stable_sort(entries.begin(), entries.end(), [this](const Entry& a, const Entry& b) {
    if (entrySort == Sort::Alphabetical && source == Source::Vocabulary && a.label != b.label)
      return beforeLabel(a.label, b.label);
    if (entrySort == Sort::Recent) return a.timestamp > b.timestamp;
    if (a.spine != b.spine && a.spine != UINT16_MAX && b.spine != UINT16_MAX) return a.spine < b.spine;
    if (a.spine == UINT16_MAX && b.spine == UINT16_MAX && a.chapter != b.chapter && !a.chapter.empty() &&
        !b.chapter.empty())
      return beforeChapter(a.chapter, b.chapter);
    if (a.page != b.page) return a.page < b.page;
    if (a.startOffset != b.startOffset && a.startOffset != UINT32_MAX && b.startOffset != UINT32_MAX)
      return a.startOffset < b.startOffset;
    return a.offset < b.offset;
  });
}

void VocabularyHighlightsActivity::activateIndex(int index) {
  if (sortPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (screen == Screen::Sections) {
    source = index == 0 ? Source::Vocabulary : Source::Highlights;
    bookSort = Sort::Alphabetical;
    entrySort = source == Source::Vocabulary ? Sort::Alphabetical : Sort::ReadingOrder;
    scanBooks();
    screen = Screen::Books;
    nav.reset();
  } else if (screen == Screen::Books) {
    selectedBook = books[static_cast<size_t>(index)].key;
    scanEntries();
    screen = Screen::Entries;
    nav.reset();
  } else {
    openDetail(index);
    return;
  }
  requestUpdate();
}

void VocabularyHighlightsActivity::openDetail(int index) {
  const Entry& item = entries[static_cast<size_t>(index)];
  const char* path = source == Source::Vocabulary ? kVocabularyPath : kClippingsPath;
  std::string raw = readRecord(path, item.offset, item.length);
  if (raw.empty()) return;
  std::string heading = item.label, text;
  if (source == Source::Vocabulary) {
    JsonDocument filter;
    filter["definition"] = true;
    filter["headword"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return;
    heading = doc["headword"] | item.label.c_str();
    text = doc["definition"] | "";
  } else {
    raw.erase(std::remove(raw.begin(), raw.end(), '\r'), raw.end());
    const auto gap = raw.find("\n\n");
    if (gap == std::string::npos) return;
    const auto end = raw.rfind("\n==========");
    text = trim(raw.substr(gap + 2, end == std::string::npos ? std::string::npos : end - (gap + 2)));
  }
  if (text.empty()) return;
  auto detail =
      makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(heading), std::move(text));
  if (!detail) {
    LOG_ERR("VOCAB", "Insufficient memory for saved definition");
    return;
  }
  startActivityForResult(std::move(detail), [this](const ActivityResult&) { requestUpdate(); });
}

void VocabularyHighlightsActivity::onBackButton() {
  if (screen == Screen::Entries) {
    entries.clear();
    screen = Screen::Books;
    nav.reset();
    requestUpdate();
  } else if (screen == Screen::Books) {
    books.clear();
    screen = Screen::Sections;
    nav.reset();
    requestUpdate();
  } else {
    finish();
  }
}

void VocabularyHighlightsActivity::showSortOptions() {
  if (screen == Screen::Sections || sortPopup.isActive()) return;
  const char* bookChoices[] = {"Title A-Z", "Recently added", showDeleted ? "Hide deleted" : "Show deleted"};
  static constexpr const char* wordChoices[] = {"Word A-Z", "Reading order", "Recently added"};
  static constexpr const char* highlightChoices[] = {"Reading order", "Recently added"};
  const bool booksScreen = screen == Screen::Books;
  const bool isWords = source == Source::Vocabulary;
  const char* const* names = booksScreen ? bookChoices : isWords ? wordChoices : highlightChoices;
  const int count = booksScreen ? 3 : !isWords ? 2 : 3;
  const Sort current = booksScreen ? bookSort : entrySort;
  const int selected = booksScreen ? (current == Sort::Recent ? 1 : 0)
                       : isWords   ? (current == Sort::Recent         ? 2
                                      : current == Sort::ReadingOrder ? 1
                                                                      : 0)
                                   : (current == Sort::Recent ? 1 : 0);
  sortPopup.show("Sort", names, count, selected, [this, booksScreen, isWords](int index) {
    if (booksScreen) {
      if (index == 2)
        showDeleted = !showDeleted;
      else
        bookSort = index == 0 ? Sort::Alphabetical : Sort::Recent;
      sortBooks();
    } else {
      entrySort = isWords ? (index == 0   ? Sort::Alphabetical
                             : index == 1 ? Sort::ReadingOrder
                                          : Sort::Recent)
                          : (index == 0 ? Sort::ReadingOrder : Sort::Recent);
      sortEntries();
    }
    nav.reset();
    requestUpdate();
  });
  requestUpdate();
}

void VocabularyHighlightsActivity::showStateError() {
  static constexpr const char* options[] = {"OK"};
  const std::string explanation = learningState.error().empty()
                                      ? "This highlight cannot be safely matched to the WebUI. Edit it in the browser."
                                      : learningState.error();
  sortPopup.showMessage("Progress unavailable", explanation.c_str(), options, 1, 0, [](int) {});
  requestUpdate();
}

void VocabularyHighlightsActivity::showEntryOptions(const int index) {
  if (screen != Screen::Entries || index < 0 || index >= listCount()) {
    showSortOptions();
    return;
  }
  const Entry& item = entries[static_cast<size_t>(index)];
  const bool wasHidden = item.hidden;
  const std::string id = item.id;
  const char* readonlyChoices[] = {"Sort entries", "Why editing is disabled", "Cancel"};
  const char* normalChoices[] = {"Sort entries", "New", "Learning", "Mastered", "Delete from list", "Cancel"};
  const char* clipChoices[] = {"Sort entries", "Delete from list", "Cancel"};
  const char* hiddenChoices[] = {"Sort entries", "Restore entry", "Cancel"};
  const bool readonly = !learningState.writable() || id.empty();
  const char* const* choices = readonly                       ? readonlyChoices
                               : wasHidden                    ? hiddenChoices
                               : source == Source::Vocabulary ? normalChoices
                                                              : clipChoices;
  const int count = readonly || wasHidden || source == Source::Highlights ? 3 : 6;
  const int selected = readonly || wasHidden || source == Source::Highlights        ? 0
                       : item.status == VocabularyHighlightsState::Status::New      ? 1
                       : item.status == VocabularyHighlightsState::Status::Learning ? 2
                                                                                    : 3;
  sortPopup.show(source == Source::Vocabulary ? "Word actions" : "Highlight actions", item.label.c_str(), choices,
                 count, selected, [this, id, wasHidden, readonly](const int choice) {
                   if (choice == 0) {
                     pendingSortDialog = true;
                     return;
                   }
                   if (readonly) {
                     if (choice == 1) pendingErrorDialog = true;
                     return;
                   }
                   if (wasHidden) {
                     if (choice == 1) {
                       if (!learningState.setHidden(id, false)) {
                         pendingErrorDialog = true;
                         return;
                       }
                       scanEntries();
                       nav.reset();
                       requestUpdate();
                     }
                     return;
                   }
                   if (source == Source::Vocabulary && choice >= 1 && choice <= 3) {
                     const auto status = choice == 1   ? VocabularyHighlightsState::Status::New
                                         : choice == 2 ? VocabularyHighlightsState::Status::Learning
                                                       : VocabularyHighlightsState::Status::Mastered;
                     if (!learningState.setStatus(id, status)) {
                       pendingErrorDialog = true;
                       return;
                     }
                     scanEntries();
                     nav.reset();
                     requestUpdate();
                   } else if (choice == (source == Source::Vocabulary ? 4 : 1)) {
                     pendingDeleteId = id;
                   }
                 });
  requestUpdate();
}

void VocabularyHighlightsActivity::showDeleteConfirmation(const std::string& id) {
  const char* const choices[] = {"Cancel", source == Source::Vocabulary ? "Hide word" : "Hide highlight"};
  const char* sourceNotice = source == Source::Vocabulary ? "The original Vocabulary.txt record will not be deleted."
                                                          : "The original My Clippings.txt record will not be deleted.";
  sortPopup.showMessage("Delete from list?", sourceNotice, choices, 2, 0, [this, id](const int choice) {
    if (choice != 1) return;
    if (!learningState.setHidden(id, true)) {
      pendingErrorDialog = true;
      return;
    }
    scanEntries();
    nav.reset();
    requestUpdate();
  });
  requestUpdate();
}

void VocabularyHighlightsActivity::onRowLongPress(const int index) {
  if (screen == Screen::Entries) {
    showEntryOptions(index);
    return;
  }
  showSortOptions();
}

bool VocabularyHighlightsActivity::handleCustomInput() {
  const bool handled = sortPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  // Never open another popup from within OptionPopup's own callback: doing so
  // would replace that std::function while it is executing.
  if (pendingSortDialog) {
    pendingSortDialog = false;
    showSortOptions();
    return true;
  }
  if (pendingErrorDialog) {
    pendingErrorDialog = false;
    showStateError();
    return true;
  }
  if (!pendingDeleteId.empty()) {
    std::string id = std::move(pendingDeleteId);
    pendingDeleteId.clear();
    showDeleteConfirmation(id);
    return true;
  }
  return handled;
}

bool VocabularyHighlightsActivity::handleButtons() {
  if (sortPopup.isActive()) return false;
  if (screen != Screen::Sections && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 700)) {
    if (screen == Screen::Entries) {
      showEntryOptions(nav.selected);
      return true;
    }
    showSortOptions();
    return true;
  }
  return UiListActivity::handleButtons();
}

const char* VocabularyHighlightsActivity::headerTitle() const {
  if (screen == Screen::Sections) return "Vocabulary & Highlights";
  if (screen == Screen::Books) return source == Source::Vocabulary ? "Vocabulary Books" : "Highlight Books";
  return source == Source::Vocabulary ? "Saved Words" : "Saved Highlights";
}

void VocabularyHighlightsActivity::refreshWindow(int start) {
  start = std::max(0, start);
  const int count = std::min(static_cast<int>(kVisibleRows), std::max(0, listCount() - start));
  for (int slot = 0; slot < count; ++slot) {
    const int index = start + slot;
    auto& label = labels[slot];
    auto& subtitle = subtitles[slot];
    if (screen == Screen::Sections) {
      label = index == 0 ? "Vocabulary" : "Highlights";
      subtitle = index == 0 ? "Browse saved words and definitions" : "Browse highlighted passages";
    } else if (screen == Screen::Books) {
      const Book& book = books[static_cast<size_t>(index)];
      label = book.title;
      subtitle =
          book.author + " - " + std::to_string(book.count) + (source == Source::Vocabulary ? " words" : " highlights");
    } else {
      const Entry& entry = entries[static_cast<size_t>(index)];
      label = entry.label;
      subtitle = entry.chapter;
      if (!subtitle.empty()) subtitle += " - ";
      subtitle += "Page " + std::to_string(entry.page);
      if (entry.hidden)
        subtitle += " - Deleted";
      else if (source == Source::Vocabulary) {
        if (entry.status == VocabularyHighlightsState::Status::Learning)
          subtitle += " - Learning";
        else if (entry.status == VocabularyHighlightsState::Status::Mastered)
          subtitle += " - Mastered";
      }
    }
    fui::ListItem row;
    row.label = label.c_str();
    row.subtitle = subtitle.c_str();
    row.icon = listIconFor(UIIcon::Bookmark, 32);
    row.actionValue = static_cast<int16_t>(index);
    rowItems[slot] = row;
  }
}

void VocabularyHighlightsActivity::buildScreen(UiScreen& screenUi) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screenUi.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screenUi.spacer(static_cast<int16_t>(metrics.verticalSpacing));
#if FREEINK_DEVICE_X3
  constexpr const char* kBookLimitNotice = "First 64 books shown";
  constexpr const char* kEntryLimitNotice = "First 192 entries shown";
#else
  constexpr const char* kBookLimitNotice = "First 128 books shown";
  constexpr const char* kEntryLimitNotice = "First 512 entries shown";
#endif
  if (screen == Screen::Books && booksOverflow)
    screenUi.centeredText(kBookLimitNotice, screenUi.theme().bodyText);
  else if (screen == Screen::Entries && entriesOverflow)
    screenUi.centeredText(kEntryLimitNotice, screenUi.theme().bodyText);
  if (listCount() == 0) {
    screenUi.centeredText("No saved entries", screenUi.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screenUi, props);
  refreshWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(nav.top);
  props.itemsWindowCount =
      static_cast<uint16_t>(std::min(static_cast<int>(kVisibleRows), std::max(0, listCount() - nav.top)));
  screenUi.list(props);
}

void VocabularyHighlightsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels("Back", "Open", "Previous", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void VocabularyHighlightsActivity::render(RenderLock&& lock) {
  if (sortPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
