#include <cassert>
#include <cstddef>

#include "../src/util/SingleCardPager.h"

int main() {
  singlecard::Pager p{};
  assert(!p.canCycle());
  assert(p.current() == 0);
  assert(p.move(1) == 0);
  p = {1, 0, true};
  assert(!p.canCycle());
  assert(p.move(-1) == 0);
  p = {3, 0, true};
  assert(p.canCycle());
  assert(p.move(-1) == 2);
  assert(p.move(1) == 0);
  assert(p.move(1) == 1);
  p.enabled = false;
  assert(p.move(-1) == 1);
  p = {2, 99, true};
  p.clamp();
  assert(p.current() == 1);
  assert(p.move(1) == 0);
  return 0;
}
