#include <cassert>

#include "../../src/activities/reader/ReaderSession.h"

int main() {
  ReaderSession session;
  session.onRenderComplete(1000, 0, 100);
  session.noteTurn(true, true);
  session.onRenderComplete(6200, 0, 200);
  assert(!session.isEmitWorthy());
  assert(!session.isTrackWorthy());  // Only 5 counted seconds.

  session.noteTurn(true, true);
  session.onRenderComplete(12200, 1710000000, 300);
  assert(session.trackedSeconds() == 11);
  assert(session.qualifiedTurns() == 2);
  assert(!session.isEmitWorthy());  // The earlier unknown-clock time is not exported.

  session.noteTurn(true, true);
  session.onRenderComplete(24200, 1710000012, 400);
  assert(session.trackedSeconds() == 23);
  assert(session.durationSeconds() == 12);
  assert(session.isEmitWorthy());
  assert(session.startTime() == 1710000000);
  assert(session.endTime() == 1710000012);
  assert(session.startProgressBp() == 300);
  assert(session.endProgressBp() == 400);
  assert(session.qualifiedTurns() == 3);

  session.noteTurn(false, true);
  session.onRenderComplete(30000, 1710000020, 350);
  assert(session.trackedSeconds() == 23);
  session.noteTurn(true, false);
  session.onRenderComplete(35000, 1710000025, 350);
  assert(session.qualifiedTurns() == 3);
  session.reset();
  assert(!session.isTrackWorthy());
  assert(session.qualifiedTurns() == 0);
}
