#pragma once

#include <cstdint>

// Fixed-scalar active-reading summary. The owner reports turn outcomes before
// render and completed pages after render; no allocation or I/O occurs here.
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();

  bool isTrackWorthy() const { return duration >= 10; }
  bool isEmitWorthy() const { return startEpoch > 0 && eventDuration >= 10 && endTime() > startEpoch; }
  uint32_t qualifiedTurns() const { return turns; }
  uint32_t trackedSeconds() const { return duration; }
  int64_t startTime() const { return startEpoch; }
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + eventDuration : 0; }
  uint32_t durationSeconds() const { return eventDuration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

 private:
  static uint16_t clampProgress(int progressBp);

  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint32_t eventDuration = 0;
  uint32_t turns = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  bool hasAnchor = false;
  bool pendingForward = false;
};
