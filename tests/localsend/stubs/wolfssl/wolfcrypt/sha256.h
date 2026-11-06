#pragma once
#include <cstdint>
#include <cstring>
using word32 = uint32_t;
// A digest sentinel, not SHA-256: verifies receiver decisions and cleanup only.
struct wc_Sha256 {
  uint32_t bytes = 0;
};
inline int wc_InitSha256(wc_Sha256* h) {
  h->bytes = 0;
  return 0;
}
inline int wc_Sha256Update(wc_Sha256* h, const uint8_t*, word32 n) {
  h->bytes += n;
  return 0;
}
inline int wc_Sha256Final(wc_Sha256*, uint8_t* out) {
  std::memset(out, 0, 32);
  return 0;
}
inline void wc_Sha256Free(wc_Sha256*) {}
