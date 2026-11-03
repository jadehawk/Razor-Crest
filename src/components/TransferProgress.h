#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// Shared transfer-only progress data and formatting for both legacy themed
// screens and FreeInkUI catalog screens. Do not use for reading progress.
namespace transferprogress {

constexpr int kBarHeight = 16;

inline int percent(const size_t current, const size_t total) {
  if (total == 0) return 0;
  return static_cast<int>(std::min<uint64_t>(100, static_cast<uint64_t>(current) * 100 / total));
}

// Decimal KB/MB/GB, matching the values users see in download sizes. Use
// integer math to avoid pulling floating-point formatting into MCU UI code.
inline std::string formatBytes(const size_t bytes) {
  char result[28];
  const uint64_t value = static_cast<uint64_t>(bytes);
  const char* unit = "B";
  uint64_t divisor = 1;
  if (value >= 1000000000ULL) {
    divisor = 1000000000ULL;
    unit = "GB";
  } else if (value >= 1000000ULL) {
    divisor = 1000000ULL;
    unit = "MB";
  } else if (value >= 1000ULL) {
    divisor = 1000ULL;
    unit = "KB";
  }
  if (divisor == 1) {
    snprintf(result, sizeof(result), "%llu B", static_cast<unsigned long long>(value));
  } else {
    auto whole = static_cast<unsigned long long>(value / divisor);
    auto tenths = static_cast<unsigned>(((value % divisor) * 10 + divisor / 2) / divisor);
    if (tenths == 10) {
      ++whole;
      tenths = 0;
    }
    if (whole < 10) {
      snprintf(result, sizeof(result), "%llu.%u %s", whole, tenths, unit);
    } else {
      snprintf(result, sizeof(result), "%llu %s", whole, unit);
    }
  }
  return result;
}

inline std::string formatTransfer(const size_t current, const size_t total) {
  if (total == 0) return formatBytes(current);
  return formatBytes(current) + " / " + formatBytes(total);
}

inline std::string formatPercent(const size_t current, const size_t total) {
  return std::to_string(percent(current, total)) + "%";
}

}  // namespace transferprogress
