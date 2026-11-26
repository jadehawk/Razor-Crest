#pragma once

#include <string>

namespace readingHistory {
// Called before activities start. The resolver must use the configured sync
// document identity, never a title-only guess. A failed conversion retains BINs.
using ResolveDocument = std::string (*)(const std::string& path);
bool migrateLegacy(ResolveDocument resolve, bool (*recoverLegacy)() = nullptr, void (*yieldWork)() = nullptr);
// False while publication is interrupted: callers must not append to the old log.
bool migrationPending();
bool migrationReady();
}  // namespace readingHistory
