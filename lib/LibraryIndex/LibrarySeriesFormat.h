#pragma once

#include <cstdio>
#include <optional>
#include <string>

namespace library {

// Render index before series: "#4 - Saga of the Forgotten Warrior".
// When no index is provided, show just the series name (no placeholder).
inline std::string formatSeriesLabel(const std::string& series, const std::optional<float>& index) {
  if (series.empty()) return {};
  if (!index) return series;
  char number[24];
  std::snprintf(number, sizeof(number), "%.2f", static_cast<double>(*index));
  std::string indexText(number);
  while (!indexText.empty() && indexText.back() == '0') indexText.pop_back();
  if (!indexText.empty() && indexText.back() == '.') indexText.pop_back();
  return indexText.empty() ? series : "#" + indexText + " - " + series;
}

// Author and series occupy separate subtitle lines in the Library list.
inline std::string formatBookSubtitle(const std::string& author, const std::string& series) {
  if (author.empty()) return series;
  if (series.empty()) return author;
  return author + "\n" + series;
}

}  // namespace library
