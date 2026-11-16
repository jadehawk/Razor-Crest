#pragma once

// Bounded, allocation-free ID registry for declarative Home-only theme scenes.
// Handles 1 is reserved for the legacy Home region. Named scenes use 2..5.
// No SD file paths, scripts, touch targets or executable callbacks.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace home_scenes {
constexpr uint8_t kLegacyHome = 1;
constexpr size_t kMaxNamed = 4;
constexpr size_t kNameLength = 32;
constexpr uint8_t kFirstNamedHandle = kLegacyHome + 1;
constexpr size_t kMaxOpsPerScene = 16;
constexpr size_t kMaxTotalOps = 48;

inline bool withinOpBudget(size_t existing, size_t incoming, bool namedHome) {
  return existing <= kMaxTotalOps && incoming <= kMaxOpsPerScene &&
         incoming <= kMaxTotalOps - existing && (!namedHome || incoming > 0);
}

inline bool validName(const char* name) {
  if (!name || name[0] < 'a' || name[0] > 'z') return false;
  size_t i = 0;
  for (; name[i]; ++i) {
    if (i >= kNameLength) return false;
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.'))
      return false;
  }
  return i > 0;
}

struct Registry {
  std::array<std::array<char, kNameLength + 1>, kMaxNamed> names{};
  uint8_t count = 0;

  // Returns zero on invalid/duplicate name or capacity exhaustion.
  uint8_t add(const char* name) {
    if (!validName(name) || count >= kMaxNamed || find(name)) return 0;
    const size_t length = std::strlen(name);
    std::memcpy(names[count].data(), name, length + 1);
    return static_cast<uint8_t>(kFirstNamedHandle + count++);
  }

  uint8_t find(const char* name) const {
    if (!validName(name)) return 0;
    for (size_t i = 0; i < count; ++i)
      if (std::strcmp(names[i].data(), name) == 0)
        return static_cast<uint8_t>(kFirstNamedHandle + i);
    return 0;
  }

  int index(uint16_t handle) const {
    if (handle < kFirstNamedHandle ||
        handle >= static_cast<uint16_t>(kFirstNamedHandle + count))
      return -1;
    return static_cast<int>(handle - kFirstNamedHandle);
  }
};
static_assert(sizeof(Registry) <= 136, "Named scene registry is too large for X3");
}  // namespace home_scenes
