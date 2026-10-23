#include "ExtensionPackages.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <JsonListParser.h>
#include <Logging.h>
#include <ZipFile.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "util/FirmwareIdentity.h"

namespace extensionpackages {
namespace {

constexpr size_t MAX_FILES = 80;
constexpr size_t MAX_ARCHIVE_BYTES = 32u * 1024u * 1024u;
constexpr size_t MAX_EXPANDED_BYTES = 64u * 1024u * 1024u;
constexpr size_t MAX_FILE_BYTES = 8u * 1024u * 1024u;
constexpr size_t MAX_MANIFEST_BYTES = 12288;
constexpr size_t MAX_VARIANTS = 8;
constexpr size_t HASH_CHUNK = 1024;

bool safeId(const std::string& value) {
  if (value.empty() || value.size() > 32 || !((value[0] >= 'a' && value[0] <= 'z') ||
                                             (value[0] >= '0' && value[0] <= '9')))
    return false;
  for (char ch : value) {
    if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '-') return false;
  }
  return true;
}

bool safeRelative(const std::string& path) {
  if (path.empty() || path.size() > 150 || path[0] == '/' || path[0] == '.' ||
      path.find("..") != std::string::npos || path.find("//") != std::string::npos ||
      path.find('\\') != std::string::npos || path.find(':') != std::string::npos ||
      path.back() == '/')
    return false;
  for (char ch : path) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/'))
      return false;
  }
  return true;
}

bool validHash(const std::string& hash) {
  if (hash.size() != 64) return false;
  for (char ch : hash)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  return true;
}

bool validRelease(const Release& release) {
  if (!safeId(release.name) || !validHash(release.sha256) ||
      release.version.empty() || release.version.size() > 24 ||
      release.version.find_first_not_of("0123456789.") != std::string::npos)
    return false;
  const char* suffix = release.kind == Kind::Theme ? "crosspoint-theme.zip" : "crosspoint-wallpaper.zip";
  const std::string expected = "/" + release.name + "-" + release.version + "." + suffix;
  return release.url.rfind("https://github.com/", 0) == 0 &&
         release.url.find("/releases/download/v" + release.version + "/") != std::string::npos &&
         release.url.size() >= expected.size() &&
         release.url.compare(release.url.size() - expected.size(), expected.size(), expected) == 0;
}

struct SelectedRow {
  const std::string& name;
  JsonListParser::Row row;
  bool matched = false;
};

void captureRow(void* context, JsonListParser::Row& row) {
  auto& target = *static_cast<SelectedRow*>(context);
  if (row.field[JsonListParser::F_TITLE] != target.name) return;
  target.row = std::move(row);
  target.matched = true;
}

bool readRow(const std::string& path, const std::string& collection,
             const std::string* const (&fields)[JsonListParser::F_COUNT],
             const std::string& arrayPath, JsonListParser::Row& result) {
  HalFile file;
  if (!Storage.openFileForRead("EXT", path, file)) return false;
  SelectedRow selected{collection, {}, false};
  auto parser = std::unique_ptr<JsonListParser>(
      new (std::nothrow) JsonListParser("items", fields, arrayPath, captureRow, &selected));
  if (!parser) return false;
  const bool ok = parser->parse(
      [](void* context, char* buffer, size_t length) {
        return static_cast<HalFile*>(context)->read(buffer, length);
      },
      &file);
  if (!ok || !selected.matched) return false;
  result = std::move(selected.row);
  return true;
}

bool digestMatches(const std::string& path, const std::string& expected) {
  HalFile file;
  if (!Storage.openFileForRead("EXT", path, file)) return false;
  if (file.fileSize64() == 0 || file.fileSize64() > MAX_ARCHIVE_BYTES) return false;
  uint8_t buffer[HASH_CHUNK];
  uint8_t hash[32] = {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  bool ok = true;
  for (int n; (n = file.read(buffer, sizeof(buffer))) > 0;) {
    if (mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(n)) != 0) {
      ok = false;
      break;
    }
  }
  if (ok) ok = mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;
  char actual[65] = {};
  for (size_t i = 0; i < sizeof(hash); ++i)
    snprintf(actual + 2 * i, 3, "%02x", static_cast<unsigned>(hash[i]));
  return expected == actual;
}

std::string destination(const Release& release) {
  if (release.kind == Kind::Theme)
    return "/.crosspoint/themes/" + release.name + ".crosspoint-theme";
  return "/.crosspoint/wallpapers/" + release.name + ".crosspoint-wallpaper";
}

bool hasRoomForPackage(const Release& release) {
  const std::string dest = destination(release);
  if (Storage.exists(dest.c_str())) return true;
  const char* root = release.kind == Kind::Theme ? "/.crosspoint/themes" : "/.crosspoint/wallpapers";
  const char* suffix = release.kind == Kind::Theme ? ".crosspoint-theme" : ".crosspoint-wallpaper";
  auto directory = Storage.open(root);
  if (!directory || !directory.isDirectory()) return true;
  unsigned count = 0;
  for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (!entry.isDirectory()) continue;
    char name[96] = {};
    entry.getName(name, sizeof(name));
    const size_t length = strlen(name), tail = strlen(suffix);
    if (length > tail && strcmp(name + length - tail, suffix) == 0 && ++count >= 8) return false;
  }
  return true;
}

bool readManifest(ZipFile& archive, const Release& release, JsonDocument& manifest,
                  std::vector<std::string>& files) {
  const std::string top = release.name +
      (release.kind == Kind::Theme ? ".crosspoint-theme/" : ".crosspoint-wallpaper/");
  const std::string entry = top + "manifest.json";
  size_t len = 0;
  if (!archive.getInflatedFileSize(entry.c_str(), &len) || len < 8 || len > MAX_MANIFEST_BYTES) return false;
  std::unique_ptr<uint8_t, decltype(&free)> text(archive.readFileToMemory(entry.c_str()), &free);
  if (!text || deserializeJson(manifest, reinterpret_cast<const char*>(text.get()), len,
                               DeserializationOption::NestingLimit(8)) != DeserializationError::Ok)
    return false;
  const char* kind = release.kind == Kind::Theme ? "theme-pack" : "wallpaper-pack";
  if (strcmp(manifest["kind"] | "", kind) != 0 || strcmp(manifest["name"] | "", release.name.c_str()) != 0 ||
      strcmp(manifest["version"] | "", release.version.c_str()) != 0)
    return false;
  JsonArrayConst declared = manifest["files"].as<JsonArrayConst>();
  if (declared.isNull() || declared.size() < 2 || declared.size() > MAX_FILES) return false;
  bool readme = false, manifestFound = false;
  files.reserve(declared.size());
  for (JsonVariantConst element : declared) {
    const char* path = element.as<const char*>();
    if (!path || !safeRelative(path)) return false;
    if (std::find(files.begin(), files.end(), path) != files.end()) return false;
    files.emplace_back(path);
    if (strcmp(path, "README.md") == 0) readme = true;
    if (strcmp(path, "manifest.json") == 0) manifestFound = true;
  }
  return readme && manifestFound;
}

bool prepareExisting(const std::string& dest, const std::string& backup) {
  // Recover an interrupted directory swap before doing anything destructive.
  if (!Storage.exists(backup.c_str())) return true;
  if (!Storage.exists(dest.c_str())) return Storage.rename(backup.c_str(), dest.c_str());
  return Storage.removeDir(backup.c_str());
}

}  // namespace

bool supports(const char* capability) {
  if (!capability || !*capability) return false;
  const size_t len = strlen(capability);
  const char* start = firmwareidentity::PLUGIN_CAPABILITIES;
  while (*start) {
    const char* end = strchr(start, ',');
    if (!end) end = start + strlen(start);
    if (static_cast<size_t>(end - start) == len && memcmp(start, capability, len) == 0) return true;
    if (*end == '\0') break;
    start = end + 1;
  }
  return false;
}

bool supportsAll(JsonArrayConst capabilities) {
  if (capabilities.isNull() || capabilities.size() > 32) return false;
  for (JsonVariantConst v : capabilities) {
    const char* required = v.as<const char*>();
    if (!supports(required)) return false;
  }
  return true;
}

bool selectRelease(const std::string& path, const Kind kind, const std::string& collection,
                   const uint16_t width, const uint16_t height, Release& release) {
  if (kind == Kind::Plugin || !safeId(collection)) return false;
  const std::string name = "name";
  const std::string empty;
  Release candidate;
  candidate.kind = kind;
  unsigned matches = 0;
  const size_t variants = kind == Kind::Wallpaper ? MAX_VARIANTS : 1;
  for (size_t i = 0; i < variants; ++i) {
    std::string namePath, assetPath, hashPath, versionPath, widthPath, heightPath, requiresPath;
    if (kind == Kind::Wallpaper) {
      const std::string prefix = "variants." + std::to_string(i) + ".";
      namePath = prefix + "name";
      assetPath = prefix + "asset_url";
      hashPath = prefix + "sha256";
      widthPath = prefix + "target_resolution.width";
      heightPath = prefix + "target_resolution.height";
      versionPath = "version";
    } else {
      namePath = "name";
      assetPath = "asset_url";
      hashPath = "sha256";
      versionPath = "version";
    }
    const std::string* const fields[JsonListParser::F_COUNT] = {
        &name, &namePath, &assetPath, &hashPath, &widthPath, &heightPath};
    JsonListParser::Row row;
    if (!readRow(path, collection, fields, requiresPath, row)) return false;
    if (row.field[JsonListParser::F_AUTHOR].empty()) {
      if (kind == Kind::Wallpaper) break;  // variants are contiguous
      continue;
    }
    if (kind == Kind::Wallpaper) {
      const std::string& w = row.field[JsonListParser::F_VERSION];
      const std::string& h = row.field[JsonListParser::F_BASE];
      if (w.empty() || h.empty() || w.find_first_not_of("0123456789") != std::string::npos ||
          h.find_first_not_of("0123456789") != std::string::npos)
        return false;
      if (w.size() > 5 || h.size() > 5) return false;
      if (strtoul(w.c_str(), nullptr, 10) != width || strtoul(h.c_str(), nullptr, 10) != height) continue;
    }
    if (++matches > 1) return false;
    candidate.name = row.field[JsonListParser::F_AUTHOR];
    candidate.width = width;
    candidate.height = height;
    candidate.url = row.field[JsonListParser::F_ID];
    candidate.sha256 = row.field[JsonListParser::F_URL];
  }
  if (matches != 1) return false;
  if (kind == Kind::Theme) {
    // A bundle may contain several themes with different feature needs.
    // At least one must be supported; the installer rechecks the ZIP manifest.
    bool compatible = false;
    const std::string emptyPath;
    const std::string* const fields[JsonListParser::F_COUNT] = {
        &name, &emptyPath, &emptyPath, &emptyPath, &emptyPath, &emptyPath};
    for (size_t i = 0; i < 16; ++i) {
      const std::string requiresPath = "themes." + std::to_string(i) + ".requires";
      JsonListParser::Row theme;
      if (!readRow(path, collection, fields, requiresPath, theme)) return false;
      if (theme.files.empty()) break;
      bool supported = true;
      for (const auto& cap : theme.files)
        if (!supports(cap.c_str())) supported = false;
      if (supported) compatible = true;
    }
    if (!compatible) return false;
  }
  // Read release version from the same indexed collection, not from the ZIP name.
  const std::string versionPath = "version";
  const std::string* const versionFields[JsonListParser::F_COUNT] = {
      &name, &versionPath, &empty, &empty, &empty, &empty};
  JsonListParser::Row versionRow;
  if (!readRow(path, collection, versionFields, empty, versionRow)) return false;
  candidate.version = versionRow.field[JsonListParser::F_AUTHOR];
  if (!validRelease(candidate)) return false;
  release = std::move(candidate);
  return true;
}

bool installVerifiedZip(const std::string& archivePath, const Release& release) {
  if (!validRelease(release) || !digestMatches(archivePath, release.sha256)) {
    LOG_ERR("EXT", "Package SHA-256 or release metadata invalid: %s", release.name.c_str());
    return false;
  }
  ZipFile zip(archivePath);
  JsonDocument manifest;
  std::vector<std::string> files;
  if (!readManifest(zip, release, manifest, files)) return false;
  const std::string top = release.name +
      (release.kind == Kind::Theme ? ".crosspoint-theme/" : ".crosspoint-wallpaper/");
  if (release.kind == Kind::Wallpaper) {
    const JsonArrayConst images = manifest["images"].as<JsonArrayConst>();
    const auto resolution = manifest["target_resolution"];
    if (images.isNull() || images.size() == 0 || images.size() > 32 ||
        resolution["width"].as<int>() != release.width || resolution["height"].as<int>() != release.height)
      return false;
    if (files.size() != images.size() + 2) return false;
    for (JsonVariantConst value : images) {
      const char* image = value.as<const char*>();
      if (!image || strncmp(image, "wallpapers/", 11) != 0 ||
          strlen(image) < 16 || strcmp(image + strlen(image) - 4, ".bmp") != 0 ||
          std::find(files.begin(), files.end(), image) == files.end()) return false;
    }
  } else {
    const JsonArrayConst themes = manifest["themes"].as<JsonArrayConst>();
    if (themes.isNull() || themes.size() == 0 || themes.size() > 16) return false;
    bool compatible = false;
    for (JsonVariantConst entry : themes) {
      const char* id = entry["id"] | "";
      const char* path = entry["path"] | "";
      if (!safeId(id) || strcmp(path, (std::string("themes/") + id + "/theme.json").c_str()) != 0 ||
          std::find(files.begin(), files.end(), path) == files.end()) return false;
      if (supportsAll(entry["requires"].as<JsonArrayConst>())) compatible = true;
    }
    if (!compatible) return false;
  }
  size_t seen = 0;
  bool validEntries = true;
  const bool scanned = zip.enumerateFilePaths([&](std::string_view entry) {
    ++seen;
    if (entry.size() < top.size() || entry.compare(0, top.size(), top) != 0) {
      validEntries = false;
      return;
    }
    const std::string relative(entry.substr(top.size()));
    if (!safeRelative(relative) || std::find(files.begin(), files.end(), relative) == files.end())
      validEntries = false;
  });
  if (!scanned || !validEntries || seen != files.size()) return false;
  size_t expanded = 0;
  for (const auto& path : files) {
    size_t bytes = 0;
    if (!zip.getInflatedFileSize((top + path).c_str(), &bytes) || bytes > MAX_FILE_BYTES ||
        expanded > MAX_EXPANDED_BYTES - bytes)
      return false;
    expanded += bytes;
  }

  const std::string dest = destination(release);
  const std::string temp = dest + ".installing";
  const std::string backup = dest + ".backup";
  const size_t cut = dest.find_last_of('/');
  const std::string root = dest.substr(0, cut);
  if (!Storage.ensureDirectoryExists(root.c_str()) || !prepareExisting(dest, backup) ||
      !hasRoomForPackage(release)) return false;
  if (Storage.exists(temp.c_str()) && !Storage.removeDir(temp.c_str())) return false;
  if (!Storage.mkdir(temp.c_str())) return false;
  bool allWritten = true;
  for (const auto& relative : files) {
    const std::string full = temp + "/" + relative;
    const size_t slash = full.find_last_of('/');
    if (!Storage.ensureDirectoryExists(full.substr(0, slash).c_str())) {
      allWritten = false;
      break;
    }
    HalFile out;
    if (!Storage.openFileForWrite("EXT", full, out) ||
        !zip.readFileToStream((top + relative).c_str(), out, 1024)) {
      allWritten = false;
      break;
    }
    out.flush();
    out.close();
    if (release.kind == Kind::Wallpaper && relative.rfind("wallpapers/", 0) == 0) {
      HalFile image;
      if (!Storage.openFileForRead("EXT", full, image)) {
        allWritten = false;
        break;
      }
      Bitmap bitmap(image);
      if (bitmap.parseHeaders() != BmpReaderError::Ok ||
          bitmap.getWidth() != release.width || bitmap.getHeight() != release.height) {
        allWritten = false;
        break;
      }
    }
  }
  if (!allWritten) {
    Storage.removeDir(temp.c_str());
    return false;
  }

  bool hadPrior = Storage.exists(dest.c_str());
  if (hadPrior && !Storage.rename(dest.c_str(), backup.c_str())) {
    Storage.removeDir(temp.c_str());
    return false;
  }
  if (!Storage.rename(temp.c_str(), dest.c_str())) {
    if (hadPrior) Storage.rename(backup.c_str(), dest.c_str());
    Storage.removeDir(temp.c_str());
    return false;
  }
  if (hadPrior) Storage.removeDir(backup.c_str());
  LOG_INF("EXT", "Installed %s package %s", release.kind == Kind::Theme ? "theme" : "wallpaper",
          release.name.c_str());
  return true;
}

}  // namespace extensionpackages
