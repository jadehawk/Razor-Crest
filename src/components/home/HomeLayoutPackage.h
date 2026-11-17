#pragma once

// Home layout packaging: validate the selected theme's manifest-relative path
// before performing *any* SD read. This does not enable new capabilities.
#include <cstddef>
#include <cstring>

namespace home_grid::package {

constexpr size_t kMaxThemeId = 32;
constexpr size_t kMaxLayoutBytes = 8192;
constexpr size_t kMaxLayoutPath = sizeof("themes/") - 1 + kMaxThemeId +
                                  sizeof("/home-layout.json");

inline bool validThemeId(const char* id) {
  if (!id || !*id) return false;
  size_t length = 0;
  for (const char* ptr = id; *ptr; ++ptr) {
    if (++length > kMaxThemeId) return false;
    const char c = *ptr;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (c == '-' && ptr != id))) return false;
  }
  return true;
}

inline bool validLayoutPath(const char* themeId, const char* relativePath) {
  if (!validThemeId(themeId) || !relativePath) return false;
  constexpr char prefix[] = "themes/";
  constexpr char suffix[] = "/home-layout.json";
  const size_t idLength = std::strlen(themeId);
  const size_t expectedLength = sizeof(prefix) - 1 + idLength +
                                sizeof(suffix) - 1;
  if (std::strlen(relativePath) != expectedLength) return false;
  const char* cursor = relativePath;
  if (std::memcmp(cursor, prefix, sizeof(prefix) - 1) != 0) return false;
  cursor += sizeof(prefix) - 1;
  if (std::memcmp(cursor, themeId, idLength) != 0) return false;
  cursor += idLength;
  return std::strcmp(cursor, suffix) == 0;
}

template <typename List>
inline bool declaredLayoutPath(const char* themeId, const char* relativePath,
                               const List& manifestFiles) {
  if (!validLayoutPath(themeId, relativePath)) return false;
  for (const auto& path : manifestFiles)
    if (path && std::strcmp(path, relativePath) == 0) return true;
  return false;
}
} // namespace home_grid::package
