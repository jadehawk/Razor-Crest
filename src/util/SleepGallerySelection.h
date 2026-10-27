#pragma once

#include <cstdint>

// Pure gallery picker shared by base and alternate theme sleep art.
// Never repeats the previous image within the same firmware session.
namespace sleep_gallery {
inline int chooseIndex(int previous, int count, uint32_t random) {
  if (count <= 1) return 0;
  if (previous < 0 || previous >= count) return static_cast<int>(random % count);
  return (previous + 1 + static_cast<int>(random % (count - 1))) % count;
}
}  // namespace sleep_gallery
