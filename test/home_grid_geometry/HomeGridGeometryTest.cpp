#include "util/HomeGridGeometry.h"

#include <cassert>
#include <cstdint>

using namespace home_grid;

namespace {
Column column(int weight = 1, int minimum = 1, int maximum = 0) {
  Column c;
  c.weight = static_cast<uint16_t>(weight);
  c.minWidth = static_cast<uint16_t>(minimum);
  c.maxWidth = static_cast<uint16_t>(maximum);
  return c;
}
Row row(int first, int count, int weight, int minimum, int maximum = 0,
        RowMode mode = RowMode::Weighted, int preferred = 0, int gap = 0) {
  Row r;
  r.firstColumn = static_cast<uint8_t>(first);
  r.columnCount = static_cast<uint8_t>(count);
  r.weight = static_cast<uint16_t>(weight);
  r.minHeight = static_cast<uint16_t>(minimum);
  r.maxHeight = static_cast<uint16_t>(maximum);
  r.mode = mode;
  r.preferredHeight = static_cast<uint16_t>(preferred);
  r.columnGap = static_cast<uint8_t>(gap);
  return r;
}
Definition halo() {
  Definition in;
  in.rowCount = 4;
  in.columnCount = 4;
  in.rows[0] = row(0, 1, 1, 56, 56, RowMode::Fixed);
  in.rows[1] = row(1, 1, 4, 165);
  in.rows[2] = row(2, 1, 2, 100);
  in.rows[3] = row(3, 1, 5, 180, 322, RowMode::Content, 276);
  return in;
}
Definition snoopy() {
  Definition in;
  in.rowCount = 5;
  in.columnCount = 5;
  in.rows[0] = row(0, 1, 1, 76, 76, RowMode::Fixed);
  in.rows[1] = row(1, 1, 2, 130);
  in.rows[2] = row(2, 1, 2, 120);
  in.rows[3] = row(3, 1, 4, 230, 310, RowMode::Content, 270);
  in.rows[4] = row(4, 1, 1, 46, 46, RowMode::Fixed);
  return in;
}
void assertInside(Rect parent, Rect child) {
  assert(child.width >= 1 && child.height >= 1);
  assert(child.x >= parent.x && child.y >= parent.y);
  assert(child.x + child.width <= parent.x + parent.width);
  assert(child.y + child.height <= parent.y + parent.height);
}
void assertValidGeometry(const Definition& def, const Result& result) {
  assert(result.rowCount == def.rowCount && result.widgetCount == def.columnCount);
  int previousBottom = result.canvas.y;
  for (int i = 0; i < result.rowCount; ++i) {
    const Rect rect = result.rows[i];
    assertInside(result.canvas, rect);
    assert(rect.y >= previousBottom);
    previousBottom = rect.y + rect.height;
    int previousRight = rect.x;
    for (int c = 0; c < def.rows[i].columnCount; ++c) {
      const int index = def.rows[i].firstColumn + c;
      const Rect widget = result.widgets[index];
      assertInside(rect, widget);
      assert(widget.x >= previousRight);
      previousRight = widget.x + widget.width;
    }
  }
}

void testHaloAndSnoopy() {
  Result out;
  Definition h = halo();
  assert(resolve(h, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assertValidGeometry(h, out);
  assert(out.canvas.width == 500 && out.canvas.height == 744);
  assert(out.rows[0].height == 56);
  assert(out.rows[3].height == 276);
  assert(out.widgets[1].x == 14 && out.widgets[1].width == 500);
  Rect underlay;
  assert(spanRows(out, 1, 2, underlay));
  assert(underlay.y == out.rows[1].y);
  assert(underlay.height == out.rows[1].height + out.rows[2].height);
  assert(!spanRows(out, 0, 2, underlay));
  assert(!spanRows(out, 1, 3, underlay));
  assert(!spanRows(out, 2, 1, underlay));

  Definition s = snoopy();
  assert(resolve(s, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assertValidGeometry(s, out);
  assert(out.rows[0].height == 76);
  assert(out.rows[3].height == 270);
  assert(out.rows[4].height == 46);
  assert(out.rows[4].y + 46 <= 744);
  assert(resolve(s, Screen{480, 800, 14, 14, 0, 8}, out) == Status::Ok);
  assertValidGeometry(s, out);
  assert(out.rows[4].y + 46 <= 792);
}

void testDifferentColumnsPerRow() {
  Definition in;
  in.rowCount = 4;
  in.rowGap = 6;
  in.columnCount = 7;
  in.rows[0] = row(0, 1, 1, 62, 62, RowMode::Fixed);
  in.rows[1] = row(1, 2, 2, 90, 0, RowMode::Weighted, 0, 8);
  in.rows[2] = row(3, 3, 1, 80, 0, RowMode::Weighted, 0, 6);
  in.rows[3] = row(6, 1, 3, 150);
  in.columns[1] = column(2, 220);
  in.columns[2] = column(1, 180);
  in.columns[3] = column(1, 85);
  in.columns[4] = column(1, 85);
  in.columns[5] = column(1, 85);

  Result out;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assertValidGeometry(in, out);
  assert(out.widgets[1].width > out.widgets[2].width);
  assert(out.widgets[1].width + out.widgets[2].width + 8 == 500);
  assert(out.widgets[3].width + out.widgets[4].width + out.widgets[5].width + 12 == 500);
  assert(out.rows[1].y == out.rows[0].y + out.rows[0].height + 6);
  assert(out.rows[3].y > out.rows[2].y);
}

void testCappedTracksAndRounding() {
  Definition in;
  in.rowCount = 3;
  in.columnCount = 4;
  in.rows[0] = row(0, 1, 1, 50, 50, RowMode::Fixed);
  in.rows[1] = row(1, 2, 1, 100, 130, RowMode::Weighted, 0, 0);
  in.rows[2] = row(3, 1, 1, 100, 150);
  in.columns[1] = column(2, 1);
  in.columns[2] = column(1, 1);
  Result out;
  assert(resolve(in, Screen{101, 400}, out) == Status::Ok);
  assertValidGeometry(in, out);
  assert(out.rows[0].height == 50);
  assert(out.rows[1].height == 130 && out.rows[2].height == 150);
  assert(out.widgets[1].width == 67 && out.widgets[2].width == 34);
  assert(out.rows[2].y + out.rows[2].height < 400); // both rows capped

  // True largest remainder: with 2 pixels left after minima and weights
  // 1:2, each column receives one extra pixel (not both to weight 2).
  in.columns[1] = column(1, 1);
  in.columns[2] = column(2, 1);
  assert(resolve(in, Screen{4, 400}, out) == Status::Ok);
  assert(out.widgets[1].width == 2 && out.widgets[2].width == 2);

  in.rows[1].maxHeight = 0;
  in.rows[2].maxHeight = 0;
  assert(resolve(in, Screen{101, 400}, out) == Status::Ok);
  assertValidGeometry(in, out);
  assert(out.rows[2].y + out.rows[2].height == 400);
}

void testContentRowsAndTightViewport() {
  Definition in = halo();
  Result out;
  in.rows[3].preferredHeight = 600; // clamped to 322
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assert(out.rows[3].height == 322);
  in.rows[3].preferredHeight = 200;
  // Only minima fit; list stays scrollable without stealing Hero min height.
  assert(resolve(in, Screen{528, 549, 14, 14, 0, 48}, out) == Status::Ok);
  assert(out.rows[3].height == 180);
  assertValidGeometry(in, out);
}

void testFailClosed() {
  Definition in = halo();
  Result out;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assert(resolve(in, Screen{528, 792, 300, 300, 0, 48}, out) == Status::InvalidCanvas);
  assert(out.widgetCount == 0);
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 350}, out) == Status::NoSpace);
  in.rowCount = 9;
  assert(resolve(in, Screen{528, 792}, out) == Status::InvalidLayout);
  in = halo();
  in.columns[0].weight = 0;
  assert(resolve(in, Screen{528, 792}, out) == Status::InvalidTracks);
  in = halo();
  in.rows[1].columnCount = 2; // overlaps next row's flat column indices
  assert(resolve(in, Screen{528, 792}, out) == Status::InvalidTracks);
  in = halo();
  in.rows[1].minHeight = 650;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::NoSpace);
  in = halo();
  in.rows[1].mode = RowMode::Content; // invalid without max/preferred
  assert(resolve(in, Screen{528, 792}, out) == Status::InvalidTracks);
  in = halo();
  in.rows[3].preferredHeight = 0;
  assert(resolve(in, Screen{528, 792}, out) == Status::InvalidTracks);
  in = halo();
  in.columns[2].minWidth = 520;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::InvalidTracks);
}

void testMaxCapacityAndRepeatability() {
  Definition in;
  in.rowCount = 8;
  in.columnCount = 16;
  in.rowGap = 1;
  for (int r = 0; r < 8; ++r) {
    in.rows[r] = row(r * 2, 2, 1 + r % 3, 40);
    in.columns[r * 2] = column(1 + r % 2, 20);
    in.columns[r * 2 + 1] = column(2, 20);
  }
  Result out;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
  assertValidGeometry(in, out);
  Result again;
  assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, again) == Status::Ok);
  for (int i = 0; i < 16; ++i) {
    const auto& a = out.widgets[i];
    const auto& b = again.widgets[i];
    assert(a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height);
  }
}

void testRepeatedDeterministicBounds() {
  uint32_t seed = 0x31415926u;
  for (int rep = 0; rep < 500; ++rep) {
    Definition in;
    in.rowCount = static_cast<uint8_t>(2 + rep % 7);
    in.rowGap = static_cast<uint8_t>(rep % 5);
    int colCount = 0;
    for (int r = 0; r < in.rowCount; ++r) {
      seed = seed * 1664525u + 1013904223u;
      const int remaining = 16 - colCount - (in.rowCount - r - 1);
      const int cols = 1 + static_cast<int>((seed >> 16) % std::min(4, remaining));
      in.rows[r] = row(colCount, cols, 1 + (seed % 10), 40 + (seed >> 8) % 16);
      for (int c = 0; c < cols; ++c) {
        seed = seed * 1664525u + 1013904223u;
        in.columns[colCount++] = column(1 + seed % 10, 10 + (seed >> 16) % 25);
      }
    }
    in.columnCount = static_cast<uint8_t>(colCount);
    Result out;
    assert(resolve(in, Screen{528, 792, 14, 14, 0, 48}, out) == Status::Ok);
    assertValidGeometry(in, out);
    assert(out.rows[in.rowCount - 1].y + out.rows[in.rowCount - 1].height == 744);
  }
}

}  // namespace

int main() {
  testHaloAndSnoopy();
  testDifferentColumnsPerRow();
  testCappedTracksAndRounding();
  testContentRowsAndTightViewport();
  testFailClosed();
  testMaxCapacityAndRepeatability();
  testRepeatedDeterministicBounds();
}
