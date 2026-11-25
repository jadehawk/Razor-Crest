#include "ReadingStatsSleepSource.h"

#include <HalStorage.h>

#include "util/PluginHttp.h"
#include "util/PluginLocations.h"

namespace readingstatssleep {
std::vector<std::string> imageCandidates() {
  const auto dir = PluginLocations::findPluginDir(kId);
  if (!enabled(dir)) return {};
  std::string token;
  if (pluginhttp::loadTokenFromFile("/.crosspoint/reading-statistics-token.json", "token", token) ||
      pluginhttp::loadTokenFromFile("/.crosspoint/reading-statistics-token.json", "access_token", token))
    return {dir + kServerImage, dir + kDeviceImage};
  return {dir + kDeviceImage};
}

bool enabled(const std::string& dir) {
#if CROSSPOINT_DESSEN_APPS
  if (dir.empty() || !Storage.exists((dir + "/app.bin").c_str())) return false;
  const std::string flag = dir + kEnabledFile;
  std::string raw;
  return Storage.exists(flag.c_str()) && Storage.readFileToString("SLP", flag, 1, raw) && raw[0] == 1;
#else
  (void)dir;
  return false;
#endif
}

bool enabled() { return enabled(PluginLocations::findPluginDir(kId)); }
}  // namespace readingstatssleep
