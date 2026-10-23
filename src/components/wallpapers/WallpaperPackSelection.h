#pragma once

// Wallpaper collections do not own live UI resources like installed SD
// themes. The saved ID is only a preference: when that collection is no
// longer present, Custom Sleep Screen falls back to the shared sleep pool.
namespace wallpaperselection {
template <typename Choices>
bool isUnavailable(const char* selectedId, const Choices& installed) {
  if (!selectedId || !*selectedId) return false;  // Empty = All Wallpapers.
  for (const auto& item : installed) {
    if (item.id == selectedId) return false;
  }
  return true;
}
}  // namespace wallpaperselection
