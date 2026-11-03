#include <cstddef>
#include <cstdint>
#include <string>

#include "../../src/components/TransferProgress.h"

#define CHECK(condition)               \
  do {                                 \
    if (!(condition)) return __LINE__; \
  } while (false)

int main() {
  using namespace transferprogress;
  CHECK(kBarHeight == 16);
  CHECK(percent(0, 0) == 0);  // Unknown length: no percent.
  CHECK(percent(0, 100) == 0);
  CHECK(percent(49, 100) == 49);
  CHECK(percent(100, 100) == 100);
  CHECK(percent(150, 100) == 100);  // Clamp racey final chunks.
  CHECK(percent(2501696, 5657632) == 44);
  CHECK(formatPercent(2501696, 5657632) == "44%");
  CHECK(formatBytes(0) == "0 B");
  CHECK(formatBytes(999) == "999 B");
  CHECK(formatBytes(1024) == "1.0 KB");
  CHECK(formatBytes(12000) == "12 KB");
  CHECK(formatBytes(1000000) == "1.0 MB");
  CHECK(formatBytes(2501696) == "2.5 MB");
  CHECK(formatBytes(5657632) == "5.7 MB");
  CHECK(formatBytes(1000000000) == "1.0 GB");
  CHECK(formatTransfer(2501696, 5657632) == "2.5 MB / 5.7 MB");
  CHECK(formatTransfer(1024, 0) == "1.0 KB");
  return 0;
}
