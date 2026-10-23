#include <cstdint>

#include "../../src/activities/DownloadProgressPolicy.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
  using namespace catalogprogress;
  CHECK(kDefault.percentStep == 5 && kDefault.maxRefreshIntervalMs == 5000);
  CHECK(kExtensionZip.percentStep == 20 && kExtensionZip.maxRefreshIntervalMs == 15000);
  CHECK(shouldRefresh(1, -1, 0, kExtensionZip));
  CHECK(!shouldRefresh(19, 0, 4000, kExtensionZip));
  CHECK(shouldRefresh(20, 0, 4000, kExtensionZip));
  CHECK(!shouldRefresh(39, 20, 4000, kExtensionZip));
  CHECK(shouldRefresh(39, 20, 15000, kExtensionZip));
  CHECK(shouldRefresh(100, 80, 0, kExtensionZip));
  CHECK(shouldRefresh(5, 0, 4000, kDefault));  // Standard downloads unchanged.
  CHECK(shouldRefresh(4, 0, 5000, kDefault));
  CHECK(!shouldRefresh(4, 0, 4999, kDefault));
  return 0;
}
