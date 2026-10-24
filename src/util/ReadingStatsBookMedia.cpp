#include "ReadingStatsBookMedia.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BookProgress.h"
#include "RecentBooksStore.h"

namespace readingstats::media {
namespace {
constexpr char kDir[] = "/.crosspoint/reading-stats/covers";
constexpr uint8_t kProgressMarker = 0xA7;

bool ensureDir() {
  return (Storage.exists("/.crosspoint") || Storage.mkdir("/.crosspoint")) &&
         (Storage.exists("/.crosspoint/reading-stats") || Storage.mkdir("/.crosspoint/reading-stats")) &&
         (Storage.exists(kDir) || Storage.mkdir(kDir));
}
std::string pathFor(uint64_t hash, const char* ext) {
  char path[100];
  snprintf(path, sizeof(path), "%s/%016llx.%s", kDir,
           static_cast<unsigned long long>(hash), ext);
  return path;
}
const RecentBook* findRecent(const BookEntry& entry) {
  // Prefer exact paths. Imported stats can point to an older book location;
  // a unique title+author match safely recovers the live Home cover/progress.
  const RecentBook* found = nullptr;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!Storage.exists(book.path.c_str())) continue;
    if (entry.path[0] && book.path == entry.path) return &book;
    if (!entry.title[0] || book.title != entry.title ||
        (entry.author[0] && book.author != entry.author)) continue;
    if (found) return nullptr;  // Ambiguous title: never borrow another book's media.
    found = &book;
  }
  return found;
}
std::string sourcePath(const BookEntry& entry) {
  if (entry.path[0] && Storage.exists(entry.path)) return entry.path;
  const RecentBook* recent = findRecent(entry);
  return recent ? recent->path : std::string();
}
int cachedProgress(uint64_t hash) {
  if (!hash) return -1;
  HalFile file;
  if (!Storage.openFileForRead("RST", pathFor(hash, "pct"), file)) return -1;
  uint8_t data[2] = {};
  const int count = file.read(data, sizeof(data));
  file.close();
  return count == sizeof(data) && data[0] == kProgressMarker && data[1] <= 100 ? data[1] : -1;
}
void saveProgress(uint64_t hash, int value) {
  if (!hash || value < 0 || value > 100 || cachedProgress(hash) == value || !ensureDir()) return;
  HalFile file;
  if (!Storage.openFileForWrite("RST", pathFor(hash, "pct"), file)) return;
  const uint8_t data[2] = {kProgressMarker, static_cast<uint8_t>(value)};
  if (file.write(data, sizeof(data)) != sizeof(data)) LOG_ERR("RST", "Progress cache write failed");
  file.close();
}
bool validBmp(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("RST", path, file)) return false;
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok &&
                     bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  file.close();
  return valid;
}
bool copyCover(const std::string& source, const std::string& destination) {
  if (!validBmp(source)) return false;
  if (!ensureDir() || source.empty()) return false;
  HalFile input;
  if (!Storage.openFileForRead("RST", source, input)) return false;
  const std::string tmp = destination + ".tmp";
  Storage.remove(tmp.c_str());
  HalFile output;
  if (!Storage.openFileForWrite("RST", tmp, output)) {
    input.close();
    return false;
  }
  uint8_t buffer[192];
  uint64_t remaining = input.fileSize();
  // Thumbnails only: reject excessively large or incomplete bitmap files.
  bool ok = remaining >= 54 && remaining <= 128 * 1024;
  while (ok && remaining) {
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), remaining));
    const int count = input.read(buffer, requested);
    if (count != static_cast<int>(requested) || output.write(buffer, requested) != requested) {
      ok = false;
      break;
    }
    remaining -= requested;
  }
  output.flush();
  ok = output.close() && ok;
  input.close();
  if (ok && validBmp(tmp) && Storage.rename(tmp.c_str(), destination.c_str())) return true;
  Storage.remove(tmp.c_str());
  return false;
}
// Use the already-generated Home thumbnail, regardless of Home theme or thumb
// height, if the EPUB parser could not generate the list/hero's ideal height.
std::string existingThumb(const std::string& cachePath) {
  auto dir = Storage.open(cachePath.c_str());
  if (!dir || !dir.isDirectory()) return {};
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory() || file.fileSize() < 54 || file.fileSize() > 128 * 1024) continue;
    char name[64] = {};
    file.getName(name, sizeof(name));
    if (strncmp(name, "thumb_", 6) != 0) continue;
    const size_t length = strlen(name);
    if (length <= 10 || strcmp(name + length - 4, ".bmp") != 0) continue;
    return cachePath + "/" + name;
  }
  return {};
}
}  // namespace

std::string coverPath(uint64_t hash, int height) {
  if (!hash) return {};
  const auto preferred = pathFor(hash, height > 90 ? "hero_bw_fill.bmp" : "list_bw_fill.bmp");
  if (Storage.exists(preferred.c_str())) return preferred;
  // If the original EPUB was removed, retain previously captured covers.
  const auto priorNative = pathFor(hash, height > 90 ? "hero_bw_native.bmp" : "list_bw_native.bmp");
  if (Storage.exists(priorNative.c_str())) return priorNative;
  const auto priorClean = pathFor(hash, height > 90 ? "hero_clean.bmp" : "list_clean.bmp");
  if (Storage.exists(priorClean.c_str())) return priorClean;
  const auto previous = pathFor(hash, height > 90 ? "hero.bmp" : "list.bmp");
  if (Storage.exists(previous.c_str())) return previous;
  const auto legacy = pathFor(hash, "bmp");
  return Storage.exists(legacy.c_str()) ? legacy : preferred;
}

uint64_t hashSourcePath(const char* path) {
  uint64_t hash = 14695981039346656037ULL;
  if (path)
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
      hash = (hash ^ *p) * 1099511628211ULL;
  return hash;
}
std::string bookSource(const BookEntry& entry) { return sourcePath(entry); }

std::string author(const BookEntry& entry) {
  if (entry.author[0]) return entry.author;
  const RecentBook* recent = findRecent(entry);
  return recent ? recent->author : std::string();
}

int progress(const BookEntry& entry, bool refresh) {
  const int previous = cachedProgress(entry.hash());
  if (!refresh) return previous;
  const std::string source = sourcePath(entry);
  if (source.empty()) return previous;
  const int value = loadBookProgress(source);
  if (value >= 0) {
    saveProgress(entry.hash(), value);
    return value;
  }
  return previous;
}

bool ensureCover(const BookEntry& entry, int height) {
  if (!entry.hash()) return false;
  // Regenerate independently at the actual target resolution; never enlarge
  // a dithered list thumbnail to fill the hero card.
  const std::string destination = pathFor(entry.hash(), height > 90 ? "hero_bw_fill.bmp" : "list_bw_fill.bmp");
  if (Storage.exists(destination.c_str())) {
    if (validBmp(destination)) return true;
    Storage.remove(destination.c_str());
  }
  const std::string source = sourcePath(entry);
  if (source.empty()) return false;

  std::string thumb, fallback;
  if (FsHelpers::hasReflowableBookExtension(source)) {
    auto book = makeUniqueNoThrow<Epub>(source, "/.crosspoint");
    if (!book) return false;
    thumb = book->getThumbBmpPath(height);
    fallback = existingThumb(book->getCachePath());
    if (!Storage.exists(thumb.c_str()) && !book->generateThumbBmpFromSource(height))
      thumb = fallback;
  } else if (FsHelpers::hasXtcExtension(source)) {
    auto book = makeUniqueNoThrow<Xtc>(source, "/.crosspoint");
    if (!book) return false;
    thumb = book->getThumbBmpPath(height);
    fallback = existingThumb(book->getCachePath());
    if (!Storage.exists(thumb.c_str()) && (!book->load() || !book->generateThumbBmp(height)))
      thumb = fallback;
  } else {
    return false;
  }
  if (!Storage.exists(thumb.c_str())) thumb = fallback;
  return copyCover(thumb, destination);
}

void remove(uint64_t hash) {
  if (!hash) return;
  const auto legacy = pathFor(hash, "bmp");
  const auto hero = pathFor(hash, "hero.bmp");
  const auto list = pathFor(hash, "list.bmp");
  const auto cleanHero = pathFor(hash, "hero_clean.bmp");
  const auto cleanList = pathFor(hash, "list_clean.bmp");
  const auto nativeHero = pathFor(hash, "hero_bw_native.bmp");
  const auto nativeList = pathFor(hash, "list_bw_native.bmp");
  const auto fillHero = pathFor(hash, "hero_bw_fill.bmp");
  const auto fillList = pathFor(hash, "list_bw_fill.bmp");
  const auto pct = pathFor(hash, "pct");
  Storage.remove(legacy.c_str());
  Storage.remove(hero.c_str());
  Storage.remove(list.c_str());
  Storage.remove(cleanHero.c_str());
  Storage.remove(cleanList.c_str());
  Storage.remove(nativeHero.c_str());
  Storage.remove(nativeList.c_str());
  Storage.remove(fillHero.c_str());
  Storage.remove(fillList.c_str());
  Storage.remove(pct.c_str());
}

}  // namespace readingstats::media
