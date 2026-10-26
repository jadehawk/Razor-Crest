#include "ThemeScene.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "ThemeFonts.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace {
constexpr size_t MAX_SCENE_JSON = 12288;
constexpr const char* REGION_NAMES[] = {"header", "home", "boot", "sleep", "hints"};

// These are generic visual primitives driven by the selected theme's style values.
void drawChamfer(const GfxRenderer& renderer, int x, int y, int w, int h, int cut, bool black, bool filled) {
  if (w <= 0 || h <= 0) return;
  cut = std::max(0, std::min({cut, w / 3, h / 3}));
  if (!cut) {
    if (filled)
      renderer.fillRect(x, y, w, h, black);
    else
      renderer.drawRect(x, y, w, h, black);
    return;
  }
  const int xs[6] = {x + cut, x + w - 1, x + w - 1, x + w - 1 - cut, x, x};
  const int ys[6] = {y, y, y + h - 1 - cut, y + h - 1, y + h - 1, y + cut};
  if (filled)
    renderer.fillPolygon(xs, ys, 6, black);
  else
    for (int i = 0; i < 6; ++i) {
      const int next = (i + 1) % 6;
      renderer.drawLine(xs[i], ys[i], xs[next], ys[next], black);
    }
}

void paintIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const bool black) {
  constexpr int size = 32;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const uint8_t value = bitmap[row * 4 + col / 8];
      if (!(value & (0x80u >> (col & 7)))) renderer.drawPixel(x + size - 1 - row, y + col, black);
    }
  }
}

std::string upperAscii(const std::string& original) {
  std::string result = original;
  for (char& character : result) {
    if (character >= 'a' && character <= 'z') character -= ('a' - 'A');
  }
  return result;
}

bool safeResource(const char* path, const std::string& id) {
  if (!path || strlen(path) > 150 || strstr(path, "..") || strstr(path, "//") || strchr(path, '\\') ||
      strchr(path, ':'))
    return false;
  const std::string own = "themes/" + id + "/assets/";
  const bool privateAsset = strncmp(path, own.c_str(), own.size()) == 0;
  const bool shared = strncmp(path, "shared/assets/", 14) == 0;
  if (!privateAsset && !shared) return false;
  const char* suffix = strrchr(path, '.');
  if (!suffix || strcmp(suffix, ".cpb") != 0) return false;
  for (const char* p = path; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
          c == '.' || c == '/'))
      return false;
  }
  return true;
}

bool allowed(const char* value, const char* const* possibilities, size_t count) {
  if (!value) return false;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(value, possibilities[i]) == 0) return true;
  return false;
}

bool safeFontPath(const char* path, const std::string& id) {
  if (!path || strlen(path) > 150 || strstr(path, "..") || strstr(path, "//") || strchr(path, '\\') ||
      strchr(path, ':'))
    return false;
  const std::string prefix = "themes/" + id + "/fonts/";
  if (strncmp(path, prefix.c_str(), prefix.size())) return false;
  const char* ext = strrchr(path, '.');
  if (!ext || strcmp(ext, ".cpfont")) return false;
  for (const char* p = path; *p; ++p) {
    const char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
          c == '.' || c == '/'))
      return false;
  }
  return true;
}

bool checkFontHeader(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("THEME", path, file)) return false;
  if (file.fileSize64() < 64 || file.fileSize64() > 200u * 1024u) return false;
  uint8_t header[32];
  if (file.read(header, sizeof(header)) != sizeof(header)) return false;
  constexpr uint8_t magic[] = {'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
  const unsigned version = static_cast<unsigned>(header[8]) | (static_cast<unsigned>(header[9]) << 8);
  return !memcmp(header, magic, sizeof(magic)) && version == 4 && header[12] >= 1 && header[12] <= 4;
}

bool validOpKeys(JsonObjectConst json, const char* kind) {
  const char* const common[] = {"op", "color", "anchor", "offset"};
  const char* const rectangle[] = {"width", "height"};
  const char* const bitmap[] = {"asset", "fit", "paint", "max_width", "max_height"};
  const char* const text[] = {"source", "value", "font", "tracking", "max_width", "plate"};
  const char* const line[] = {"thickness", "orientation"};
  const char* const book[] = {"variant", "badge_asset"};
  const char* const progress[] = {"source", "thickness"};
  const char* const* extra = nullptr;
  size_t count = 0;
  if (!strcmp(kind, "rect")) {
    extra = rectangle;
    count = 2;
  } else if (!strcmp(kind, "bitmap")) {
    extra = bitmap;
    count = 5;
  } else if (!strcmp(kind, "text")) {
    extra = text;
    count = 6;
  } else if (!strcmp(kind, "line")) {
    extra = line;
    count = 2;
  } else if (!strcmp(kind, "book-card")) {
    extra = book;
    count = 2;
  } else if (!strcmp(kind, "progress")) {
    extra = progress;
    count = 2;
  } else if (strcmp(kind, "fill"))
    return false;
  for (JsonPairConst member : json) {
    const char* key = member.key().c_str();
    if (!allowed(key, common, 4) && !allowed(key, extra, count)) return false;
  }
  return true;
}

int anchorPart(const char* anchor, bool x) {
  if (!anchor) return 0;
  if (x) {
    if (strstr(anchor, "right")) return 2;
    if (!strcmp(anchor, "center") || !strcmp(anchor, "top") || !strcmp(anchor, "bottom")) return 1;
  } else {
    if (strstr(anchor, "bottom")) return 2;
    if (!strcmp(anchor, "center") || !strcmp(anchor, "left") || !strcmp(anchor, "right")) return 1;
  }
  return 0;
}

bool checkOffset(JsonVariantConst raw, int& x, int& y) {
  if (raw.isNull()) {
    x = 0;
    y = 0;
    return true;
  }
  JsonArrayConst values = raw.as<JsonArrayConst>();
  if (values.isNull() || values.size() != 2 || !values[0].is<int>() || !values[1].is<int>()) return false;
  x = values[0].as<int>();
  y = values[1].as<int>();
  return x >= -1024 && x <= 1024 && y >= -1024 && y <= 1024;
}

int anchored(int origin, int size, int objectSize, int part, int offset) {
  if (part == 2) return origin + size - objectSize + offset;
  if (part == 1) return origin + (size - objectSize) / 2 + offset;
  return origin + offset;
}

bool isDeclared(JsonArrayConst files, const char* path) {
  if (files.isNull()) return false;
  for (JsonVariantConst member : files) {
    const char* file = member.as<const char*>();
    if (file && strcmp(file, path) == 0) return true;
  }
  return false;
}
}  // namespace

bool ThemeScene::load(const std::string& themeJsonPath) {
  for (auto& region : regions) region.clear();
  cornerCut = 0;
  cardCut = 0;
  rowCut = 0;
  hintCut = 0;
  rowSeparators = false;
  settingsNavigationTabs = false;
  menuDoubleOutline = false;
  menuFooterFile.clear();
  fonts = {};
  const size_t index = themeJsonPath.rfind("/themes/");
  if (index == std::string::npos) return false;
  const std::string base = themeJsonPath.substr(0, index);
  const std::string rest = themeJsonPath.substr(index + 8);
  const size_t slash = rest.find('/');
  if (slash == std::string::npos || rest.substr(slash) != "/theme.json") return false;
  const std::string id = rest.substr(0, slash);

  std::string raw;
  if (!Storage.readFileToString("THEME", themeJsonPath, MAX_SCENE_JSON, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::NestingLimit(7)) != DeserializationError::Ok) return false;
  JsonObjectConst styles = doc["styles"].as<JsonObjectConst>();
  // Metrics-only collections from Stage A have no styles object.
  if (!styles.isNull()) {
    if (styles.size() > 16) return false;
    const char* corners = styles["corners"] | "square";
    if (!strcmp(corners, "chamfer")) {
      if (!styles["chamfer_cut"].is<int>()) return false;
      cornerCut = styles["chamfer_cut"].as<int>();
      if (cornerCut < 0 || cornerCut > 32) return false;
    } else if (strcmp(corners, "square") && strcmp(corners, "rounded")) {
      return false;
    }
    // Per-element cuts are optional. Older theme packages retain their
    // original universal chamfer_cut value.
    const auto getCut = [&](const char* key, int& out) -> bool {
      if (styles[key].isNull()) {
        out = cornerCut;
        return true;
      }
      if (!styles[key].is<int>()) return false;
      out = styles[key].as<int>();
      return out >= 0 && out <= 32;
    };
    if (!getCut("chamfer_card_cut", cardCut) || !getCut("chamfer_row_cut", rowCut) ||
        !getCut("chamfer_hint_cut", hintCut))
      return false;
    if (!styles["row_separators"].isNull() && !styles["row_separators"].is<bool>()) return false;
    rowSeparators = styles["row_separators"] | false;
    if (!styles["settings_navigation"].isNull() && !styles["settings_navigation"].is<const char*>()) return false;
    const char* navigation = styles["settings_navigation"] | "list";
    if (strcmp(navigation, "list") && strcmp(navigation, "tabs")) return false;
    settingsNavigationTabs = strcmp(navigation, "tabs") == 0;
    if (!styles["menu_double_outline"].isNull() && !styles["menu_double_outline"].is<bool>()) return false;
    menuDoubleOutline = styles["menu_double_outline"] | false;
  }
  JsonObjectConst assets = doc["assets"].as<JsonObjectConst>();
  JsonObjectConst rawRegions = doc["regions"].as<JsonObjectConst>();
  if (assets.isNull() || rawRegions.isNull() || assets.size() > 16 || rawRegions.size() > 5) return false;

  std::string manifestRaw;
  if (!Storage.readFileToString("THEME", base + "/manifest.json", MAX_SCENE_JSON, manifestRaw)) return false;
  JsonDocument manifest;
  if (deserializeJson(manifest, manifestRaw, DeserializationOption::NestingLimit(6)) != DeserializationError::Ok)
    return false;
  JsonArrayConst files = manifest["files"].as<JsonArrayConst>();
  if (files.isNull() || files.size() > 64) return false;
  // Additional artwork is declared, validated and cached like every other
  // theme asset. It must be a scalar path under this package.
  const char* footerRole = styles.isNull() ? nullptr : styles["menu_footer_asset"].as<const char*>();
  if (!styles.isNull() && !styles["menu_footer_asset"].isNull() && !footerRole) return false;
  if (footerRole) {
    JsonVariantConst resource = assets[footerRole];
    const char* path = resource.as<const char*>();
    if (!path || !safeResource(path, id) || !isDeclared(files, path)) return false;
    menuFooterFile = base + "/" + path;
  }

  // A theme may request its Settings presentation only when it advertises
  // the corresponding generic firmware capability in its own manifest entry.
  const bool declaredSettingsLayout = !styles.isNull() && !styles["settings_navigation"].isNull();
  bool advertisedSettingsLayout = false;
  bool advertisedFontFamily = false;
  bool advertisedMenuDecor = false;
  JsonArrayConst themeEntries = manifest["themes"].as<JsonArrayConst>();
  if (themeEntries.isNull()) return false;
  for (JsonVariantConst entry : themeEntries) {
    const char* listedId = entry["id"] | "";
    if (strcmp(listedId, id.c_str()) != 0) continue;
    JsonArrayConst required = entry["requires"].as<JsonArrayConst>();
    for (JsonVariantConst cap : required) {
      const char* name = cap.as<const char*>();
      if (name && strcmp(name, "theme.settings-navigation.v1") == 0) advertisedSettingsLayout = true;
      if (name && strcmp(name, "theme.font-family.v1") == 0) advertisedFontFamily = true;
      if (name && strcmp(name, "theme.menu-decoration.v1") == 0) advertisedMenuDecor = true;
    }
    break;
  }
  if (declaredSettingsLayout != advertisedSettingsLayout) return false;
  if ((menuDoubleOutline || !menuFooterFile.empty()) != advertisedMenuDecor) return false;

  JsonObjectConst fontRoles = doc["fonts"].as<JsonObjectConst>();
  if (!doc["fonts"].isNull()) {
    if (fontRoles.isNull()) return false;
    // Optional single-file UI role: one font instance shared across the UI.
    const bool sharedUiFont = fontRoles.size() == 1 && fontRoles["ui"].is<const char*>();
    const JsonObjectConst variants = fontRoles["ui"].as<JsonObjectConst>();
    const bool familyVariants = fontRoles.size() == 1 && !variants.isNull() && variants.size() == 4;
    if (!sharedUiFont && !familyVariants && fontRoles.size() != 4) return false;
    uiFamilyVariants_ = familyVariants;
    const char* roleNames[] = {"ui_small", "ui_regular", "ui_large", "display"};
    for (int i = 0; i < (sharedUiFont ? 1 : 4); ++i) {
      const char* role = sharedUiFont ? "ui" : roleNames[i];
      const char* variantNames[] = {"small", "regular", "large", "display"};
      const char* relative =
          familyVariants ? variants[variantNames[i]].as<const char*>() : fontRoles[role].as<const char*>();
      if (!safeFontPath(relative, id) || !isDeclared(files, relative) || !checkFontHeader(base + "/" + relative)) {
        LOG_ERR("THEME", "Invalid external font role %s", role);
        return false;
      }
      fonts[i] = base + "/" + relative;
    }
  }

  // Nested size variants and the single-file 'ui' role require the newer
  // family capability; older firmware supporting only four legacy roles must
  // reject this package during discovery rather than at render time.
  const bool familyFormat = !fontRoles.isNull() && fontRoles.size() == 1 && !fontRoles["ui"].isNull();
  if (familyFormat != advertisedFontFamily) return false;

  // Validate every referenced resource before accepting a theme, not mid-render.
  for (JsonPairConst asset : assets) {
    JsonVariantConst location = asset.value();
    JsonArrayConst list = location.as<JsonArrayConst>();
    if (list.isNull() && !location.is<const char*>()) return false;
    if (!list.isNull() && (list.size() == 0 || list.size() > 8)) return false;
    const size_t count = list.isNull() ? 1 : list.size();
    for (size_t i = 0; i < count; ++i) {
      const char* path = list.isNull() ? location.as<const char*>() : list[i].as<const char*>();
      if (!safeResource(path, id) || !isDeclared(files, path) || !themebitmap::validate(base + "/" + path)) {
        LOG_ERR("THEME", "Invalid theme asset in %s", themeJsonPath.c_str());
        return false;
      }
    }
  }

  size_t total = 0;
  for (JsonPairConst item : rawRegions) {
    size_t regionIndex = 5;
    for (size_t i = 0; i < 5; ++i)
      if (!strcmp(item.key().c_str(), REGION_NAMES[i])) regionIndex = i;
    if (regionIndex == 5) return false;
    JsonArrayConst list = item.value().as<JsonArrayConst>();
    if (list.isNull() || list.size() > 16 || total + list.size() > 48) return false;
    auto& operations = regions[regionIndex];
    operations.reserve(list.size());
    for (JsonVariantConst value : list) {
      JsonObjectConst json = value.as<JsonObjectConst>();
      const char* kind = json["op"] | "";
      if (!validOpKeys(json, kind)) return false;
      const char* color = json["color"] | "black";
      const char* anchor = json["anchor"] | "top-left";
      const char* const anchors[] = {"top-left", "top",         "top-right", "left",        "center",
                                     "right",    "bottom-left", "bottom",    "bottom-right"};
      if (!allowed(anchor, anchors, 9) || (strcmp(color, "black") && strcmp(color, "white"))) return false;
      Op op{};
      op.black = strcmp(color, "white") != 0;
      op.anchorX = anchorPart(anchor, true);
      op.anchorY = anchorPart(anchor, false);
      if (!checkOffset(json["offset"], op.offsetX, op.offsetY)) return false;
      if (!strcmp(kind, "fill")) {
        op.kind = Kind::Fill;
      } else if (!strcmp(kind, "rect")) {
        op.kind = Kind::Rect;
        if (!json["width"].is<int>() || !json["height"].is<int>()) return false;
        op.maxWidth = json["width"].as<int>();
        op.maxHeight = json["height"].as<int>();
      } else if (!strcmp(kind, "bitmap")) {
        op.kind = Kind::Bitmap;
        const char* role = json["asset"] | "";
        JsonVariantConst ref = assets[role];
        if (ref.isNull()) return false;
        const char* resource =
            ref.is<JsonArrayConst>() ? ref.as<JsonArrayConst>()[0].as<const char*>() : ref.as<const char*>();
        if (!safeResource(resource, id)) return false;
        op.data = base + "/" + resource;
        if (ref.is<JsonArrayConst>()) {
          for (JsonVariantConst member : ref.as<JsonArrayConst>()) {
            const char* item = member.as<const char*>();
            if (!safeResource(item, id)) return false;
            op.gallery.push_back(base + "/" + item);
          }
        }
        const char* mode = json["fit"] | "contain";
        if (!strcmp(mode, "cover"))
          op.fit = themebitmap::Fit::Cover;
        else if (!strcmp(mode, "native"))
          op.fit = themebitmap::Fit::Native;
        else if (strcmp(mode, "contain"))
          return false;
        const char* paint = json["paint"] | "opaque";
        if (strcmp(paint, "opaque") && strcmp(paint, "ink-only")) return false;
        op.inkOnly = !strcmp(paint, "ink-only");
        if (json["max_width"].is<int>()) op.maxWidth = json["max_width"].as<int>();
        if (json["max_height"].is<int>()) op.maxHeight = json["max_height"].as<int>();
      } else if (!strcmp(kind, "text")) {
        op.kind = Kind::Text;
        const bool literal = json["value"].is<const char*>();
        const bool dynamic = json["source"].is<const char*>();
        if (literal == dynamic) return false;
        op.data = literal ? json["value"].as<const char*>() : "";
        op.source = dynamic ? json["source"].as<const char*>() : "";
        const char* const sources[] = {"screen_title",     "date",    "device_name",    "book_title", "book_author",
                                       "progress_percent", "tagline", "loading_percent"};
        if (dynamic && !allowed(op.source.c_str(), sources, 8)) return false;
        if (op.data.size() > 128) return false;
        op.font = json["font"] | "ui_regular";
        if (json["max_width"].is<int>()) op.maxWidth = json["max_width"].as<int>();
        op.tracking = json["tracking"] | 0;
        if (op.tracking < 0 || op.tracking > 10) return false;
        const char* plate = json["plate"] | "none";
        if (!strcmp(plate, "chamfer-black")) {
          op.plate = true;
          op.plateBlack = true;
        } else if (!strcmp(plate, "chamfer-white")) {
          op.plate = true;
          op.plateBlack = false;
        } else if (strcmp(plate, "none"))
          return false;
        const char* const fonts[] = {"ui_small", "ui_regular", "ui_bold", "display"};
        if (!allowed(op.font.c_str(), fonts, 4)) return false;
      } else if (!strcmp(kind, "line")) {
        op.kind = Kind::Line;
        const char* orientation = json["orientation"] | "horizontal";
        if (strcmp(orientation, "horizontal") && strcmp(orientation, "vertical")) return false;
        op.vertical = !strcmp(orientation, "vertical");
        op.thickness = json["thickness"] | 1;
      } else if (!strcmp(kind, "book-card") && regionIndex == 1) {
        op.kind = Kind::BookCard;
        const char* variant = json["variant"] | "outlined";
        if (strcmp(variant, "outlined") && strcmp(variant, "filled") && strcmp(variant, "cover")) return false;
        if (!strcmp(variant, "cover")) op.data = "@native-cover";
        const char* badge = json["badge_asset"] | "";
        if (*badge) {
          if (op.data == "@native-cover") return false;
          JsonVariantConst asset = assets[badge];
          if (!asset.is<const char*>() || !safeResource(asset.as<const char*>(), id)) return false;
          op.data = base + "/" + asset.as<const char*>();
        }
      } else if (!strcmp(kind, "progress")) {
        op.kind = Kind::Progress;
        op.source = json["source"] | "loading_percent";
        if (op.source != "loading_percent" && op.source != "progress_percent") return false;
        op.thickness = json["thickness"] | 4;
      } else {
        return false;
      }
      if (op.maxWidth < 0 || op.maxWidth > 1024 || op.maxHeight < 0 || op.maxHeight > 1024 || op.thickness < 0 ||
          op.thickness > 1024)
        return false;
      operations.push_back(std::move(op));
    }
    total += list.size();
  }
  return true;
}

bool ThemeScene::hasRegion(const Region region) const { return !regions[static_cast<size_t>(region)].empty(); }

bool ThemeScene::draw(const GfxRenderer& renderer, Region region, Rect area, const char* screenTitle,
                      const std::vector<RecentBook>* recentBooks, const int selectorIndex, const Phase phase) const {
  if (area.width <= 0 || area.height <= 0) return false;
  const auto clip = renderer.getClipRect();
  const int left = std::max(clip[0], area.x);
  const int top = std::max(clip[1], area.y);
  const int right = std::min(clip[0] + clip[2], area.x + area.width);
  const int bottom = std::min(clip[1] + clip[3], area.y + area.height);
  if (left >= right || top >= bottom) return true;
  renderer.setClipRect(left, top, right - left, bottom - top);
  bool success = true;
  for (const auto& op : regions[static_cast<size_t>(region)]) {
    if (phase == Phase::Background && op.kind == Kind::BookCard) continue;
    if (phase == Phase::Foreground && op.kind != Kind::BookCard) continue;
    if (op.kind == Kind::Fill) {
      renderer.fillRect(area.x, area.y, area.width, area.height, op.black);
    } else if (op.kind == Kind::Rect) {
      const int width = op.maxWidth == 0 ? area.width : op.maxWidth;
      const int height = op.maxHeight == 0 ? area.height : op.maxHeight;
      const int x = anchored(area.x, area.width, width, op.anchorX, op.offsetX);
      const int y = anchored(area.y, area.height, height, op.anchorY, op.offsetY);
      renderer.fillRect(x, y, width, height, op.black);
    } else if (op.kind == Kind::Bitmap) {
      const int width = op.maxWidth > 0 ? std::min(area.width, op.maxWidth) : area.width;
      const int height = op.maxHeight > 0 ? std::min(area.height, op.maxHeight) : area.height;
      themebitmap::Options options;
      options.x = anchored(area.x, area.width, width, op.anchorX, op.offsetX);
      options.y = anchored(area.y, area.height, height, op.anchorY, op.offsetY);
      options.width = width;
      options.height = height;
      options.fit = op.fit;
      options.inkOnly = op.inkOnly;
      options.inkBlack = op.black;
      const std::string& image = op.gallery.empty() ? op.data : op.gallery[esp_random() % op.gallery.size()];
      if (!themebitmap::draw(renderer, image, options)) {
        success = false;
        break;
      }
    } else if (op.kind == Kind::Text) {
      const char* text = op.data.c_str();
      char temporal[40] = {};
      if (op.source == "screen_title")
        text = screenTitle ? screenTitle : "";
      else if (op.source == "device_name")
        text = BoardConfig::ACTIVE.name;
      else if (op.source == "book_title")
        text = recentBooks && !recentBooks->empty() ? (*recentBooks)[0].title.c_str() : "";
      else if (op.source == "book_author")
        text = recentBooks && !recentBooks->empty() ? (*recentBooks)[0].author.c_str() : "";
      else if (op.source == "date") {
        tm clock{};
        if (SETTINGS.clockHasBeenSynced && halClock.isAvailable() && halClock.localTime(clock) &&
            clock.tm_year + 1900 >= 2025)
          strftime(temporal, sizeof(temporal), "%a %d %b", &clock);
        text = temporal;
      } else if (op.source == "tagline")
        text = "";
      else if (!op.source.empty())
        text = "0%";
      const int font = op.font == "display" && renderer.getFontMap().count(ThemeFonts::DISPLAY_ID)
                           ? ThemeFonts::DISPLAY_ID
                       : op.font == "display"  ? UI_12_FONT_ID
                       : op.font == "ui_small" ? SMALL_FONT_ID
                       : op.font == "ui_bold"  ? UI_12_FONT_ID
                                               : UI_10_FONT_ID;
      const auto style = op.font == "ui_bold" ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int limit = std::max(1, op.maxWidth > 0 ? std::min(area.width, op.maxWidth) : area.width - 24);
      const int reserve = static_cast<int>(strlen(text)) * op.tracking;
      const auto shortened = renderer.truncatedText(font, text, std::max(1, limit - reserve), style);
      const auto title = upperAscii(shortened);
      const int width = renderer.getTextAdvanceX(font, title.c_str(), style, op.tracking);
      const int height = renderer.getLineHeight(font);
      const int y = anchored(area.y, area.height, height, op.anchorY, op.offsetY);
      const int x = anchored(area.x, area.width, width, op.anchorX, op.offsetX);
      if (op.plate && !title.empty())
        drawChamfer(renderer, x - 8, y - 4, width + 16, height + 8, cornerCut, op.plateBlack, true);
      renderer.drawText(font, x, y, title.c_str(), op.black, style, BidiUtils::BidiBaseDir::AUTO, op.tracking);
    } else if (op.kind == Kind::Line) {
      const int thickness = std::max(1, std::min(op.thickness, 12));
      const int x = anchored(area.x, area.width, thickness, op.anchorX, op.offsetX);
      const int y = anchored(area.y, area.height, thickness, op.anchorY, op.offsetY);
      if (op.vertical)
        renderer.fillRect(x, area.y, thickness, area.height, op.black);
      else
        renderer.fillRect(area.x, y, area.width, thickness, op.black);
    } else if (op.kind == Kind::BookCard) {
      // The book data, focus state and actual navigation remain firmware owned.
      const bool hasBook = recentBooks && !recentBooks->empty();
      const bool selected = hasBook && selectorIndex == 0;
      const int width = std::max(1, area.width - 28);
      const int height = std::min(area.height, 66);
      const int x = anchored(area.x, area.width, width, op.anchorX, op.offsetX);
      const int y = anchored(area.y, area.height, height, op.anchorY, op.offsetY);
      const int cut = cardCut;
      if (op.data == "@native-cover") {
        // Featured native thumbnail, title and saved progress; never bake cover
        // artwork into the theme. The theme owns the geometry only.
        const RecentBook* first = hasBook ? &(*recentBooks)[0] : nullptr;
        const int coverHeight = std::min(area.height, 166);
        const int coverY = anchored(area.y, area.height, coverHeight, op.anchorY, op.offsetY);
        const int coverX = x;
        renderer.fillRect(coverX, coverY, width, coverHeight, selected);
        renderer.drawRect(coverX, coverY, width, coverHeight, !selected);
        const Rect imageSlot{coverX + 10, coverY + 9, 102, std::max(1, coverHeight - 18)};
        bool coverDrawn = false;
        if (first && !first->coverBmpPath.empty()) {
          // Use the sized thumb first, then the underlying cached BMP.
          // The second candidate supports legacy paths that do not carry
          // the [HEIGHT] template. Don't generate thumbnails in a render.
          const std::string thumb =
              UITheme::getCoverThumbPath(first->coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
          const std::string candidates[2] = {thumb, first->coverBmpPath};
          for (const auto& candidate : candidates) {
            if (coverDrawn || candidate.empty()) break;
            HalFile file;
            if (!Storage.openFileForRead("HOME", candidate, file)) continue;
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok)
              coverDrawn = BaseTheme::drawCoverThumbFill(renderer, bitmap, imageSlot);
            file.close();
          }
        }
        if (!coverDrawn) {
          // Light card stays light when unfocused; the generic placeholder
          // painted its lower two-thirds solid black and looked selected.
          renderer.fillRect(imageSlot.x, imageSlot.y, imageSlot.width, imageSlot.height, selected);
          paintIcon(renderer, BookIcon, imageSlot.x + (imageSlot.width - 32) / 2,
                    imageSlot.y + (imageSlot.height - 32) / 2, !selected);
        }
        renderer.drawRect(imageSlot.x, imageSlot.y, imageSlot.width, imageSlot.height, !selected);
        const int textX = coverX + 128;
        const int available = std::max(1, width - 146);
        const auto titleText = upperAscii(renderer.truncatedText(
            UI_12_FONT_ID,
            first ? (first->title.empty() ? first->path.c_str() : first->title.c_str()) : tr(STR_NO_OPEN_BOOK),
            available));
        renderer.drawText(UI_12_FONT_ID, textX, coverY + 25, titleText.c_str(), !selected, EpdFontFamily::BOLD);
        if (first) {
          const auto author = renderer.truncatedText(UI_10_FONT_ID, first->author.c_str(), available);
          renderer.drawText(UI_10_FONT_ID, textX, coverY + 58, author.c_str(), !selected);
          // Cached briefly to avoid constructing EPUB metadata on every arrow
          // repaint. Refresh periodically so progress is updated after reading.
          static std::string cachedBook;
          static uint32_t lastRead = 0;
          static int cachedPercent = -1;
          const uint32_t now = millis();
          if (first->path != cachedBook || now - lastRead > 10000u) {
            cachedBook = first->path;
            cachedPercent = loadBookProgress(first->path);
            lastRead = now;
          }
          if (cachedPercent >= 0 && cachedPercent <= 100) {
            char percent[8];
            snprintf(percent, sizeof(percent), "%d%%", cachedPercent);
            // Percentage and bar share the same baseline and never overlap.
            const int barWidth = std::max(28, std::min(available - 58, 206));
            const int barY = coverY + coverHeight - 40;
            renderer.drawRect(textX, barY, barWidth, 12, !selected);
            if (cachedPercent > 0 && barWidth > 4)
              renderer.fillRect(textX + 2, barY + 2, (barWidth - 4) * cachedPercent / 100, 8, !selected);
            renderer.drawText(UI_10_FONT_ID, textX + barWidth + 9, barY - 8, percent, !selected);
          }
        }
        continue;
      }
      drawChamfer(renderer, x, y, width, height, cut, true, true);
      if (selected)
        drawChamfer(renderer, x + 3, y + 3, width - 6, height - 6, cut, false, true);
      else
        drawChamfer(renderer, x + 3, y + 3, width - 6, height - 6, cut, false, false);
      paintIcon(renderer, BookIcon, x + 18, y + (height - 32) / 2, selected);
      const int textX = x + 64;
      const int usable = std::max(1, width - 116);
      const char* heading = hasBook ? tr(STR_CONTINUE_READING) : tr(STR_NO_OPEN_BOOK);
      const auto upper = upperAscii(renderer.truncatedText(UI_10_FONT_ID, heading, usable));
      renderer.drawText(UI_10_FONT_ID, textX, y + 12, upper.c_str(), selected, EpdFontFamily::BOLD);
      const RecentBook* first = hasBook ? &(*recentBooks)[0] : nullptr;
      const char* detail =
          first ? (first->title.empty() ? first->path.c_str() : first->title.c_str()) : tr(STR_START_READING);
      const auto line = renderer.truncatedText(SMALL_FONT_ID, detail, usable);
      renderer.drawText(SMALL_FONT_ID, textX, y + 34, line.c_str(), selected);
      if (selected) {
        const int cx = x + width - 25;
        const int cy = y + height / 2;
        renderer.drawLine(cx - 4, cy - 7, cx + 3, cy, 2, true);
        renderer.drawLine(cx + 3, cy, cx - 4, cy + 7, 2, true);
      } else if (!op.data.empty()) {
        themebitmap::Options badge;
        badge.x = x + width - 50;
        badge.y = y + (height - 32) / 2;
        badge.width = 32;
        badge.height = 32;
        badge.fit = themebitmap::Fit::Native;
        badge.inkOnly = true;
        badge.inkBlack = false;
        if (!themebitmap::draw(renderer, op.data, badge)) {
          success = false;
          break;
        }
      }
    } else if (op.kind == Kind::Progress) {
      const int width = std::max(1, std::min(area.width - 32, 240));
      const int height = std::max(1, std::min(op.thickness, 12));
      const int x = anchored(area.x, area.width, width, op.anchorX, op.offsetX);
      const int y = anchored(area.y, area.height, height, op.anchorY, op.offsetY);
      renderer.drawRect(x, y, width, height, op.black);
    }
  }
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  return success;
}
