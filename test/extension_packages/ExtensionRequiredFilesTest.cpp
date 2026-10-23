#include <cstddef>
#include <string>
#include <vector>

#include "../../src/util/ExtensionRequiredFiles.h"

#define CHECK(test) do { if (!(test)) return __LINE__; } while (false)

int main() {
  using namespace extensionrequiredfiles;
  std::vector<std::string> theme = {"manifest.json", "README.md", "DETAILS.md", "themes/halo/theme.json"};
  CHECK(hasRequiredMetadata(theme));
  for (size_t i = 0; i < 3; ++i) {
    const auto missing = theme[i];
    theme.erase(theme.begin() + static_cast<std::ptrdiff_t>(i));
    CHECK(!hasRequiredMetadata(theme));
    theme.insert(theme.begin() + static_cast<std::ptrdiff_t>(i), missing);
  }

  std::vector<std::string> wallpaper = {"manifest.json", "README.md", "DETAILS.md",
                                        "wallpapers/one.bmp", "wallpapers/two.bmp"};
  CHECK(wallpaperCountMatches(wallpaper, 2));
  CHECK(!wallpaperCountMatches(wallpaper, 1));
  CHECK(!wallpaperCountMatches(wallpaper, 3));
  CHECK(!wallpaperCountMatches(wallpaper, 0));
  CHECK(!wallpaperCountMatches(wallpaper, 33));
  for (size_t i = 0; i < 3; ++i) {
    const auto missing = wallpaper[i];
    wallpaper.erase(wallpaper.begin() + static_cast<std::ptrdiff_t>(i));
    CHECK(!wallpaperCountMatches(wallpaper, 2));
    wallpaper.insert(wallpaper.begin() + static_cast<std::ptrdiff_t>(i), missing);
  }
  wallpaper.push_back("wallpapers/extra.bmp");
  CHECK(!wallpaperCountMatches(wallpaper, 2));
  return 0;
}
