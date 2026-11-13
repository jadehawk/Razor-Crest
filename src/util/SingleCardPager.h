#pragma once
#include <cstddef>
#include <cstdint>

// Stateless, renderer-agnostic single-card pager for Home and native plugin cards.
// Callers own their data, focus state and gesture/button routing.
namespace singlecard {
struct Pager {
  size_t size = 0;
  size_t index = 0;
  bool enabled = true;
  bool canCycle() const { return enabled && size > 1; }
  // Home uses bounded paging; other widgets keep move()'s cyclic behavior.
  bool canMoveBounded(int direction) const {
    return canCycle() && (direction < 0 ? index > 0 : direction > 0 && index + 1 < size);
  }
  size_t moveBounded(int direction) {
    if (canMoveBounded(direction)) index = direction < 0 ? index - 1 : index + 1;
    return current();
  }
  size_t current() const { return size ? index % size : 0; }
  size_t move(int direction) {
    if (!canCycle()) return current();
    index = direction < 0 ? (current() + size - 1) % size : (current() + 1) % size;
    return index;
  }
  void clamp() { index = current(); }
};
}  // namespace singlecard
