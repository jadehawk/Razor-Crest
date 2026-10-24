#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Fixed-size monochrome icon assets for V1 dashboard widgets.
namespace dashboardcards {
constexpr int kIconSize = 80;
constexpr size_t kMaxIcons = 8;
constexpr size_t kIconBytes = (kIconSize * kIconSize) / 8;

struct Icon {
  std::array<uint8_t, kIconBytes> pixels{};
  uint8_t dimension = 0;
  bool at(int x, int y) const {
    if (x < 0 || x >= dimension || y < 0 || y >= dimension) return false;
    const int index = y * kIconSize + x;
    return (pixels[index / 8] & (0x80u >> (index % 8))) != 0;
  }
};

inline bool validIconName(std::string_view name) {
  if (name.empty() || name.size() > 32) return false;
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

inline bool decodePbm(std::string_view source, Icon& output) {
  constexpr std::string_view header = "P4\n80 80\n";
  if (source.size() != header.size() + kIconBytes || source.substr(0, header.size()) != header) return false;
  Icon decoded;
  std::memcpy(decoded.pixels.data(), source.data() + header.size(), kIconBytes);
  decoded.dimension = kIconSize;
  output = decoded;
  return true;
}

enum class Border : uint8_t { Square, Rounded, Chamfered };
inline bool parseBorder(std::string_view value, Border& result) {
  if (value.empty() || value == "square") result = Border::Square;
  else if (value == "rounded") result = Border::Rounded;
  else if (value == "chamfered") result = Border::Chamfered;
  else return false;
  return true;
}
}  // namespace dashboardcards
