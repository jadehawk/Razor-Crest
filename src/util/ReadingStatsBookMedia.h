#pragma once

#include <cstdint>
#include <string>

#include "ReadingStatsBookIndex.h"

namespace readingstats::media {

// Persistent SD snapshots, independent of EPUB/library cache lifetime.
// Two independent snapshots: list cover (~68px) and hero cover (~142px).
// A previously captured legacy preview remains the fallback if source is gone.
std::string coverPath(uint64_t hash, int height);
uint64_t hashSourcePath(const char* path);
std::string bookSource(const BookEntry& entry);
std::string author(const BookEntry& entry);
int progress(const BookEntry& entry, bool refresh = true);
bool ensureCover(const BookEntry& entry, int height);
void remove(uint64_t hash);

}  // namespace readingstats::media
