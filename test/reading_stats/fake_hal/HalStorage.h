#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>

constexpr int O_RDWR = 0x02;
constexpr int O_CREAT = 0x40;
constexpr int O_APPEND = 0x400;

struct FakeHalStorage;
struct HalFile {
  FakeHalStorage* storage = nullptr;
  std::string path;
  std::string content;
  uint64_t position = 0;
  size_t readLimit = SIZE_MAX;
  bool writable = false;
  bool opened = false;
  explicit operator bool() const { return opened; }
  size_t fileSize() const { return content.size(); }
  uint64_t fileSize64() const { return content.size(); }
  bool directory = false;
  std::string lastChild;
  bool isDirectory() const { return directory; }
  size_t getName(char* name, size_t cap) const {
    const auto base = path.substr(path.find_last_of('/') + 1);
    if (base.size() >= cap) return 0;
    memcpy(name, base.c_str(), base.size() + 1);
    return base.size();
  }
  HalFile openNextFile();
  bool seek64(uint64_t target) {
    if (!opened || target > content.size()) return false;
    position = target;
    return true;
  }
  int read(void* dest, size_t count) {
    if (!opened) return -1;
    const size_t end = std::min(content.size(), readLimit);
    if (position >= end) return 0;
    const size_t amount = std::min<size_t>(count, end - static_cast<size_t>(position));
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
  bool failRemoveDir = false;
  size_t readLimit = SIZE_MAX;
  bool exists(const char* p) const { return files.count(p) || directories.count(p); }
  bool mkdir(const char* p) {
    directories.insert(p);
    return true;
  }
  bool openFileForRead(const char*, const std::string& p, HalFile& out) {
    const auto it = files.find(p);
    if (it == files.end()) return false;
    out.storage = this;
    out.path = p;
    out.content = it->second;
    out.readLimit = readLimit;
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
  HalFile open(const std::string& p, int flags = 0) {
    HalFile file;
    if (!(flags & O_CREAT) && !exists(p.c_str())) return file;
    file.storage = this;
    file.path = p;
    file.directory = directories.count(p) != 0;
    if (!file.directory) file.content = files[p];
    file.writable = (flags & O_RDWR) != 0;
    if (flags & O_APPEND) file.position = file.content.size();
    file.opened = true;
    return file;
  }
  bool remove(const char* p) { return files.erase(p) != 0; }
  bool removeDir(const char* p) {
    if (failRemoveDir) return false;
    const std::string prefix = std::string(p) + "/";
    for (auto it = files.begin(); it != files.end();) {
      if (it->first.starts_with(prefix))
        it = files.erase(it);
      else
        ++it;
    }
    for (auto it = directories.begin(); it != directories.end();) {
      if (*it == p || it->starts_with(prefix))
        it = directories.erase(it);
      else
        ++it;
    }
    return true;
  }
  bool replaceFile(const char* tmp, const char* path) { return rename(tmp, path); }
  bool rename(const char* oldPath, const char* newPath) {
    if (failRenameTo == newPath) return false;
    auto item = files.find(oldPath);
    if (item == files.end()) return false;
    files[newPath] = item->second;
    files.erase(item);
    return true;
  }
};

inline HalFile HalFile::openNextFile() {
  if (!directory || !opened) return {};
  const std::string prefix = path + "/";
  std::string next;
  auto consider = [&](const std::string& name) {
    if (!name.starts_with(prefix)) return;
    const auto child = name.substr(0, name.find('/', prefix.size()));
    if (child > lastChild && (next.empty() || child < next)) next = child;
  };
  for (const auto& item : storage->files) consider(item.first);
  for (const auto& name : storage->directories) consider(name);
  if (next.empty()) return {};
  lastChild = next;
  return storage->open(next);
}

inline bool HalFile::close() {
  if (!opened) return false;
  if (writable && storage) storage->files[path] = content;
  opened = false;
  return true;
}
extern FakeHalStorage Storage;
