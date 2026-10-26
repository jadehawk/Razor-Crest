#include "ThemePackRegistry.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ThemeScene.h"

namespace {
constexpr char ROOT[] = "/.crosspoint/themes";
constexpr size_t MAX_PACKS = 8;
constexpr size_t MAX_CHOICES = 16;
constexpr size_t MAX_FILES = 64;
constexpr size_t MAX_JSON = 12288;

bool validId(const char* id) {
  if (!id || !*id) return false;
  size_t n = 0;
  for (const char* p = id; *p; ++p) {
    const char c = *p;
    if (++n > 32 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && p != id))) return false;
  }
  return true;
}

bool safeRelative(const char* path) {
  if (!path || !*path || *path == '/' || strlen(path) > 150) return false;
  if (strstr(path, "..") || strstr(path, "//") || strchr(path, '\\') || strchr(path, ':')) return false;
  for (const char* p = path; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
          c == '.' || c == '/'))
      return false;
  }
  return true;
}

bool loadJson(const std::string& path, JsonDocument& document) {
  std::string contents;
  if (!Storage.readFileToString("THEME", path, MAX_JSON, contents)) return false;
  return deserializeJson(document, contents, DeserializationOption::NestingLimit(6)) == DeserializationError::Ok;
}

bool supportsThemeCapabilities(JsonArrayConst capabilities) {
  if (capabilities.isNull() || capabilities.size() == 0 || capabilities.size() > 16) return false;
  bool sd = false;
  bool metrics = false;
  for (JsonVariantConst value : capabilities) {
    const char* cap = value.as<const char*>();
    if (!cap) return false;
    if (strcmp(cap, "theme.sd.v1") == 0)
      sd = true;
    else if (strcmp(cap, "theme.metrics.v1") == 0)
      metrics = true;
    else if (strcmp(cap, "theme.images.v1") == 0 || strcmp(cap, "theme.composition.v1") == 0 ||
             strcmp(cap, "theme.fonts.v1") == 0 || strcmp(cap, "theme.font-family.v1") == 0 ||
             strcmp(cap, "theme.settings-navigation.v1") == 0 || strcmp(cap, "theme.book-cover.v1") == 0) {
      // Fixed primitives, bounded bitmap decoder and generic scenes.
    } else
      return false;  // Unknown capabilities must not be accepted optimistically.
  }
  return sd && metrics;
}

bool declared(JsonArrayConst files, const std::string& candidate) {
  for (JsonVariantConst path : files) {
    const char* filename = path.as<const char*>();
    if (filename && candidate == filename) return true;
  }
  return false;
}

enum class MetricType : uint8_t { Integer, Boolean, Ratio };
struct Field {
  const char* name;
  size_t offset;
  MetricType type;
};

#define INT_FIELD(name) {#name, offsetof(ThemeMetrics, name), MetricType::Integer}
#define BOOL_FIELD(name) {#name, offsetof(ThemeMetrics, name), MetricType::Boolean}
#define RATIO_FIELD(name) {#name, offsetof(ThemeMetrics, name), MetricType::Ratio}
const Field FIELDS[] = {
    INT_FIELD(batteryWidth),
    INT_FIELD(batteryHeight),
    INT_FIELD(topPadding),
    INT_FIELD(batteryBarHeight),
    INT_FIELD(headerHeight),
    INT_FIELD(verticalSpacing),
    INT_FIELD(previewPadding),
    INT_FIELD(previewHeightPercent),
    INT_FIELD(contentSidePadding),
    INT_FIELD(listRowHeight),
    INT_FIELD(listWithSubtitleRowHeight),
    INT_FIELD(listRowGap),
    INT_FIELD(listRowRadius),
    INT_FIELD(listInset),
    INT_FIELD(listSidePadding),
    INT_FIELD(listSelectionStyle),
    INT_FIELD(listScrollWidth),
    INT_FIELD(listScrollSide),
    BOOL_FIELD(listTitleBold),
    INT_FIELD(headerSidePadding),
    INT_FIELD(headerUnderlineSize),
    INT_FIELD(headerTitleAlign),
    INT_FIELD(headerBatterySide),
    BOOL_FIELD(headerShowsClock),
    BOOL_FIELD(headerClockCentered),
    INT_FIELD(menuRowHeight),
    INT_FIELD(menuSpacing),
    INT_FIELD(tabSpacing),
    INT_FIELD(tabBarHeight),
    INT_FIELD(coverGridTabBarHeight),
    BOOL_FIELD(tabPillFullSlot),
    INT_FIELD(scrollBarWidth),
    INT_FIELD(scrollBarRightOffset),
    INT_FIELD(homeTopPadding),
    INT_FIELD(homeCoverHeight),
    INT_FIELD(homeCoverTileHeight),
    INT_FIELD(homeRecentBooksCount),
    BOOL_FIELD(homeContinueReadingInMenu),
    INT_FIELD(homeMenuTopOffset),
    INT_FIELD(buttonHintsHeight),
    INT_FIELD(sideButtonHintsWidth),
    INT_FIELD(progressBarHeight),
    INT_FIELD(progressBarMarginTop),
    INT_FIELD(statusBarHorizontalMargin),
    INT_FIELD(statusBarVerticalMargin),
    INT_FIELD(keyboardKeyHeight),
    INT_FIELD(keyboardKeySpacing),
    BOOL_FIELD(keyboardCenteredText),
    INT_FIELD(keyboardVerticalOffset),
    INT_FIELD(keyboardTextFieldWidthPercent),
    INT_FIELD(keyboardWidthPercent),
    RATIO_FIELD(popupTopOffsetRatio),
    INT_FIELD(popupMarginX),
    INT_FIELD(popupMarginY),
    INT_FIELD(popupFrameThickness),
    INT_FIELD(popupCornerRadius),
    BOOL_FIELD(popupTextBold),
    BOOL_FIELD(popupTextInverted),
    INT_FIELD(popupTextBaselineOffsetY),
    INT_FIELD(popupProgressBarHeight),
    BOOL_FIELD(popupProgressDrawOutline),
    BOOL_FIELD(popupProgressClampPercent),
    BOOL_FIELD(popupProgressFillInverted),
    BOOL_FIELD(popupProgressOutlineInverted),
    INT_FIELD(optionPopupItemSpacing),
    INT_FIELD(optionPopupInnerPadding),
    INT_FIELD(optionPopupSelectionVPadding),
    INT_FIELD(optionPopupDialogSideMargin),
    INT_FIELD(textFieldHorizontalPadding),
    INT_FIELD(textFieldNormalThickness),
    INT_FIELD(textFieldCursorThickness),
    INT_FIELD(textFieldLineEndOffset),
    INT_FIELD(controlRadius),
    INT_FIELD(sheetRadius),
    INT_FIELD(capsuleRadius),
};
#undef INT_FIELD
#undef BOOL_FIELD
#undef RATIO_FIELD

bool parseMetrics(const std::string& path, ThemeMetrics& output) {
  JsonDocument doc;
  if (!loadJson(path, doc)) return false;
  if (doc["schema_version"].as<int>() != 1 || strcmp(doc["base"] | "", "classic") != 0) return false;
  JsonObjectConst metrics = doc["metrics"].as<JsonObjectConst>();
  if (metrics.isNull() || metrics.size() > 80) return false;
  JsonObjectConst assets = doc["assets"].as<JsonObjectConst>();
  JsonObjectConst regions = doc["regions"].as<JsonObjectConst>();
  if (assets.isNull() || regions.isNull()) return false;

  ThemeMetrics candidate = BaseMetrics::values;
  for (JsonPairConst member : metrics) {
    const char* key = member.key().c_str();
    const Field* field = nullptr;
    for (const auto& entry : FIELDS) {
      if (strcmp(key, entry.name) == 0) {
        field = &entry;
        break;
      }
    }
    if (!field && strcmp(key, "listSeparators") == 0 && member.value().is<bool>()) continue;
    if (!field) return false;
    void* dest = reinterpret_cast<uint8_t*>(&candidate) + field->offset;
    JsonVariantConst value = member.value();
    if (field->type == MetricType::Boolean) {
      if (!value.is<bool>()) return false;
      *static_cast<bool*>(dest) = value.as<bool>();
    } else if (field->type == MetricType::Ratio) {
      if (!value.is<double>() && !value.is<float>() && !value.is<int>()) return false;
      const float ratio = value.as<float>();
      if (!std::isfinite(ratio) || ratio < 0.0f || ratio > 1.0f) return false;
      *static_cast<float*>(dest) = ratio;
    } else {
      if (!value.is<int>()) return false;
      const int n = value.as<int>();
      const bool signedValue = strcmp(key, "keyboardVerticalOffset") == 0 ||
                               strcmp(key, "popupTextBaselineOffsetY") == 0 ||
                               strcmp(key, "textFieldLineEndOffset") == 0;
      if (n < (signedValue ? -128 : 0) || n > 1024) return false;
      if ((strcmp(key, "listSelectionStyle") == 0 && n > 3) || (strcmp(key, "headerTitleAlign") == 0 && n > 2) ||
          ((strcmp(key, "listScrollSide") == 0 || strcmp(key, "headerBatterySide") == 0) && n > 1))
        return false;
      *static_cast<int*>(dest) = n;
    }
  }
  output = candidate;
  return true;
}
}  // namespace

ThemePackRegistry& ThemePackRegistry::getInstance() {
  static ThemePackRegistry instance;
  return instance;
}

void ThemePackRegistry::scan() {
  installed.clear();
  if (!Storage.ready()) return;
  auto root = Storage.open(ROOT);
  if (!root || !root.isDirectory()) return;
  installed.reserve(MAX_CHOICES);
  size_t packs = 0;
  for (auto directory = root.openNextFile(); directory && packs < MAX_PACKS && installed.size() < MAX_CHOICES;
       directory = root.openNextFile()) {
    if (!directory.isDirectory()) continue;
    char dirname[80] = {};
    directory.getName(dirname, sizeof(dirname));
    const std::string folder(dirname);
    static constexpr char suffix[] = ".crosspoint-theme";
    if (folder.size() <= sizeof(suffix) - 1 ||
        folder.compare(folder.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) != 0)
      continue;
    const std::string pack = folder.substr(0, folder.size() - (sizeof(suffix) - 1));
    if (!validId(pack.c_str())) continue;
    ++packs;

    const std::string base = std::string(ROOT) + "/" + folder;
    JsonDocument manifest;
    if (!loadJson(base + "/manifest.json", manifest) || strcmp(manifest["kind"] | "", "theme-pack") != 0 ||
        strcmp(manifest["name"] | "", pack.c_str()) != 0) {
      LOG_ERR("THEME", "Skip %s: manifest missing, invalid or name mismatch", folder.c_str());
      continue;
    }
    JsonArrayConst files = manifest["files"].as<JsonArrayConst>();
    JsonArrayConst themes = manifest["themes"].as<JsonArrayConst>();
    if (files.isNull() || files.size() < 3 || files.size() > MAX_FILES || themes.isNull() || themes.size() == 0 ||
        themes.size() > 16 || !declared(files, "manifest.json") || !declared(files, "README.md"))
      continue;
    bool safe = true;
    for (JsonVariantConst file : files) {
      if (!safeRelative(file.as<const char*>())) {
        safe = false;
        break;
      }
    }
    if (!safe) continue;
    const char* packTitle = manifest["title"] | pack.c_str();
    if (strlen(packTitle) > 60) continue;
    for (JsonVariantConst value : themes) {
      JsonObjectConst entry = value.as<JsonObjectConst>();
      const char* themeId = entry["id"] | "";
      const char* title = entry["title"] | "";
      const char* path = entry["path"] | "";
      if (!validId(themeId) || !*title || strlen(title) > 60) continue;
      const std::string expected = std::string("themes/") + themeId + "/theme.json";
      if (expected != path || !declared(files, expected)) continue;
      if (!supportsThemeCapabilities(entry["requires"].as<JsonArrayConst>())) {
        LOG_ERR("THEME", "Skip %s/%s: unsupported capabilities", pack.c_str(), themeId);
        continue;
      }
      const std::string filePath = base + "/" + expected;
      ThemeMetrics check{};
      if (!parseMetrics(filePath, check)) {
        LOG_ERR("THEME", "Skip %s/%s: invalid metrics or theme JSON", pack.c_str(), themeId);
        continue;
      }
      ThemeScene scene;
      if (!scene.load(filePath)) {
        LOG_ERR("THEME", "Skip %s/%s: invalid scene, properties or CPB assets", pack.c_str(), themeId);
        continue;
      }
      const std::string id = pack + "/" + themeId;
      bool duplicate = false;
      for (const auto& existing : installed) {
        if (existing.id == id) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) continue;
      // themes[].title is the publisher-supplied display label; collection
      // identity remains the stable pack/theme id, never part of UI text.
      installed.push_back({id, title, filePath});
      if (installed.size() == MAX_CHOICES) break;
    }
  }
  LOG_INF("THEME", "Found %u compatible SD themes", static_cast<unsigned>(installed.size()));
}

bool ThemePackRegistry::loadMetrics(const char* id, ThemeMetrics& out, std::string* definitionPath) const {
  if (!id || !*id) return false;
  for (const auto& entry : installed) {
    if (entry.id == id) {
      if (!parseMetrics(entry.path, out)) return false;
      if (definitionPath) *definitionPath = entry.path;
      return true;
    }
  }
  return false;
}
