#include <cassert>

#include "util/ScreenDecorationLayout.h"

int main() {
  using namespace screendecor;
  // X3: sparse Settings menu leaves a safe decorative area.
  const auto root = forList(792, 40, 76, 58, 6, 72, 8);
  assert(root.valid());
  assert(root.top >= 76 + 58 + 6 * 80);
  assert(root.bottom <= 792 - 40);
  // A long scrolling settings page must never be decorated over text.
  assert(!forList(792, 40, 76, 58, 10, 72, 8).valid());
  // A plugin browser on an empty/status page may have centered messages.
  assert(!forList(792, 40, 76, 58, 0, 72, 8).valid());
  // Tiny screens with no spare space must fail closed.
  assert(!forList(240, 40, 76, 40, 6, 72, 8).valid());
  // A shorter Wi-Fi transfer summary may have an unused lower band.
  assert(afterContent(792, 40, 400).valid());
  // QR codes extending down the screen must be kept clear.
  assert(!afterContent(792, 40, 700).valid());
  // Negative input never produces a valid phantom band.
  assert(!forList(792, 40, 76, 58, -1, 72, 8).valid());
  return 0;
}
