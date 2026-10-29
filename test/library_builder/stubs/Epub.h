#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "HalStorage.h"

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  bool success = true;
  std::string series;
  std::optional<float> seriesIndex;

  FakeMetadata() = default;
  FakeMetadata(std::string t, std::string a, bool ok) : title(std::move(t)), author(std::move(a)), success(ok) {}
};

inline std::map<std::string, FakeMetadata> bookMetadata;

class Epub {
  std::string path;

 public:
  struct SyncMetadata {
    std::string isbn;
    std::string asin;
    std::string series;
    std::optional<float> seriesIndex;
  };

  Epub(const std::string& path, const char*) : path(path) {}

  bool loadMetadata(std::string& title, std::string& author) {
    ++fake::parses;
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    return true;
  }

  bool loadSyncMetadata(SyncMetadata& result) {
    const auto& metadata = bookMetadata[path];
    if (!metadata.success) return false;
    result.series = metadata.series;
    result.seriesIndex = metadata.seriesIndex;
    return true;
  }
};
