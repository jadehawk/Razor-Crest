#include "ReadingStatsLegacyImport.h"

#include <HalStorage.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"

namespace readingstats::legacy {
namespace {
void setTotals(Snapshot& delta, uint64_t seconds, uint32_t sessions, uint32_t turns) {
  delta.totalSecondsLo = static_cast<uint32_t>(seconds);
  delta.totalSecondsHi = static_cast<uint32_t>(seconds >> 32);
  delta.sessions = sessions;
  delta.pageTurns = turns;
  delta.seal();
}

bool readCrossInkStats(const std::string& filename, Counters& totals) {
  HalFile file;
  if (!Storage.openFileForRead("RST", filename, file)) return false;
  const size_t length = file.fileSize();
  uint8_t buffer[73] = {};
  const int got = length <= sizeof(buffer) ? file.read(buffer, length) : -1;
  file.close();
  return got == static_cast<int>(length) && crossInkBook(buffer, length, totals);
}

bool tryCrossInkBook(const char* path, const char* title, const char* author, bool hasGlobal, ImportReport& report) {
  if (!path || *path != '/' || strstr(path, "..") || !Storage.exists(path)) return true;
  const size_t size = strlen(path);
  if (size < 6 || strcmp(path + size - 5, ".epub") != 0) return true;

  // CrossInk Epub::cachePathForFilePath: stable FNV-1a 64-bit decimal.
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char* it = reinterpret_cast<const unsigned char*>(path); *it; ++it)
    hash = (hash ^ *it) * 1099511628211ull;
  const std::string dir = "/.crosspoint/epub_" + std::to_string(hash);
  std::string statsPath;
  for (const char* name : {"stats_v5.bin", "stats_v4.bin", "stats.bin"}) {
    const std::string candidate = dir + "/" + name;
    if (Storage.exists(candidate.c_str())) {
      statsPath = candidate;
      break;
    }
  }
  if (statsPath.empty()) return true;

  Counters numbers{};
  if (!readCrossInkStats(statsPath, numbers)) {
    ++report.errors;
    return false;
  }
  Snapshot delta{};
  setTotals(delta, numbers.totalSeconds, numbers.sessions, numbers.turns);
  const std::string key = std::string("crossink:book:") + path;
  const auto result =
      readingstats::importLegacy(LegacySource::CrossInk, key.c_str(), path, title, author, delta, !hasGlobal);
  ++report.booksSeen;
#if defined(ARDUINO)
  yield();
#endif
  if (result == LegacyResult::Imported)
    ++report.imported;
  else if (result == LegacyResult::AlreadyImported)
    ++report.alreadyImported;
  else {
    if (result == LegacyResult::OtherSource) report.otherSourceAlreadyImported = true;
    ++report.errors;
    return false;
  }
  return true;
}

bool importCrossInk(ImportReport& report) {
  const Preview preview = inspect(Source::CrossInk);
  const bool hasGlobal = preview.present;
  if (hasGlobal) {
    report.sourcePresent = true;
    if (!preview.supported) return false;
    Snapshot delta{};
    setTotals(delta, preview.totals.totalSeconds, preview.totals.sessions, preview.totals.turns);
    const auto outcome =
        readingstats::importLegacy(LegacySource::CrossInk, "crossink:global", nullptr, nullptr, nullptr, delta);
    if (outcome == LegacyResult::Imported)
      ++report.imported;
    else if (outcome == LegacyResult::AlreadyImported)
      ++report.alreadyImported;
    else {
      if (outcome == LegacyResult::OtherSource) report.otherSourceAlreadyImported = true;
      ++report.errors;
      return false;
    }
  }
  // The recent-books store is shared between CrossInk and CrossPoint, and
  // preserves the exact path needed to resolve CrossInk's hashed cache name.
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!tryCrossInkBook(book.path.c_str(), book.title.c_str(), book.author.c_str(), hasGlobal, report)) return false;
  }
  // Also revisit the native BIN index; it can contain books no longer recent.
  for (uint32_t i = 0, count = readingstats::bookCount(); i < count; ++i) {
    BookEntry book{};
    if (!readingstats::bookAt(i, book)) return false;
    if (!tryCrossInkBook(book.path, book.title, book.author, hasGlobal, report)) return false;
  }
  report.sourcePresent |= report.booksSeen != 0;
  return report.sourcePresent;
}

bool consumeVCodexBook(const BookRecord& book, void* context) {
  auto& report = *static_cast<ImportReport*>(context);
  const std::string key = "vcodex:" + book.key;
  const auto result = readingstats::importLegacy(LegacySource::VCodex, key.c_str(), book.path.c_str(),
                                                 book.title.c_str(), book.author.c_str(), book.delta);
  if (result == LegacyResult::Imported)
    ++report.imported;
  else if (result == LegacyResult::AlreadyImported)
    ++report.alreadyImported;
  else {
    if (result == LegacyResult::OtherSource) report.otherSourceAlreadyImported = true;
    ++report.errors;
    return false;
  }
  return true;
}
}  // namespace

ImportReport importSource(const Source source) {
  ImportReport report{};
  if (!readingstats::recoverLegacyImport()) {
    ++report.errors;
    return report;
  }
  if (source == Source::CrossInk) {
    report.supported = importCrossInk(report);
    report.success = report.supported && report.errors == 0;
  } else {
    const Preview preview = inspect(Source::VCodex);
    report.sourcePresent = preview.present;
    report.supported = preview.supported;
    if (preview.supported)
      report.success = scanVCodexBooks(consumeVCodexBook, &report, report.booksSeen) && report.errors == 0;
  }
  return report;
}
}  // namespace readingstats::legacy
