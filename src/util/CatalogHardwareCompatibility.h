#pragma once

// Catalog discovery advisory only. Never call this from an installer.
// This parser is allocation-free to keep X3/ESP32-C3 memory bounded.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cataloghardware {
inline bool inList(std::string_view values, std::string_view wanted) {
  while (!values.empty()) {
    const auto end = values.find('|');
    const auto token = values.substr(0, end);
    if (token == wanted) return true;
    if (end == std::string_view::npos) break;
    values.remove_prefix(end + 1);
  }
  return false;
}

inline bool matches(std::string_view advisory, std::string_view model, size_t installedPsramBytes) {
  while (!advisory.empty()) {
    const auto end = advisory.find(';');
    const auto clause = advisory.substr(0, end);
    if (clause.substr(0, 7) == "models=") {
      if (!inList(clause.substr(7), model)) return false;
    } else if (clause.substr(0, 8) == "exclude=") {
      if (inList(clause.substr(8), model)) return false;
    } else if (clause.substr(0, 9) == "psram_mb=") {
      const auto digits = clause.substr(9);
      if (digits.empty() || digits.size() > 2) return false;
      size_t minimum = 0;
      for (char c : digits) {
        if (c < '0' || c > '9') return false;
        minimum = minimum * 10 + static_cast<size_t>(c - '0');
      }
      if (installedPsramBytes / (1024u * 1024u) < minimum) return false;
    }
    // Unknown advisory clauses are ignored so newer manifests remain usable.
    if (end == std::string_view::npos) break;
    advisory.remove_prefix(end + 1);
  }
  return true;
}
}  // namespace cataloghardware
