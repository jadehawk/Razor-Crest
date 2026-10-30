#pragma once

#include <cstddef>

// Release-tag and version helpers for the Razor-Crest OTA flow.
//
// Release tags look like "razor-crest-v2.0.0-r5"; the running firmware reports
// CROSSPOINT_VERSION such as "2.0.0-r5-x4pro". Comparing only the three
// semantic-version segments would treat r4 and r5 as equal, so the fork
// revision is compared as a fourth numeric component.
namespace otaversion {

struct Parsed {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int revision = 0;
  bool releaseCandidate = false;
  bool development = false;
  bool valid = false;
};

Parsed parse(const char* value);

bool isNewer(const char* latestTag, const char* currentVersion);

// Builds "<assetPrefix><version><suffix>.bin" from a release tag, e.g.
// "razor-crest-" + "razor-crest-v2.0.0-r5" + "-x4pro" ->
// "razor-crest-2.0.0-r5-x4pro.bin".
// Returns false when the tag carries no version or the result does not fit.
bool buildAssetName(const char* assetPrefix, const char* releaseTag, const char* suffix, char* out, size_t outSize);

}  // namespace otaversion
