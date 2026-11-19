#include "components/home/HomeGridInteraction.h"

#include <cassert>

using namespace home_grid;
using namespace home_grid::interaction;
using namespace home_widgets;

namespace {
void testHaloBookUnderlayAndMenu() {
  Definition d;
  d.rowCount = 4;
  d.columnCount = 4;
  for (int i = 0; i < 4; ++i) {
    d.rows[i].firstColumn = static_cast<uint8_t>(i);
    d.rows[i].columnCount = 1;
    d.rows[i].mode = RowMode::Fixed;
    d.rows[i].minHeight = d.rows[i].maxHeight =
        static_cast<uint16_t>(i == 0 ? 56 : i == 1 ? 180 : i == 2 ? 125 : 250);
  }
  Plan plan;
  plan.widgetCount = 4;
  plan.widgets[0].kind = Kind::Header;
  plan.widgets[1].kind = Kind::Empty;
  plan.widgets[2].kind = Kind::Book;
  plan.widgets[3].kind = Kind::Shortcuts;
  plan.underlayCount = 1;
  plan.underlays[0] = Underlay{1, 2, 2};

  Result rects;
  assert(resolve(d, Screen{528, 792, 14, 14, 8, 48}, rects) == Status::Ok);
  Regions areas;
  assert(locate(plan, rects, areas));
  assert(areas.menuWidget == 3 && areas.bookWidget == 2);
  assert(areas.bookScene.y == rects.rows[1].y);
  assert(areas.bookScene.height == rects.rows[1].height + rects.rows[2].height);
  assert(areas.book.y == rects.rows[2].y);
  assert(areas.bookCacheSafe); // Full-row book has no neighboring widget.
  assert(areas.bookScene.y < areas.book.y);
  assert(areas.menu.y == rects.rows[3].y);
  assert(areas.menu.y + areas.menu.height <= rects.canvas.y + rects.canvas.height);

  const int rows = visibleMenuCapacity(areas.menu, 46, 3);
  assert(rows == 5);
  assert(menuHit(areas.menu, areas.menu.x + 9, areas.menu.y, 46, 3, rows) == 0);
  assert(menuHit(areas.menu, areas.menu.x + 9, areas.menu.y + 45, 46, 3, rows) == 0);
  assert(menuHit(areas.menu, areas.menu.x + 9, areas.menu.y + 46, 46, 3, rows) == -1);
  assert(menuHit(areas.menu, areas.menu.x + 9, areas.menu.y + 49, 46, 3, rows) == 1);
  assert(menuHit(areas.menu, areas.menu.x + 9, areas.menu.y + 49 * 4, 46, 3, rows) == 4);
  assert(menuHit(areas.menu, areas.menu.x - 1, areas.menu.y, 46, 3, rows) == -1);
  assert(menuHit(areas.menu, areas.menu.x + 1, areas.menu.y - 1, 46, 3, rows) == -1);
  assert(inside(areas.book, areas.book.x + 10, areas.book.y + 5));
  assert(!inside(areas.book, areas.book.x + 10, areas.bookScene.y + 5));
  assert(!inside(areas.book, areas.book.x + 10, areas.menu.y));
}

void testMixedColumnBookAndScrollableShortcuts() {
  Definition d;
  d.rowCount = 3;
  d.columnCount = 4;
  d.rows[0].columnCount = 1;
  d.rows[0].firstColumn = 0;
  d.rows[0].mode = RowMode::Fixed;
  d.rows[0].minHeight = d.rows[0].maxHeight = 56;
  d.rows[1].columnCount = 2;
  d.rows[1].firstColumn = 1;
  d.rows[1].minHeight = 100;
  d.rows[1].columnGap = 4;
  d.rows[1].weight = 2;
  d.rows[2].columnCount = 1;
  d.rows[2].firstColumn = 3;
  d.rows[2].minHeight = 54;
  d.rows[2].mode = RowMode::Content;
  d.rows[2].maxHeight = 250;
  d.rows[2].preferredHeight = 160;

  Plan plan;
  plan.widgetCount = 4;
  plan.widgets[0].kind = Kind::Header;
  plan.widgets[1].kind = Kind::Book;
  plan.widgets[2].kind = Kind::Clock;
  plan.widgets[3].kind = Kind::Shortcuts;
  Result rects;
  assert(resolve(d, Screen{480, 800, 12, 12, 4, 8}, rects) == Status::Ok);
  Regions areas;
  assert(locate(plan, rects, areas));
  assert(areas.menuWidget == 3);
  assert(areas.bookWidget == 1);
  assert(areas.bookScene.x == areas.book.x);
  assert(areas.bookScene.width == areas.book.width);
  assert(areas.book.width < rects.rows[1].width);
  assert(!areas.bookCacheSafe); // Byte-wide snapshot restore could spill into the clock.
  assert(visibleMenuCapacity(areas.menu, 54, 3) >= 1);

  plan.widgets[2].kind = Kind::Book;
  assert(!locate(plan, rects, areas));
  plan.widgets[2].kind = Kind::Clock;
  plan.widgets[3].kind = Kind::Empty;
  assert(!locate(plan, rects, areas));
  assert(areas.menuWidget == -1);
  assert(!areas.bookCacheSafe); // Inactive/incomplete layouts never cache.
}

void testMinimumOneRowAndBounds() {
  Rect menu{10, 100, 200, 54};
  assert(visibleMenuCapacity(menu, 54, 3) == 1);
  // Footer art keeps one trailing row gap, matching ThemeScene::drawMenu.
  assert(visibleMenuCapacity(Rect{10, 100, 200, 111}, 54, 3, false) == 2);
  assert(visibleMenuCapacity(Rect{10, 100, 200, 111}, 54, 3, true) == 1);
  // One visible row never makes Settings or plugin shortcuts unreachable.
  assert(home_menu::scrollTopForSelection(0, 8, 9, 1) == 8);
  assert(home_menu::scrollTopBy(8, -1, 9, 1) == 7);
  assert(home_menu::visibleRows(9, 8, 1) == 1);
  assert(visibleMenuCapacity(menu, 55, 3) == 0);
  assert(visibleMenuCapacity(Rect{10, 100, 0, 54}, 50, 3) == 0);
  assert(menuHit(menu, 10, 100, 54, 3, 1) == 0);
  assert(menuHit(menu, 10, 153, 54, 3, 1) == 0);
  assert(menuHit(menu, 10, 154, 54, 3, 1) == -1);
  assert(menuHit(menu, 9, 100, 54, 3, 1) == -1);
  assert(menuHit(menu, 210, 100, 54, 3, 1) == -1);
  assert(menuHit(menu, 10, 99, 54, 3, 1) == -1);
  assert(menuHit(menu, 10, 100, 0, 3, 1) == -1);
  assert(menuHit(menu, 10, 100, 54, 3, 0) == -1);
}
} // namespace

int main() {
  testHaloBookUnderlayAndMenu();
  testMixedColumnBookAndScrollableShortcuts();
  testMinimumOneRowAndBounds();
}
