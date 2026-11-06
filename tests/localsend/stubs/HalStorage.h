#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
constexpr int O_RDONLY = 0, O_WRITE = 1, O_CREAT = 2, O_EXCL = 4, O_RDWR = 8;
inline std::map<std::string, std::vector<uint8_t>> testFiles;
inline bool testShortWrite = false, testCloseFails = false, testRenameFails = false;
inline std::string testRenameFailSource;
inline std::string testRemoveFailPath;
inline bool testNestedStagingDirEnsured = false, testBadStagingPath = false;
inline size_t testWriteCalls = 0;
inline uint64_t testCardTotalBytes = 0, testCardUsedBytes = 0;
inline size_t testSpaceCheckCalls = 0;
class HalFile {
  std::string path;
  bool open = false;
  uint64_t cursor = 0;

 public:
  HalFile() = default;
  explicit HalFile(std::string p) : path(std::move(p)), open(true) {}
  explicit operator bool() const { return open; }
  size_t write(const void* bytes, size_t n) {
    ++testWriteCalls;
    if (testShortWrite && n) --n;
    auto& data = testFiles[path];
    if (cursor > data.size()) return 0;
    if (cursor + n > data.size()) data.resize(static_cast<size_t>(cursor + n));
    std::memcpy(data.data() + cursor, bytes, n);
    cursor += n;
    return n;
  }
  size_t write(const uint8_t* bytes, size_t n) { return write(static_cast<const void*>(bytes), n); }
  int read(void* bytes, size_t n) {
    if (!open) return -1;
    const auto& data = testFiles[path];
    const size_t take = std::min<size_t>(n, data.size() - static_cast<size_t>(cursor));
    if (take) std::memcpy(bytes, data.data() + cursor, take);
    cursor += take;
    return static_cast<int>(take);
  }
  uint64_t fileSize64() const { return testFiles[path].size(); }
  bool seek64(uint64_t pos) {
    if (!open || pos > testFiles[path].size()) return false;
    cursor = pos;
    return true;
  }
  bool truncate(uint64_t length) {
    if (!open || length > testFiles[path].size()) return false;
    testFiles[path].resize(static_cast<size_t>(length));
    cursor = std::min(cursor, length);
    return true;
  }
  void flush() {}
  bool close() {
    open = false;
    return !testCloseFails;
  }
};
struct TestStorage {
  uint64_t sdTotalBytes() const { return testCardTotalBytes; }
  uint64_t sdUsedBytes() const {
    ++testSpaceCheckCalls;
    return testCardUsedBytes;
  }
  bool ensureDirectoryExists(const char* path) {
    if (!std::strcmp(path, "/.crosspoint/localsend")) testNestedStagingDirEnsured = true;
    return true;
  }
  bool exists(const char* p) { return testFiles.count(p); }
  HalFile open(const char* p, int flags = O_RDONLY) {
    const std::string path(p);
    if (path.ends_with(".part") && !path.starts_with("/.crosspoint/localsend/")) testBadStagingPath = true;
    if (exists(p) && (flags & O_EXCL)) return {};
    if (!exists(p)) {
      if (!(flags & O_CREAT)) return {};
      testFiles[p] = {};
    }
    return HalFile(p);
  }
  bool remove(const char* p) { return p != testRemoveFailPath && testFiles.erase(p) > 0; }
  bool rename(const char* a, const char* b) {
    if (testRenameFails || a == testRenameFailSource || exists(b) || !exists(a)) return false;
    testFiles[b] = std::move(testFiles[a]);
    testFiles.erase(a);
    return true;
  }
  bool replaceFile(const char* a, const char* b) {
    if (testRenameFails || !exists(a)) return false;
    testFiles[b] = std::move(testFiles[a]);
    testFiles.erase(a);
    return true;
  }
};
inline TestStorage Storage;
