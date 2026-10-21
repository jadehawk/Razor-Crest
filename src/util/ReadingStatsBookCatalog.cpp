#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "ReadingStatsStore.h"

namespace readingstats {
namespace {
constexpr char kPath[] = "/.crosspoint/reading-stats/books_v1.bin";
constexpr uint32_t kMaxEntries = 2048;
constexpr char kDir[] = "/.crosspoint/reading-stats";

template <size_t N>
void copyBounded(char (&dest)[N], const char* src) {
  if (!src) return;
  const size_t size = strlen(src);
  const size_t keep = size < N ? size : N - 1;
  memcpy(dest, src, keep);
  dest[keep] = 0;
}

uint32_t entryCount(HalFile& file) { return static_cast<uint32_t>(file.fileSize() / sizeof(BookEntry)); }
bool readIndexEntry(HalFile& file, const uint32_t position, BookEntry& entry) {
  const uint64_t offset = static_cast<uint64_t>(position) * sizeof(BookEntry);
  if (offset + sizeof(BookEntry) > file.fileSize() || !file.seek64(offset)) return false;
  return file.read(&entry, sizeof(entry)) == static_cast<int>(sizeof(entry)) && entry.valid();
}
}  // namespace

uint32_t bookCount() {
  HalFile file;
  if (!Storage.openFileForRead("RST", kPath, file)) return 0;
  const uint32_t size = entryCount(file);
  file.close();
  return size > kMaxEntries ? kMaxEntries : size;
}

bool bookAt(const uint32_t index, BookEntry& entry) {
  if (index >= kMaxEntries) return false;
  HalFile file;
  if (!Storage.openFileForRead("RST", kPath, file)) return false;
  const bool found = readIndexEntry(file, index, entry);
  file.close();
  return found;
}

bool ensureBookIndexed(const uint64_t hash, const char* path, const char* title, const char* author) {
  if (!path || !*path) return false;
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  if (!Storage.exists(kDir) && !Storage.mkdir(kDir)) return false;
  HalFile file = Storage.open(kPath, O_RDWR | O_CREAT);
  if (!file) return false;
  const uint64_t bytes = file.fileSize();
  const uint32_t count = entryCount(file);
  if (count >= kMaxEntries) {
    file.close();
    LOG_ERR("RST", "Book index full (%u)", static_cast<unsigned>(count));
    return false;
  }
  BookEntry entry{};
  for (uint32_t i = 0; i < count; ++i) {
    if (!readIndexEntry(file, i, entry)) {
      file.close();
      LOG_ERR("RST", "Book index invalid at %u; retaining source", static_cast<unsigned>(i));
      return false;
    }
    if (entry.hash() == hash) {
      file.close();
      return true;
    }
  }
  // A partially appended last record is safe to discard; all complete
  // entries have their own checksums. This is not a rewrite of the catalog.
  const uint64_t end = static_cast<uint64_t>(count) * sizeof(BookEntry);
  if (bytes != end && !file.truncate(end)) {
    file.close();
    return false;
  }
  BookEntry fresh{};
  fresh.setHash(hash);
  copyBounded(fresh.path, path);
  copyBounded(fresh.title, title && *title ? title : path);
  copyBounded(fresh.author, author);
  fresh.seal();
  if (!file.seek64(end)) {
    file.close();
    return false;
  }
  const bool written = file.write(&fresh, sizeof(fresh)) == sizeof(fresh);
  file.flush();
  const bool closed = file.close();
  return written && closed;
}
}  // namespace readingstats
