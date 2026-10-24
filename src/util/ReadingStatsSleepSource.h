#pragma once

#include <string>

namespace readingstatssleep {
// Reserved virtual Custom Wallpaper Collection source, not a wallpaper-pack ID.
inline constexpr const char* kId = "reading-statistics";
inline constexpr const char* kLabel = "Reading Statistics";
bool selected(const char* id);
std::string manifestPath();
bool available();
}  // namespace readingstatssleep
