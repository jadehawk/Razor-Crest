#include "OtaVersion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Tags may carry a firmware prefix ("razor-crest-v2.0.0-r5") or a leading "v";
// the numeric version always starts at the first digit.
const char* versionStart(const char* value) {
  if (value == nullptr) return "";
  while (*value != '\0' && (*value < '0' || *value > '9')) ++value;
  return value;
}

int parseRevision(const char* value) {
  for (const char* p = value; (p = strstr(p, "-r")) != nullptr; p += 2) {
    if (p[2] >= '0' && p[2] <= '9') return atoi(p + 2);
  }
  return 0;
}

}  // namespace

namespace otaversion {

Parsed parse(const char* value) {
  Parsed out;
  if (value == nullptr) return out;

  const char* base = versionStart(value);
  int consumed = 0;
  if (base[0] == '\0' || sscanf(base, "%d.%d.%d%n", &out.major, &out.minor, &out.patch, &consumed) != 3) return out;

  out.valid = true;
  out.revision = parseRevision(base);
  const char* suffix = base + consumed;
  out.releaseCandidate =
      (suffix[0] == 'r' && suffix[1] == 'c') || (suffix[0] == '-' && suffix[1] == 'r' && suffix[2] == 'c');
  out.development = strstr(base, "-dev") != nullptr;
  return out;
}

bool isNewer(const char* latestTag, const char* currentVersion) {
  const Parsed latest = parse(latestTag);
  const Parsed current = parse(currentVersion);
  if (!latest.valid || !current.valid) return false;

  // Development images are built past a release tag; never downgrade them.
  if (current.development) return false;

  if (latest.major != current.major) return latest.major > current.major;
  if (latest.minor != current.minor) return latest.minor > current.minor;
  if (latest.patch != current.patch) return latest.patch > current.patch;
  if (latest.revision != current.revision) return latest.revision > current.revision;

  // An RC build of the same base version is older than its final release.
  if (current.releaseCandidate && !latest.releaseCandidate) return true;
  return false;
}

bool buildAssetName(const char* assetPrefix, const char* releaseTag, const char* suffix, char* out, size_t outSize) {
  if (assetPrefix == nullptr || releaseTag == nullptr || suffix == nullptr || out == nullptr || outSize == 0)
    return false;

  const char* version = versionStart(releaseTag);
  if (version[0] == '\0') return false;

  const int written = snprintf(out, outSize, "%s%s%s.bin", assetPrefix, version, suffix);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

}  // namespace otaversion
