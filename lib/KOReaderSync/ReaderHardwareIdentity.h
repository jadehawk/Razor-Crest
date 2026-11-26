#pragma once

#include <esp_mac.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace readerHardwareIdentity {
using Id = std::array<char, 24>;
inline constexpr char kPrefix[] = "crosspoint-";

// Factory eFuse identity, not the Wi-Fi interface's mutable MAC address.
// This is an identifier only, never an authentication credential.
inline bool read(Id& out) {
  out.fill(0);
  uint8_t mac[6]{};
  if (esp_efuse_mac_get_default(mac) != 0) return false;
  if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) return false;
  snprintf(out.data(), out.size(), "%s%02x%02x%02x%02x%02x%02x", kPrefix, mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
  return true;
}

inline bool validMac(const char* mac) {
  return mac && std::strlen(mac) == 12 && std::strspn(mac, "0123456789abcdef") == 12 &&
         std::strcmp(mac, "000000000000") != 0;
}

inline const char* compact(const Id& id) { return id.data() + sizeof(kPrefix) - 1; }
}  // namespace readerHardwareIdentity
