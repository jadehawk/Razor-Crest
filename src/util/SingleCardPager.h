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
  size_t current() const { return size ? index % size : 0; }
  size_t move(int direction) {
    if (!canCycle()) return current();
    index = direction < 0 ? (current() + size - 1) % size : (current() + 1) % size;
    return index;
  }
  void clamp() { index = current(); }
};
}  // namespace singlecard
