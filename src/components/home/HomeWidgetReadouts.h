#pragma once

// Small, native-only Home readout renderer. The owner supplies already
// localized/bounded text from RTC/statistics; SD themes never execute code
// or fetch statistics. All temporary state stays on the render stack.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "components/home/HomeWidgetEngine.h"

namespace home_widgets::firmware {

constexpr size_t kReadoutCapacity = 40;

// IDs are assigned by a trusted parser from its allowlisted metric field
// names; arbitrary user-provided resource handles cannot access providers.
enum class MetricField : uint16_t {
  ActiveDays = 1,
  Last7Seconds,
  Sessions,
  TotalSeconds,
  QualifiedTurns,
  SecondsPerTurn,
  LatestDaySeconds,
};

inline bool validMetric(uint16_t field) {
  return field >= static_cast<uint16_t>(MetricField::ActiveDays) &&
         field <= static_cast<uint16_t>(MetricField::LatestDaySeconds);
}

struct ReadoutSource {
  void* user = nullptr;
  bool (*clock)(void*, char* value, size_t valueCapacity) = nullptr;
  bool (*metric)(void*, uint16_t field, char* label, size_t labelCapacity,
                 char* value, size_t valueCapacity) = nullptr;
};

// Truncate at a UTF-8 codepoint boundary when measured text does not fit;
// no heap-backed std::string or full-width image buffers.
template <typename Renderer>
inline void drawFitted(const Renderer& renderer, int font, home_grid::Rect area,
                       int y, char* text, size_t capacity, bool inkBlack) {
  if (area.width <= 4 || !capacity) return;
  text[capacity - 1] = '\0';
  size_t len = std::strlen(text);
  while (len && renderer.getTextWidth(font, text) > area.width - 8) {
    --len;
    while (len && (static_cast<unsigned char>(text[len]) & 0xc0) == 0x80) --len;
    text[len] = '\0';
  }
  if (len) renderer.drawText(font, area.x + 4, y, text, inkBlack);
}

// The fonts and text engine come from the existing GUI renderer. Returns
// false only for unsupported fields or a physically unreadable widget slot.
template <typename Renderer>
inline bool paintReadout(const Renderer& renderer, Kind kind, uint16_t resource,
                         home_grid::Rect area, Panel panel, const ReadoutSource& data,
                         int font) {
  if (kind != Kind::Clock && kind != Kind::Metric) return false;
  if (kind == Kind::Metric && !validMetric(resource)) return false;
  const int lineHeight = renderer.getLineHeight(font);
  if (lineHeight <= 0 || area.width < 24 || area.height < lineHeight + 4) return false;

  char label[kReadoutCapacity]{};
  char value[kReadoutCapacity]{};
  if (kind == Kind::Clock) {
    std::memcpy(label, "CLOCK", sizeof("CLOCK"));
    if (!data.clock || !data.clock(data.user, value, sizeof(value))) {
      std::memcpy(value, "--:--", sizeof("--:--"));
    }
  } else if (!data.metric ||
             !data.metric(data.user, resource, label, sizeof(label), value, sizeof(value))) {
    std::memcpy(label, "NO DATA", sizeof("NO DATA"));
    std::memcpy(value, "--", sizeof("--"));
  }
  label[sizeof(label) - 1] = '\0';
  value[sizeof(value) - 1] = '\0';
  if (!value[0]) {
    std::memcpy(value, "--", sizeof("--"));
  }

  const bool black = panel != Panel::Black;
  const bool twoLines = area.height >= lineHeight * 2 + 10;
  if (twoLines) {
    const int firstY = area.y + (area.height - lineHeight * 2 - 2) / 2;
    drawFitted(renderer, font, area, firstY, label, sizeof(label), black);
    drawFitted(renderer, font, area, firstY + lineHeight + 2, value, sizeof(value), black);
  } else {
    drawFitted(renderer, font, area, area.y + (area.height - lineHeight) / 2,
               value, sizeof(value), black);
  }
  return true;
}

}  // namespace home_widgets::firmware
