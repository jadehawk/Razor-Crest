#pragma once

#include <cstdint>

#include "ReadingStatsBookIndex.h"
#include "ReadingStatsData.h"

// Native, local reading-statistics provider. Recording is independent of
// plugin subscriptions, Wi-Fi, and any particular display.
namespace readingstats {

// Records one flushed reader session; returns false when SD persistence fails.
bool record(const char* bookPath, uint32_t activeSeconds, uint32_t qualifiedTurns, int64_t trustedStartEpoch,
            const char* title = nullptr, const char* author = nullptr);

// Register a successfully opened book immediately. This creates only its
// catalog entry (0m until a qualifying reader session), not an invented
// session/global-total update. Deleted history can reappear on a later open.
bool rememberOpenedBook(const char* path, const char* title, const char* author);

// Bounded book index: the UI pages by position, never loads every title.
bool ensureBookIndexed(uint64_t hash, const char* path, const char* title, const char* author);
uint32_t bookCount();
bool bookAt(uint32_t index, BookEntry& out);
// Removes book-specific history, book catalog entry, and backup. Does not
// mutate the independent cumulative global snapshot or the source EPUB.
bool removeBookRecord(uint64_t hash);
bool loadBookHash(uint64_t hash, Snapshot& out);

// A source record is a one-time historical snapshot. It is never imported a
// second time, even if the original source file remains or changes later.
// Callers supply an immutable per-source book identifier (not merely a title).
enum class LegacySource : uint32_t { CrossInk = 1, VCodex = 2 };
enum class LegacyResult : uint8_t { Imported, AlreadyImported, OtherSource, Error };
LegacyResult importLegacy(LegacySource source, const char* stableKey, const char* bookPath, const char* title,
                          const char* author, const Snapshot& delta, bool addToGlobal = true);
bool recoverLegacyImport();

// Read-only snapshots for browser and future on-device plugin data sources.
// Missing/corrupt files yield a fresh, empty valid snapshot and return false.
bool loadGlobal(Snapshot& out);
bool loadBook(const char* bookPath, Snapshot& out);

}  // namespace readingstats
