#include <cassert>
#include <optional>
#include <string>

#include "../lib/LibraryIndex/LibrarySeriesFormat.h"

int main() {
  using library::formatBookSubtitle;
  using library::formatSeriesLabel;

  const std::string series = "Saga of the Forgotten Warrior";
  assert(formatSeriesLabel(series, 4.0f) == "#4 - Saga of the Forgotten Warrior");
  assert(formatSeriesLabel(series, 2.5f) == "#2.5 - Saga of the Forgotten Warrior");
  assert(formatSeriesLabel(series, std::nullopt) == series);
  assert(formatSeriesLabel("", 4.0f).empty());

  const std::string author = "Larry Correia";
  assert(formatBookSubtitle(author, formatSeriesLabel(series, 4.0f)) ==
         "Larry Correia\n#4 - Saga of the Forgotten Warrior");
  assert(formatBookSubtitle(author, "") == author);
  assert(formatBookSubtitle("", formatSeriesLabel(series, 4.0f)) == "#4 - Saga of the Forgotten Warrior");
  assert(formatBookSubtitle("", "").empty());
  return 0;
}
