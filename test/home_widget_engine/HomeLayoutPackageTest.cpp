#include "components/home/HomeLayoutPackage.h"

#include <array>
#include <cassert>
#include <string>

using namespace home_grid::package;

void validThemePaths() {
  const std::array<const char*, 4> files = {
      "manifest.json", "themes/halo/theme.json", "themes/halo/home-layout.json",
      "themes/halo/assets/home.cpb"};
  assert(validLayoutPath("halo", "themes/halo/home-layout.json"));
  assert(declaredLayoutPath("halo", "themes/halo/home-layout.json", files));
  assert(!declaredLayoutPath("snoopy", "themes/snoopy/home-layout.json", files));
  assert(!declaredLayoutPath("halo", "themes/halo/theme.json", files));
}

void rejectUntrustedPaths() {
  const char* const files[] = {"themes/halo/home-layout.json", nullptr};
  constexpr const char* rejected[] = {
      "", "/themes/halo/home-layout.json", "../themes/halo/home-layout.json",
      "themes/halo/../home-layout.json", "themes/halo//home-layout.json",
      "themes/Halo/home-layout.json", "themes/snoopy/home-layout.json",
      "themes/halo/home-layout.json/extra", "themes/halo/home-layout.JSON",
      "themes/halo\\home-layout.json", "themes/halo/home-layout.json?x=1",
      "themes/halo/home-layout.json\n"};
  for (const char* path : rejected)
    assert(!declaredLayoutPath("halo", path, files));
  assert(!declaredLayoutPath("halo", nullptr, files));
  assert(!declaredLayoutPath(nullptr, "themes/halo/home-layout.json", files));
}

void boundaryIdsAndDeclarations() {
  const std::array<const char*, 2> files = {
      "themes/halo/home-layout.json", "themes/example/home-layout.json"};
  assert(validThemeId("halo"));
  assert(validThemeId("0"));
  assert(validThemeId("h-2"));
  assert(!validThemeId(""));
  assert(!validThemeId("-bad"));
  assert(!validThemeId("Halo"));
  assert(!validThemeId("ha_lo"));
  assert(!validThemeId("a/b"));
  assert(!validThemeId("bad.."));
  assert(!validThemeId(std::string(33, 'x').c_str()));
  assert(validLayoutPath("not-listed", "themes/not-listed/home-layout.json"));
  assert(!declaredLayoutPath("missing", "themes/missing/home-layout.json", files));
  assert(kMaxLayoutBytes == 8192);
}

int main() {
  validThemePaths();
  rejectUntrustedPaths();
  boundaryIdsAndDeclarations();
}
