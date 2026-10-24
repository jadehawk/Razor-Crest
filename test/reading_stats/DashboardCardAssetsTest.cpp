#include <cassert>
#include <string>

#include "../../src/activities/plugins/DashboardCardAssets.h"

int main() {
  using namespace dashboardcards;
  assert(validIconName("reading_time"));
  assert(validIconName("book-80"));
  assert(!validIconName(""));
  assert(!validIconName("../secret"));
  assert(!validIconName("sub/path"));
  assert(!validIconName("Capitalized"));
  assert(!validIconName(std::string(33, 'a')));
  assert(kIconSize == 80);
  assert(kIconBytes == 800);
  assert(kMaxIcons == 8);

  Icon icon;
  std::string binary = "P4\n80 80\n" + std::string(kIconBytes, '\0');
  binary[9] = static_cast<char>(0x80);
  binary[9 + kIconBytes - 1] = static_cast<char>(0x01);
  assert(decodePbm(binary, icon));
  assert(icon.dimension == 80);
  assert(icon.at(0, 0));
  assert(icon.at(79, 79));
  assert(!icon.at(1, 0));
  assert(!icon.at(79, 0));
  assert(!icon.at(80, 0));
  assert(!decodePbm(binary.substr(0, binary.size() - 1), icon));
  assert(!decodePbm(binary + "x", icon));
  assert(!decodePbm("P1\n24 24\n", icon));

  Border border{};
  assert(parseBorder("square", border) && border == Border::Square);
  assert(parseBorder("rounded", border) && border == Border::Rounded);
  assert(parseBorder("chamfered", border) && border == Border::Chamfered);
  assert(!parseBorder("circle", border));
}
