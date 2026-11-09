#include "OtaVersion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

// Keep recognizing the original RC.-prefixed Nightlies during migration.
const char* versionStart(const char* value) {
  if (value == nullptr) return "";
  if (strncmp(value, "rn-vRC.", 7) == 0) return value + 4;
  if (strncmp(value, "RC.", 3) == 0) return value;
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

  const char* taggedVersion = versionStart(value);
  const bool legacyPrefixedNightly = strncmp(taggedVersion, "RC.", 3) == 0;
  const char* base = legacyPrefixedNightly ? taggedVersion + 3 : taggedVersion;
  int consumed = 0;
  if (base[0] == '\0' || sscanf(base, "%d.%d.%d%n", &out.major, &out.minor, &out.patch, &consumed) != 3) return out;

  out.valid = true;
  out.revision = parseRevision(base);
  const char* suffix = base + consumed;
  const bool commitNightly = legacyPrefixedNightly || strncmp(suffix, "n-", 2) == 0;
  out.releaseCandidate =
      (suffix[0] == 'r' && suffix[1] == 'c') || (suffix[0] == '-' && suffix[1] == 'r' && suffix[2] == 'c');
  out.development = strstr(base, "-dev") != nullptr;
  if (commitNightly) {
    if (strncmp(suffix, "n-", 2) != 0 || strlen(suffix + 2) != 7) {
      out.valid = false;
      return out;
    }
    for (const char* p = suffix + 2; *p; ++p) {
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
        out.valid = false;
        return out;
      }
    }
    memcpy(out.nightlyCommit, suffix + 2, 7);
    out.nightly = true;
  } else if (strncmp(suffix, "-n", 2) == 0) {
    unsigned int date = 0;
    unsigned int run = 0;
    char trailing = '\0';
    // The nightly stamp is YYYYMMDD.<run number>, with only decimal digits.
    const char* stamp = suffix + 2;
    bool validStamp = strlen(stamp) >= 10 && stamp[8] == '.';
    if (validStamp) {
      for (int i = 0; i < 8; ++i) {
        if (stamp[i] < '0' || stamp[i] > '9') validStamp = false;
      }
      for (const char* p = stamp + 9; *p; ++p) {
        if (*p < '0' || *p > '9') validStamp = false;
      }
    }
    if (!validStamp || sscanf(stamp, "%8u.%u%c", &date, &run, &trailing) != 2 || date < 20260101 || date > 20991231 ||
        (date / 100) % 100 < 1 || (date / 100) % 100 > 12 || date % 100 < 1 || date % 100 > 31 || run == 0) {
      out.valid = false;
      return out;
    }
    out.nightly = true;
    out.nightlyDate = date;
    out.nightlyRun = run;
  }
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

bool isNewerForChannel(const char* latestTag, const char* currentVersion, const Channel channel) {
  const Parsed latest = parse(latestTag);
  const Parsed current = parse(currentVersion);
  if (!latest.valid || !current.valid || latest.nightly != (channel == Channel::Nightly)) return false;

  if (latest.major != current.major) {
    // Switching to Stable is an explicit opt-in even if it rolls the base back.
    return channel == Channel::Stable && current.nightly ? true : latest.major > current.major;
  }
  if (latest.minor != current.minor) {
    return channel == Channel::Stable && current.nightly ? true : latest.minor > current.minor;
  }
  if (latest.patch != current.patch) {
    return channel == Channel::Stable && current.nightly ? true : latest.patch > current.patch;
  }

  if (current.nightly && latest.nightly) {
    if (latest.nightlyCommit[0] != '\0') {
      // A commit hash is an identity, not an ordered version number.
      return strcmp(latest.nightlyCommit, current.nightlyCommit) != 0;
    }
    if (current.nightlyCommit[0] != '\0') return false;
    if (latest.nightlyDate != current.nightlyDate) return latest.nightlyDate > current.nightlyDate;
    return latest.nightlyRun > current.nightlyRun;
  }
  if (current.nightly && channel == Channel::Stable) return true;
  if (channel == Channel::Nightly) return !current.development;
  return isNewer(latestTag, currentVersion);
}

bool isSwitchingToStable(const char* currentVersion, const Channel channel) {
  return channel == Channel::Stable && parse(currentVersion).nightly;
}

bool buildBoardAssetSuffix(const char* boardName, size_t boardNameLen, bool hyphenBoardSuffixes, char* out,
                           size_t outSize) {
  if (boardName == nullptr || boardNameLen == 0 || out == nullptr || outSize == 0) return false;

  const std::string_view board(boardName, boardNameLen);
  const char* fixedSuffix = nullptr;
  if (board == "x4") {
    fixedSuffix = "-x3-x4";
  } else if (hyphenBoardSuffixes && board == "metalio_eink4") {
    fixedSuffix = "-metalio-eink4";
  } else if (hyphenBoardSuffixes && board == "eego_a4") {
    fixedSuffix = "-eego-a4";
  }

  // Keep the board tag length, not its trailing delimiter, in the fallback.
  if (fixedSuffix == nullptr && boardNameLen >= outSize) return false;
  const int written = fixedSuffix != nullptr
                          ? snprintf(out, outSize, "%s", fixedSuffix)
                          : snprintf(out, outSize, "-%.*s", static_cast<int>(boardNameLen), boardName);
  return written > 0 && static_cast<size_t>(written) < outSize;
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
