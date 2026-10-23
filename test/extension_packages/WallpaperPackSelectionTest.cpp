#include <string>
#include <vector>

#include "../../src/components/wallpapers/WallpaperPackSelection.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

struct Pack {
  std::string id;
};

int main() {
  using wallpaperselection::isUnavailable;
  std::vector<Pack> packs{{"starwars-x3"}, {"other"}};
  CHECK(!isUnavailable("", packs));             // All Wallpapers is always valid.
  CHECK(!isUnavailable(nullptr, packs));
  CHECK(!isUnavailable("starwars-x3", packs));   // Installed collection remains selected.
  CHECK(isUnavailable("missing-pack", packs));  // Deleted/corrupt pack => All.
  packs.erase(packs.begin());
  CHECK(isUnavailable("starwars-x3", packs));   // Removing selected wallpaper must clear ID.
  CHECK(!isUnavailable("other", packs));         // Unrelated selection unaffected.
  packs.clear();
  CHECK(isUnavailable("other", packs));         // SD package removed externally.
  CHECK(!isUnavailable("", packs));             // Shared pool still available.
  return 0;
}
