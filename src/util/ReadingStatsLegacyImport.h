#pragma once

#include <cstdint>

#include "ReadingStatsLegacyPreview.h"

namespace readingstats::legacy {
struct ImportReport {
  bool sourcePresent = false;
  bool supported = false;
  bool success = false;
  uint32_t imported = 0;
  uint32_t alreadyImported = 0;
  uint32_t booksSeen = 0;
  uint32_t errors = 0;
  bool otherSourceAlreadyImported = false;
};

// Requires explicit user confirmation. Preserves legacy source files.
// Journal and receipts are managed by the native BIN store.
ImportReport importSource(Source source);
}  // namespace readingstats::legacy
