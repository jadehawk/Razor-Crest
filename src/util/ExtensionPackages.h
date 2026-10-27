#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <string>

namespace extensionpackages {

enum class Kind : uint8_t { Plugin, Theme, Wallpaper };

struct Release {
  Kind kind = Kind::Plugin;
  std::string name;     // Stable install ID / collection.
  std::string variant;  // Download filename identity; resolution-scoped.
  std::string version;
  std::string url;
  std::string sha256;
  uint16_t width = 0;
  uint16_t height = 0;
};

// Capabilities are exact tokens in the firmware's comma-separated declaration.
bool supports(const char* capability);
bool supportsAll(JsonArrayConst capabilities);

// Reads only the selected package from the on-SD streamed browse response.
// Theme and wallpaper variants match the actual panel resolution, not a model name.
bool selectRelease(const std::string& responsePath, Kind kind, const std::string& collection, uint16_t width,
                   uint16_t height, Release& release);

// Verifies the published digest and an exact manifest-declared ZIP file set;
// stages unpacked files and atomically swaps the package directory on SD.
bool installVerifiedZip(const std::string& archivePath, const Release& release);

}  // namespace extensionpackages
