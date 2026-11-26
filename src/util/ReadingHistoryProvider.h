#pragma once

#include <HalStorage.h>

#include "ReadingHistory.h"

namespace readingHistory {
struct BookInfo {
  DocumentId id{};
  uint16_t progress = UINT16_MAX;  // Basis points; UINT16_MAX is unknown.
  uint8_t titleLength = 0;
  char title[80]{};
  char path[176]{};
  char author[48]{};
  uint64_t seconds = 0;
};

// Single owner. The JSONL is authoritative; the checkpoint is derived and
// disposable. A completed query retains only its summary and an SD file handle.
class Provider {
 public:
  Status begin(std::string_view document = {}, uint32_t anchorDay = 0, bool detailedBooks = false);
  Status step();
  void cancel();
  const Summary* summary() const { return state == Status::Ready ? &cached : nullptr; }
  uint64_t processedSessions() const {
    return state == Status::Ready ? cached.overall.sessions : scan.processedSessions();
  }
  uint64_t rejectedLines() const { return state == Status::Ready ? cached.rejected : scan.rejectedLines(); }
  bool readDay(uint32_t date, Day& out) const;
  bool readWeekDay(uint32_t date, uint64_t (&out)[4], bool selected) const;
  bool readBook(size_t index, DocumentId& id, uint32_t& offset) const;
  Status readBooks(size_t first, BookInfo* out, size_t count) const;
  uint64_t expectedBytes() const { return expected; }
  uint64_t processedBytes() const { return processed; }
  uint64_t parsedBytes() const { return parsed; }
  size_t aggregationBuffersBytes() const;
  bool validatingCache() const { return verifying; }
  unsigned progressPercent() const {
    if (state == Status::Ready) return 100;
    if (!expected || state != Status::Scanning) return 0;
    return static_cast<unsigned>(processed * 100 / expected);
  }

 private:
  static constexpr size_t READ_BYTES = 4096;
  void close();
  Status fail(Status reason);
  Status fresh();
  Status finish();
  Status resumeCheckpoint();
  bool loadCheckpoint();
  bool saveCheckpoint();
  bool readRecord(uint64_t position, void* out, size_t size) const;

  Query scan;
  HalFile file;
  mutable HalFile checkpoint;
  std::unique_ptr<char[]> chunk;
  Summary cached{};
  char documentKey[33]{};
  uint32_t requestedAnchor = 0;
  uint64_t expected = 0;
  uint64_t processed = 0;
  uint64_t parsed = 0;
  uint64_t sourceHash = 0;
  uint64_t checkpointHash = 0;
  Status state = Status::Idle;
  bool verifying = false;
  bool detailedBooks = false;
};
}  // namespace readingHistory
