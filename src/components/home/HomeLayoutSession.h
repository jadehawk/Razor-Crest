#pragma once

// One-shot Home layout cold-path parse + safe activation. Only retained native
// descriptors and resolved rectangles survive; ArduinoJson and HalFile do not.
#include "components/home/HomeLayoutJson.h"

namespace home_grid::session {

struct State {
  home_grid::json::Parsed parsed{};
  home_grid::activation::Active active{};
};
static_assert(sizeof(State) <= 1184, "Retained Home grid exceeds low-memory envelope");

// Caller opens a manifest-validated path belonging to the selected theme.
// The input stream is consumed once, outside render()/touch/loop callbacks.
template <typename File>
inline home_grid::json::Status load(File& file, const home_grid::json::Resources& resources,
                                    const home_grid::Screen& screen,
                                    const home_grid::activation::Policy& capabilities,
                                    State& state) {
  state = State{};
  // Reject unsupported themes before touching SD bytes or allocating the
  // ArduinoJson document; the legacy Home remains the sole active renderer.
  if (!capabilities.hasLayout || !capabilities.supportsLayout ||
      !capabilities.supportsWidgets)
    return home_grid::json::Status::InvalidLayout;
  const home_grid::json::Status status =
      home_grid::json::parseFile(file, resources, state.parsed);
  if (status != home_grid::json::Status::Ok) {
    state = State{};
    return status;
  }
  if (home_grid::activation::prepare(state.parsed.layout, state.parsed.widgets,
                                     screen, capabilities, state.active) !=
      home_grid::activation::Status::Ready) {
    state = State{};
    return home_grid::json::Status::InvalidLayout;
  }
  return home_grid::json::Status::Ok;
}

}  // namespace home_grid::session
