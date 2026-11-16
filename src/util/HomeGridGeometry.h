#pragma once

// Home Layout V1: bounded, allocation-free geometry only.
// Does not parse SD files, paint widgets, own artwork or change HomeActivity.
// Caller owns Definition and Result storage; X3 render stack must stay small.
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace home_grid {
constexpr int kMaxRows = 8;
constexpr int kMaxColumnsPerRow = 4;
constexpr int kMaxWidgets = 16;
constexpr int kMaxScreenSize = 2048;

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
};

enum class RowMode : uint8_t { Weighted, Fixed, Content };
enum class Status : uint8_t { Ok, InvalidCanvas, InvalidLayout, InvalidTracks, NoSpace };

struct Screen {
  int width = 0;
  int height = 0;
  // Firmware-owned protected areas, including physical button hints.
  // The status indicators are drawn inside the first (native.header) row.
  int left = 0;
  int right = 0;
  int top = 0;
  int bottom = 0;
};

struct Column {
  uint16_t weight = 1;
  uint16_t minWidth = 1;
  // Zero means no explicit upper bound inside the row.
  uint16_t maxWidth = 0;
};

struct Row {
  uint16_t weight = 1;
  uint16_t minHeight = 1;
  uint16_t maxHeight = 0;
  // Used by Content rows only, measured by the native widget once.
  uint16_t preferredHeight = 0;
  uint8_t firstColumn = 0;
  uint8_t columnCount = 1;
  uint8_t columnGap = 0;
  RowMode mode = RowMode::Weighted;
};

struct Definition {
  Row rows[kMaxRows]{};
  Column columns[kMaxWidgets]{};
  uint8_t rowCount = 0;
  uint8_t columnCount = 0;
  uint8_t rowGap = 0;
};

struct Result {
  Rect canvas{};
  Rect rows[kMaxRows]{};
  Rect widgets[kMaxWidgets]{};
  uint8_t rowCount = 0;
  uint8_t widgetCount = 0;
};

static_assert(sizeof(Definition) <= 512, "Layout descriptor exceeds bounded V1 storage");
static_assert(sizeof(Result) <= 256, "Resolved Home rectangles exceed bounded V1 storage");

namespace detail {
struct Track {
  int16_t allocated = 0;
  int16_t maximum = 0;
  uint8_t weight = 1;
};
static_assert(sizeof(Track) <= 6, "Keep the resolver's stack scratch compact");

// All data is provided in bounded fixed arrays. Any residual when ALL tracks
// hit their maxima remains unused, rather than stretching buttons/artwork.
// Largest-remainder ties are resolved in document order.
inline void distribute(Track* tracks, int count, int pixels) {
  while (pixels > 0) {
    int totalWeight = 0;
    for (int i = 0; i < count; ++i)
      if (tracks[i].allocated < tracks[i].maximum) totalWeight += tracks[i].weight;
    if (!totalWeight) break;

    const int available = pixels;
    int granted = 0;
    for (int i = 0; i < count; ++i) {
      Track& track = tracks[i];
      if (track.allocated >= track.maximum) continue;
      const int fairShare = available * track.weight / totalWeight;
      const int amount = std::min(fairShare, static_cast<int>(track.maximum - track.allocated));
      track.allocated = static_cast<int16_t>(track.allocated + amount);
      granted += amount;
    }
    pixels -= granted;

    // Award fractional pixels in true largest-remainder order, at most once
    // per track per round. Avoids biasing all leftover pixels to one column.
    uint8_t awarded = 0;
    for (int award = 0; award < count && pixels > 0; ++award) {
      int best = -1;
      int bestRemainder = 0;
      for (int i = 0; i < count; ++i) {
        if ((awarded & (1u << i)) || tracks[i].allocated >= tracks[i].maximum ||
            tracks[i].weight == 0)
          continue;
        const int fraction = available * tracks[i].weight % totalWeight;
        if (fraction > bestRemainder) {
          best = i;
          bestRemainder = fraction;
        }
      }
      if (best < 0) break;
      ++tracks[best].allocated;
      awarded |= static_cast<uint8_t>(1u << best);
      --pixels;
    }
    if (granted == 0 && awarded == 0) break; // defensive: malformed input

  }
}
}  // namespace detail

// Geometry is valid ONLY when Status::Ok is returned. No dynamic memory,
// recursive calls, framebuffer copies, device libraries, or plugin execution.
inline Status resolve(const Definition& def, const Screen& screen, Result& out) {
  out = Result{};
  if (screen.width <= 0 || screen.height <= 0 || screen.width > kMaxScreenSize ||
      screen.height > kMaxScreenSize || screen.left < 0 || screen.right < 0 ||
      screen.top < 0 || screen.bottom < 0 || screen.left + screen.right >= screen.width ||
      screen.top + screen.bottom >= screen.height)
    return Status::InvalidCanvas;
  if (def.rowCount == 0 || def.rowCount > kMaxRows ||
      def.columnCount == 0 || def.columnCount > kMaxWidgets || def.rowGap > 32)
    return Status::InvalidLayout;

  const int canvasW = screen.width - screen.left - screen.right;
  const int canvasH = screen.height - screen.top - screen.bottom;
  int minTotal = (def.rowCount - 1) * def.rowGap;
  int expectedColumn = 0;

  // Validate all tracks BEFORE publishing any rectangles.
  for (int r = 0; r < def.rowCount; ++r) {
    const Row& row = def.rows[r];
    if (row.weight < 1 || row.weight > 10 || row.minHeight == 0 ||
        row.minHeight > canvasH || (row.maxHeight && row.maxHeight < row.minHeight) ||
        row.maxHeight > kMaxScreenSize || row.columnCount == 0 ||
        row.columnCount > kMaxColumnsPerRow || row.columnGap > 32 ||
        row.firstColumn != expectedColumn ||
        expectedColumn + row.columnCount > def.columnCount)
      return Status::InvalidTracks;
    if (row.mode == RowMode::Fixed && row.maxHeight != row.minHeight)
      return Status::InvalidTracks;
    if (row.mode == RowMode::Content && (!row.maxHeight || !row.preferredHeight))
      return Status::InvalidTracks;
    if (row.mode != RowMode::Weighted && row.mode != RowMode::Fixed &&
        row.mode != RowMode::Content)
      return Status::InvalidTracks;
    minTotal += row.minHeight;

    int minWidth = (row.columnCount - 1) * row.columnGap;
    for (int c = 0; c < row.columnCount; ++c) {
      const Column& column = def.columns[expectedColumn + c];
      if (column.weight < 1 || column.weight > 10 || column.minWidth == 0 ||
          column.minWidth > canvasW || (column.maxWidth && column.maxWidth < column.minWidth) ||
          column.maxWidth > kMaxScreenSize)
        return Status::InvalidTracks;
      minWidth += column.minWidth;
    }
    if (minWidth > canvasW) return Status::NoSpace;
    expectedColumn += row.columnCount;
  }
  if (expectedColumn != def.columnCount) return Status::InvalidLayout;
  if (minTotal > canvasH) return Status::NoSpace;

  detail::Track tracks[kMaxRows]{};
  for (int r = 0; r < def.rowCount; ++r) {
    const Row& row = def.rows[r];
    tracks[r].allocated = static_cast<int16_t>(row.minHeight);
    tracks[r].maximum = static_cast<int16_t>(row.maxHeight ? row.maxHeight : canvasH);
    tracks[r].weight = row.mode == RowMode::Weighted ? static_cast<uint8_t>(row.weight) : 0;
  }

  int remaining = canvasH - minTotal;
  // Give a natural-sized list first claim to room, but never steal weighted
  // minima. When room is tight, it remains scrollable at its minimum height.
  for (int r = 0; r < def.rowCount && remaining > 0; ++r) {
    const Row& row = def.rows[r];
    if (row.mode != RowMode::Content) continue;
    const int wanted = std::min(static_cast<int>(row.preferredHeight),
                                static_cast<int>(tracks[r].maximum));
    const int increase = std::min(remaining, std::max(0, wanted - tracks[r].allocated));
    tracks[r].allocated = static_cast<int16_t>(tracks[r].allocated + increase);
    remaining -= increase;
  }

  detail::distribute(tracks, def.rowCount, remaining);
  out.canvas = Rect{static_cast<int16_t>(screen.left), static_cast<int16_t>(screen.top),
                    static_cast<int16_t>(canvasW), static_cast<int16_t>(canvasH)};
  out.rowCount = def.rowCount;
  out.widgetCount = def.columnCount;

  // Save every resolved row height before reusing the small scratch array
  // for column allocation. Otherwise earlier columns overwrite later rows.
  int y = screen.top;
  for (int r = 0; r < def.rowCount; ++r) {
    const int height = tracks[r].allocated;
    out.rows[r] = Rect{static_cast<int16_t>(screen.left), static_cast<int16_t>(y),
                       static_cast<int16_t>(canvasW), static_cast<int16_t>(height)};
    y += height + (r + 1 < def.rowCount ? def.rowGap : 0);
  }

  for (int r = 0; r < def.rowCount; ++r) {
    const Row& row = def.rows[r];
    const Rect& rowRect = out.rows[r];
    int minWidth = (row.columnCount - 1) * row.columnGap;
    for (int c = 0; c < row.columnCount; ++c) {
      const Column& column = def.columns[row.firstColumn + c];
      tracks[c].allocated = static_cast<int16_t>(column.minWidth);
      tracks[c].maximum = static_cast<int16_t>(column.maxWidth ? column.maxWidth : canvasW);
      tracks[c].weight = static_cast<uint8_t>(column.weight);
      minWidth += column.minWidth;
    }
    detail::distribute(tracks, row.columnCount, canvasW - minWidth);
    int x = screen.left;
    for (int c = 0; c < row.columnCount; ++c) {
      const int index = row.firstColumn + c;
      const int width = tracks[c].allocated;
      out.widgets[index] = Rect{static_cast<int16_t>(x), rowRect.y,
                                 static_cast<int16_t>(width), rowRect.height};
      x += width + row.columnGap;
    }
  }
  return Status::Ok;
}

// The caller/parser authorizes the scene underlay's row IDs. Geometry merely
// returns the bounded union of adjacent full-width rows; no drawable ownership.
inline bool spanRows(const Result& result, int first, int last, Rect& unionRect) {
  unionRect = Rect{};
  if (first <= 0 || last < first || last >= result.rowCount - 1) return false;
  const Rect& a = result.rows[first];
  const Rect& b = result.rows[last];
  if (a.x != b.x || a.width != b.width || b.y < a.y) return false;
  unionRect = Rect{a.x, a.y, a.width,
                   static_cast<int16_t>(b.y + b.height - a.y)};
  return true;
}
}  // namespace home_grid
