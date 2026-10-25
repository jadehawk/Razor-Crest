#pragma once

// Stable hardware identities shared by native catalog discovery and WebUI.
#include <Arduino.h>
#include <BoardConfig.h>

#include <cstddef>

namespace devicehardware {
inline const char* model() {
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return "x3";
    case BoardConfig::Board::XteinkX4:
      return "x4";
    case BoardConfig::Board::XteinkX4Pro:
      return "x4pro";
    case BoardConfig::Board::XteinkX4Classic:
      return "x4classic";
    case BoardConfig::Board::Sticky:
      return "sticky";
    case BoardConfig::Board::PaperMono:
      return "papermono";
    default:
      return BoardConfig::ACTIVE.name;  // Stable FreeInk profile identifier.
  }
}

inline size_t psramBytes() {
  // Installed hardware capacity, not volatile free heap. C3 returns zero.
  return static_cast<size_t>(ESP.getPsramSize());
}
}  // namespace devicehardware
