#pragma once
#if (defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND) || defined(CROSSPOINT_LOCALSEND_HOST_TEST)

#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "LocalSendSession.h"

namespace localsend {

// Fixed-size SD records permit newest-first random access without an in-RAM
// catalog. The FreeInk list/grid action indexes are signed 16-bit, hence the
// practical 32,767-item ceiling; all actual received files remain on SD.
class History {
 public:
  struct Entry {
    char folder[96]{};
    char name[129]{};
  };
  static_assert(sizeof(Entry) == 225, "SD history record layout must stay fixed");
  static constexpr int MAX_ENTRIES = 32767;
  static constexpr const char* PATH = "/.crosspoint/localsend/history.bin";
  static constexpr const char* TEMP = "/.crosspoint/localsend/history.tmp";
  static constexpr const char* BACKUP = "/.crosspoint/localsend/history.bak";

  int count() const {
    if (!recover()) return 0;
    auto file = Storage.open(PATH, O_RDONLY);
    if (!file) return 0;
    const uint64_t size = file.fileSize64();
    file.close();
    return static_cast<int>(std::min<uint64_t>(size / sizeof(Entry), MAX_ENTRIES));
  }

  int readWindow(int first, Entry* entries, int capacity) const {
    if (!entries || capacity <= 0 || first < 0 || !recover()) return 0;
    auto file = Storage.open(PATH, O_RDONLY);
    if (!file) return 0;
    const uint64_t total = file.fileSize64() / sizeof(Entry);
    const uint64_t available = std::min<uint64_t>(total, MAX_ENTRIES);
    int read = 0;
    while (uint64_t(first + read) < available && read < capacity) {
      Entry entry{};
      const uint64_t offset = (total - 1 - uint64_t(first + read)) * sizeof(Entry);
      if (!file.seek64(offset) || file.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry)) || !valid(entry))
        break;
      entries[read++] = entry;
    }
    file.close();
    return read;
  }

  bool read(int index, Entry& entry) const { return readWindow(index, &entry, 1) == 1; }

  bool append(const char* folder, const char* name) const {
    if (!safeDestinationFolder(folder) || !safeReceiveName(name) || !Storage.ensureDirectoryExists("/.crosspoint") ||
        !Storage.ensureDirectoryExists("/.crosspoint/localsend"))
      return false;
    if (!recover()) return false;
    if (count() == MAX_ENTRIES && !eraseAt(MAX_ENTRIES - 1)) return false;
    auto file = Storage.open(PATH, O_RDWR | O_CREAT);
    if (!file) return false;
    uint64_t size = file.fileSize64();
    // Ignore and repair an incomplete trailing record from interrupted power.
    if (size % sizeof(Entry)) {
      size -= size % sizeof(Entry);
      if (!file.truncate(size)) {
        file.close();
        return false;
      }
    }
    Entry entry{};
    std::snprintf(entry.folder, sizeof(entry.folder), "%s", folder);
    std::snprintf(entry.name, sizeof(entry.name), "%s", name);
    const bool written = file.seek64(size) && file.write(&entry, sizeof(entry)) == sizeof(entry);
    if (!written) file.truncate(size);
    file.flush();
    return file.close() && written;
  }

  bool eraseAt(int index) const { return rewrite(index, nullptr) == 1; }
  int erasePath(const char* path) const { return rewrite(-1, path); }

  // Clear only the received-file index. Books and images are never removed.
  // Drop backups first so a later count() cannot restore the old history.
  bool clear() const {
    if (!recover()) return false;
    if (Storage.exists(TEMP) && !Storage.remove(TEMP)) return false;
    if (Storage.exists(BACKUP) && !Storage.remove(BACKUP)) return false;
    return !Storage.exists(PATH) || Storage.remove(PATH);
  }

 private:
  bool recover() const {
    if (Storage.exists(PATH)) {
      if (Storage.exists(BACKUP)) Storage.remove(BACKUP);
      return true;
    }
    return !Storage.exists(BACKUP) || Storage.rename(BACKUP, PATH);
  }

  static bool valid(const Entry& entry) {
    return std::memchr(entry.folder, 0, sizeof(entry.folder)) && std::memchr(entry.name, 0, sizeof(entry.name)) &&
           safeDestinationFolder(entry.folder) && safeReceiveName(entry.name);
  }

  // Write an edited copy before replacing the original: deletion must never
  // destroy received files, and an interrupted edit must preserve old history.
  int rewrite(int newestIndex, const char* path) const {
    if (!recover()) return -1;
    auto source = Storage.open(PATH, O_RDONLY);
    if (!source) return 0;
    const uint64_t total = source.fileSize64() / sizeof(Entry);
    if (newestIndex >= 0 && uint64_t(newestIndex) >= total) {
      source.close();
      return 0;
    }
    if (Storage.exists(TEMP) && !Storage.remove(TEMP)) {
      source.close();
      return -1;
    }
    auto output = Storage.open(TEMP, O_WRITE | O_CREAT | O_EXCL);
    if (!output) {
      source.close();
      return -1;
    }
    int removed = 0;
    bool ok = true;
    for (uint64_t i = 0; i < total; ++i) {
      Entry entry{};
      if (source.read(&entry, sizeof(entry)) != static_cast<int>(sizeof(entry)) || !valid(entry)) {
        ok = false;
        break;
      }
      const bool byIndex = newestIndex >= 0 && i == total - 1 - uint64_t(newestIndex);
      char fullPath[256];
      std::snprintf(fullPath, sizeof(fullPath), "%s%s%s", entry.folder, std::strcmp(entry.folder, "/") ? "/" : "",
                    entry.name);
      if (byIndex || (path && !std::strcmp(fullPath, path))) {
        ++removed;
      } else if (output.write(&entry, sizeof(entry)) != sizeof(entry)) {
        ok = false;
        break;
      }
    }
    output.flush();
    ok = output.close() && ok;
    source.close();
    if (!ok || removed == 0) {
      Storage.remove(TEMP);
      return ok ? 0 : -1;
    }
    if ((Storage.exists(BACKUP) && !Storage.remove(BACKUP)) || !Storage.rename(PATH, BACKUP)) {
      Storage.remove(TEMP);
      return -1;
    }
    if (!Storage.rename(TEMP, PATH)) {
      // Preserve BACKUP for recovery if restoring the original fails.
      Storage.rename(BACKUP, PATH);
      Storage.remove(TEMP);
      return -1;
    }
    Storage.remove(BACKUP);
    return removed;
  }
};

}  // namespace localsend
#endif
