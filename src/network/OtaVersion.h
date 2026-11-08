#pragma once

#include <cstddef>

// Release-tag and version helpers for the Razor-Crest OTA flow.
//
// Release tags look like "razor-crest-v2.0.0-r5"; the running firmware reports
// CROSSPOINT_VERSION such as "2.0.0-r5". Comparing only the three
// semantic-version segments would treat r4 and r5 as equal, so the fork
// revision is compared as a fourth numeric component.
namespace otaversion {

enum class Channel { Stable, Nightly };

struct Parsed {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int revision = 0;
  bool releaseCandidate = false;
  bool development = false;
  bool nightly = false;
  unsigned int nightlyDate = 0;
  unsigned int nightlyRun = 0;
  char nightlyCommit[8] = {};
  bool valid = false;
};

Parsed parse(const char* value);

bool isNewer(const char* latestTag, const char* currentVersion);

// Compare only within the chosen channel. Changing from Nightly to Stable is
// an explicit channel switch, not an automatic downgrade.
bool isNewerForChannel(const char* latestTag, const char* currentVersion, Channel channel);
bool isSwitchingToStable(const char* currentVersion, Channel channel);

// Board names in the firmware tag are length-delimited, not null-terminated.
// Resolve their release asset suffix without reading the trailing ';'.
// hyphenBoardSuffixes selects the Razor-Crest names for MetalIO and EEGO;
// upstream releases retain their underscore spellings.
bool buildBoardAssetSuffix(const char* boardName, size_t boardNameLen, bool hyphenBoardSuffixes, char* out,
                           size_t outSize);

// Builds "<assetPrefix><version><suffix>.bin" from a release tag, e.g.
// "razor-crest-" + "razor-crest-v2.0.0-r5" + "-x4pro" ->
// "razor-crest-2.0.0-r5-x4pro.bin".
// Returns false when the tag carries no version or the result does not fit.
bool buildAssetName(const char* assetPrefix, const char* releaseTag, const char* suffix, char* out, size_t outSize);

}  // namespace otaversion
