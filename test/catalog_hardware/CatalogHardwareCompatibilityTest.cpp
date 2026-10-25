#include <cassert>

#include "util/CatalogHardwareCompatibility.h"

int main() {
  using cataloghardware::matches;
  constexpr size_t MB = 1024u * 1024u;
  assert(matches("", "x3", 0));
  assert(matches("psram_mb=0", "x3", 0));
  assert(!matches("psram_mb=2", "x3", 0));
  assert(matches("psram_mb=2", "x4pro", 8 * MB));
  assert(!matches("psram_mb=4", "x4pro", 2 * MB));
  assert(matches("models=x4pro|sticky;psram_mb=2", "sticky", 8 * MB));
  assert(!matches("models=x4pro|sticky", "x3", 0));
  assert(!matches("exclude=x3|x4", "x3", 0));
  assert(matches("exclude=x3|x4", "x4pro", 0));
  assert(!matches("models=x3|x4;exclude=x3", "x3", 0));
  assert(matches("models=x3|x4;exclude=x3", "x4", 0));
  assert(!matches("psram_mb=bad", "x3", 0));
  assert(matches("future_rule=1", "x3", 0));
}
