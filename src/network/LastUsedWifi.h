#pragma once

#include <cstdint>

// One bounded, non-interactive attempt at the last successfully used Wi-Fi.
// Requires no network picker; the caller owns Wi-Fi teardown.
bool connectLastUsedWifi(uint32_t timeoutMs, bool showPopup = false);
