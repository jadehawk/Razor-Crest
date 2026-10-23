#pragma once

#include <cstdint>

namespace catalogprogress {
// Keep existing behavior for books and regular plugins.
struct Policy {
  uint8_t percentStep;
  unsigned long maxRefreshIntervalMs;
};

constexpr Policy kDefault{5, 5000};
constexpr Policy kExtensionZip{20, 15000};

inline bool shouldRefresh(const int percent, const int lastShownPercent,
                          const unsigned long elapsedSinceRefreshMs, const Policy& policy) {
  return percent >= 100 || lastShownPercent < 0 ||
         percent >= lastShownPercent + policy.percentStep ||
         elapsedSinceRefreshMs >= policy.maxRefreshIntervalMs;
}
}  // namespace catalogprogress
