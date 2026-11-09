#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Matches the WebUI's two FNV-1a passes over JavaScript UTF-16 code units.
// Strict UTF-8 validation: never generate a potentially wrong deletion ID for
// malformed source text. Empty result means the record must remain read-only.
namespace clippingfingerprint {
inline bool hasUnsupportedTrimBoundary(const std::string& input) {
  // JS String.trim() strips these Unicode spacing code points. Rather than
  // silently hash different text, leave such records browser-editable only.
  static const char* const spaces[] = {
      "\xc2\xa0",     "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84",
      "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
      "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80", "\xef\xbb\xbf",
  };
  for (const char* space : spaces) {
    const size_t count = std::strlen(space);
    if (input.size() >= count &&
        (input.compare(0, count, space, count) == 0 || input.compare(input.size() - count, count, space, count) == 0))
      return true;
  }
  return false;
}

inline std::string fromUtf8(const std::string& input) {
  uint32_t a = 2166136261u;
  uint32_t b = 2166136261u ^ 0x9e3779b9u;
  const auto feed = [&](const uint16_t unit) {
    a = (a ^ unit) * 16777619u;
    b = (b ^ unit) * 16777619u;
  };
  for (size_t i = 0; i < input.size();) {
    const uint8_t first = static_cast<uint8_t>(input[i++]);
    uint32_t scalar = first;
    unsigned continuation = 0;
    if (first < 0x80) {
      // ASCII code unit, unchanged.
    } else if (first >= 0xc2 && first <= 0xdf) {
      scalar = first & 0x1f;
      continuation = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      scalar = first & 0x0f;
      continuation = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      scalar = first & 0x07;
      continuation = 3;
    } else {
      return {};
    }
    if (continuation > input.size() - i) return {};
    for (unsigned k = 0; k < continuation; ++k) {
      const uint8_t byte = static_cast<uint8_t>(input[i++]);
      if ((byte & 0xc0) != 0x80) return {};
      scalar = (scalar << 6) | (byte & 0x3f);
    }
    if ((continuation == 1 && scalar < 0x80) || (continuation == 2 && scalar < 0x800) ||
        (continuation == 3 && scalar < 0x10000) || (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff)
      return {};
    if (scalar > 0xffff) {
      scalar -= 0x10000;
      feed(static_cast<uint16_t>(0xd800 + (scalar >> 10)));
      feed(static_cast<uint16_t>(0xdc00 + (scalar & 0x3ff)));
    } else {
      feed(static_cast<uint16_t>(scalar));
    }
  }
  char hex[17];
  snprintf(hex, sizeof(hex), "%08x%08x", static_cast<unsigned int>(a), static_cast<unsigned int>(b));
  return hex;
}
}  // namespace clippingfingerprint
