#include "ReadingStatsSleepSource.h"

#include <cstring>

#include "activities/plugins/StatsDashboardActivity.h"
#include "util/PluginLocations.h"

namespace readingstatssleep {
bool selected(const char* id) { return id && std::strcmp(id, kId) == 0; }

std::string manifestPath() {
  const std::string pluginDir = PluginLocations::findPluginDir("reading-statistics");
  return pluginDir.empty() ? std::string() : pluginDir + "/device.json";
}

bool available() {
  const std::string path = manifestPath();
  return !path.empty() && StatsDashboardActivity::supportsManifest(path);
}
}  // namespace readingstatssleep
