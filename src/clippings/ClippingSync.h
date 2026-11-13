#pragma once

#include <cstdint>
#include <string>

namespace clippingSync {
enum class Mode { Bidirectional, UploadOnly };

// Disabled in KOSync settings by default. Returns false on an enabled sync failure.
// UploadOnly sends pending additions/deletions without applying any remote changes.
// timeoutMs == 0 retains the manual sync client's default timeout.
// C3 requires an active framebuffer loan with rendering locked until this returns.
bool run(const std::string& bookPath, const std::string& documentHash, Mode mode = Mode::Bidirectional,
         uint32_t timeoutMs = 0);
}  // namespace clippingSync
