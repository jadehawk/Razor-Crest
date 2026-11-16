#include "components/themes/HomeSceneNames.h"

#include <cassert>
#include <cstring>

using namespace home_scenes;

void testNamesAndHandles() {
  Registry names;
  assert(names.count == 0);
  assert(names.find("halo.hero") == 0);
  assert(names.index(kLegacyHome) == -1);
  assert(names.add("halo.hero") == 2);
  assert(names.add("snoopy.footer") == 3);
  assert(names.add("m2_b") == 4);
  assert(names.add("widget-background") == 5);
  assert(names.add("fifth") == 0);
  assert(names.count == 4);
  assert(names.find("halo.hero") == 2);
  assert(names.find("snoopy.footer") == 3);
  assert(names.index(2) == 0 && names.index(5) == 3);
  assert(names.index(6) == -1 && names.index(255) == -1);
  assert(names.add("halo.hero") == 0);
  assert(names.find("unknown") == 0);
}

void testNameValidation() {
  assert(validName("a"));
  assert(validName("home1"));
  assert(validName("halo.hero"));
  assert(!validName(nullptr));
  assert(!validName(""));
  assert(!validName("A"));
  assert(!validName("1scene"));
  assert(!validName("../other"));
  assert(!validName("home/screen"));
  assert(!validName("home\\scene"));
  assert(!validName("home:scene"));
  assert(!validName("home scene"));
  assert(!validName("home\nscene"));
  assert(!validName("snoopy_\xc3\xa9"));
  char exact[kNameLength + 1];
  exact[0] = 'a';
  for (size_t i = 1; i < kNameLength; ++i) exact[i] = 'x';
  exact[kNameLength] = '\0';
  assert(validName(exact));
  char tooLong[kNameLength + 2];
  std::memcpy(tooLong, exact, sizeof(exact));
  tooLong[kNameLength] = 'x';
  tooLong[kNameLength + 1] = '\0';
  assert(!validName(tooLong));
}

void testSharedOperationBudget() {
  assert(withinOpBudget(0, 0, false));  // Legacy empty regions unchanged.
  assert(!withinOpBudget(0, 0, true));  // Named scenes must contain artwork.
  assert(withinOpBudget(0, 16, true));
  assert(withinOpBudget(32, 16, true));
  assert(!withinOpBudget(33, 16, true));
  assert(!withinOpBudget(48, 1, true));
  assert(!withinOpBudget(0, 17, true));
  assert(!withinOpBudget(49, 0, false));
}

void testIndependentRegistriesAndReset() {
  Registry a, b;
  assert(a.add("first") == 2);
  assert(a.add("second") == 3);
  assert(b.add("other") == 2);
  assert(a.find("other") == 0);
  assert(b.find("first") == 0);
  a = Registry{};
  assert(a.count == 0 && a.find("first") == 0);
  assert(a.add("new") == 2);
}

int main() {
  testNamesAndHandles();
  testNameValidation();
  testSharedOperationBudget();
  testIndependentRegistriesAndReset();
}
