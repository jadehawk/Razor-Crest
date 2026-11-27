#pragma once
#include <cstdint>

namespace readingHistory {
class Provider;
// Generic local provenance descriptor. Mode 0 reads; mode 1 explicitly
// binds a verified, unowned history to the current physical reader.
int readSourceIdentity(const Provider& provider, uint32_t mode, uint8_t* out, uint32_t cap);
}  // namespace readingHistory
