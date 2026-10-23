#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// Shared metadata requirements of *new catalog ZIP installs* for SD themes and
// wallpapers. The installed-pack registries may continue to read legacy packs.
namespace extensionrequiredfiles {
inline bool contains(const std::vector<std::string>& files, const char* name) {
  return std::find(files.begin(), files.end(), name) != files.end();
}

inline bool hasRequiredMetadata(const std::vector<std::string>& files) {
  return contains(files, "manifest.json") && contains(files, "README.md") &&
         contains(files, "DETAILS.md");
}

inline bool wallpaperCountMatches(const std::vector<std::string>& files, size_t imageCount) {
  return hasRequiredMetadata(files) && imageCount > 0 && imageCount <= 32 &&
         files.size() == imageCount + 3;
}
}  // namespace extensionrequiredfiles
