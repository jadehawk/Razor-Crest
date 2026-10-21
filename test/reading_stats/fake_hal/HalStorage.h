#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>

constexpr int O_RDWR = 0x02;
constexpr int O_CREAT = 0x40;

struct FakeHalStorage;
struct HalFile {
  FakeHalStorage* storage = nullptr;
  std::string path;
  std::string content;
  uint64_t position = 0;
  bool writable = false;
  bool opened = false;
  explicit operator bool() const { return opened; }
  size_t fileSize() const { return content.size(); }
  bool seek64(uint64_t target) {
    if (!opened || target > content.size()) return false;
    position = target;
    return true;
  }
  int read(void* dest, size_t count) {
    if (!opened) return -1;
    const size_t amount = std::min<size_t>(count, content.size() - static_cast<size_t>(position));
    if (amount) std::memcpy(dest, content.data() + position, amount);
    position += amount;
    return static_cast<int>(amount);
  }
  size_t write(const void* data, size_t count) {
    if (!opened || !writable) return 0;
    if (position + count > content.size()) content.resize(static_cast<size_t>(position + count));
    std::memcpy(content.data() + position, data, count);
    position += count;
    return count;
  }
  bool truncate(uint64_t count) {
    if (!writable || count > content.size()) return false;
    content.resize(static_cast<size_t>(count));
    if (position > count) position = count;
    return true;
  }
  void flush() {}
  bool close();
};

struct FakeHalStorage {
  std::map<std::string, std::string> files;
  std::set<std::string> directories;
  std::string failRenameTo;
  bool exists(const char* p) const { return files.count(p) || directories.count(p); }
  bool mkdir(const char* p) { directories.insert(p); return true; }
  bool openFileForRead(const char*, const std::string& p, HalFile& out) {
    const auto it = files.find(p);
    if (it == files.end()) return false;
    out.storage = this;
    out.path = p;
    out.content = it->second;
    out.writable = false;
    out.position = 0;
    out.opened = true;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& p, HalFile& out) {
    out.storage = this;
    out.path = p;
    out.content.clear();
    out.position = 0;
    out.writable = true;
    out.opened = true;
    return true;
  }
  HalFile open(const std::string& p, int flags) {
    HalFile file;
    if (!(flags & O_CREAT) && !files.count(p)) return file;
    file.storage = this;
    file.path = p;
    file.content = files[p];
    file.writable = (flags & O_RDWR) != 0;
    file.opened = true;
    return file;
  }
  bool remove(const char* p) { return files.erase(p) != 0; }
  bool rename(const char* oldPath, const char* newPath) {
    if (failRenameTo == newPath) return false;
    auto item = files.find(oldPath);
    if (item == files.end()) return false;
    files[newPath] = item->second;
    files.erase(item);
    return true;
  }
};

inline bool HalFile::close() {
  if (!opened) return false;
  if (writable && storage) storage->files[path] = content;
  opened = false;
  return true;
}
extern FakeHalStorage Storage;
