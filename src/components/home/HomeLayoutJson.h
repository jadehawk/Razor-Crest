#pragma once

// Home Layout V1: cold-path, bounded JSON -> native descriptors.
// Parse on theme activation, never from the render task. No executable theme
// content, per-frame allocations, filesystem assumptions or framebuffer use.
#include <ArduinoJson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "components/home/HomeGridActivation.h"
#include "components/home/HomeWidgetReadouts.h"

namespace home_grid::json {
constexpr size_t kMaxLayoutBytes = 8192;
constexpr int kNestingLimit = 8;

enum class Status : uint8_t {
  Ok,
  InvalidJson,
  TooLarge,
  InvalidSchema,
  InvalidLayout,
  MissingResource,
  NoMemory,
};

struct Resources {
  void* owner = nullptr;
  // Only validated, package-declared CPBs and ThemeScene handles are allowed.
  uint16_t (*scene)(void*, const char* name) = nullptr;
  uint16_t (*bitmap)(void*, const char* assetRole) = nullptr;
  // Optional providers may be uninstalled: handle 0 selects the fallback.
  uint16_t (*provider)(void*, const char* pluginId, const char* widgetId) = nullptr;
  home_widgets::Panel opaquePanel = home_widgets::Panel::White;
  uint16_t preferredMenuHeight = 0; // Native HomeMenuRegistry measured once.
};

struct Parsed {
  home_grid::Definition layout{};
  home_widgets::Plan widgets{};
  uint8_t menuRowHeight = 0;
  bool bookCarousel = false;
};
static_assert(sizeof(Parsed) <= 896, "Home JSON descriptor exceeds retained memory envelope");

namespace detail {
inline const char* string(JsonVariantConst value) {
  return value.is<const char*>() ? value.as<const char*>() : nullptr;
}
inline bool number(JsonVariantConst value, int low, int high, int& dest) {
  if (!value.is<int>()) return false;
  const int n = value.as<int>();
  if (n < low || n > high) return false;
  dest = n;
  return true;
}
inline bool maybeNumber(JsonObjectConst obj, const char* key, int low, int high, int& dest) {
  return obj[key].isUnbound() || number(obj[key], low, high, dest);
}
inline bool optionalBool(JsonObjectConst obj, const char* key, bool& dest) {
  if (obj[key].isUnbound()) return true;
  if (!obj[key].is<bool>()) return false;
  dest = obj[key].as<bool>();
  return true;
}
template <size_t N>
inline bool keys(JsonObjectConst obj, const char* const (&allowed)[N]) {
  if (obj.isNull() || obj.size() > N) return false;
  for (JsonPairConst item : obj) {
    bool found = false;
    for (const char* key : allowed)
      if (!std::strcmp(item.key().c_str(), key)) { found = true; break; }
    if (!found) return false;
  }
  return true;
}
inline bool identifier(const char* s, bool dot = false, size_t limit = 32) {
  if (!s || *s < 'a' || *s > 'z') return false;
  size_t len = 0;
  for (; s[len]; ++len) {
    if (len >= limit) return false;
    const char c = s[len];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || (dot && c == '.')))
      return false;
  }
  return len > 0;
}
inline bool same(const char* lhs, const char* rhs) {
  return lhs && rhs && !std::strcmp(lhs, rhs);
}
inline uint16_t metricId(const char* name) {
  constexpr const char* metrics[] = {
      "active_days", "last_7_seconds", "sessions", "total_seconds",
      "qualified_turns", "seconds_per_turn", "latest_day_seconds"};
  for (uint16_t i = 0; i < 7; ++i)
    if (same(name, metrics[i])) return static_cast<uint16_t>(i + 1);
  return 0;
}

inline Status widget(JsonObjectConst json, bool first,
                     const Resources& resources, Parsed& target, uint8_t index) {
  using namespace home_widgets;
  constexpr const char* allowed[] = {
      "type", "id", "panel", "scene", "carousel", "variant", "source",
      "row_height", "overflow", "plugin", "widget", "fallback", "field"};
  if (!keys(json, allowed)) return Status::InvalidSchema;
  const char* type = string(json["type"]);
  if (!type) return Status::InvalidSchema;
  if (!json["id"].isUnbound() && !identifier(string(json["id"]))) return Status::InvalidSchema;
  const char* panel = json["panel"].isUnbound() ? "opaque" : string(json["panel"]);
  if (!panel || (!same(panel, "opaque") && !same(panel, "transparent")))
    return Status::InvalidSchema;
  Widget result;
  result.panel = same(panel, "transparent") ? Panel::Transparent : resources.opaquePanel;
  if (result.panel > Panel::Black) return Status::InvalidLayout;

  // Reject options belonging to other widget types. This is deliberately
  // stricter than simply ignoring unknown fields.
  auto permitted = [&](const char* names) {
    for (JsonPairConst item : json) {
      const char* key = item.key().c_str();
      if (same(key, "type") || same(key, "id") || same(key, "panel")) continue;
      const size_t length = std::strlen(key);
      const char* hit = std::strstr(names, key);
      if (!hit || (hit != names && hit[-1] != '|') || hit[length] != '|') return false;
    }
    return true;
  };

  if (same(type, "native.header")) {
    if (!permitted("") || !first) return Status::InvalidSchema;
    result.kind = Kind::Header;
  } else if (same(type, "theme.scene")) {
    if (!permitted("scene|")) return Status::InvalidSchema;
    const char* id = string(json["scene"]);
    if (!identifier(id, true, 32)) return Status::InvalidSchema;
    result.resource = resources.scene ? resources.scene(resources.owner, id) : 0;
    if (!result.resource) return Status::MissingResource;
    result.kind = Kind::Scene;
  } else if (same(type, "native.book")) {
    if (!permitted("carousel|variant|") || !optionalBool(json, "carousel", target.bookCarousel))
      return Status::InvalidSchema;
    const char* variant = json["variant"].isUnbound() ? "cover" : string(json["variant"]);
    if (!variant || (!same(variant, "cover") && !same(variant, "outlined")))
      return Status::InvalidSchema;
    result.kind = Kind::Book;
  } else if (same(type, "native.shortcuts")) {
    if (!permitted("source|row_height|overflow|") ||
        (!json["source"].isUnbound() && !same(string(json["source"]), "home_shortcuts")) ||
        (!json["overflow"].isUnbound() && !same(string(json["overflow"]), "scroll")))
      return Status::InvalidSchema;
    int height = 0;
    if (!json["row_height"].isUnbound() && !number(json["row_height"], 24, 100, height))
      return Status::InvalidSchema;
    target.menuRowHeight = static_cast<uint8_t>(height);
    result.kind = Kind::Shortcuts;
  } else if (same(type, "native.clock")) {
    if (!permitted("")) return Status::InvalidSchema;
    result.kind = Kind::Clock;
    result.minWidth = 24;
    result.minHeight = 12;
  } else if (same(type, "native.metric")) {
    if (!permitted("field|")) return Status::InvalidSchema;
    result.resource = metricId(string(json["field"]));
    if (!result.resource) return Status::InvalidSchema;
    result.kind = Kind::Metric;
    result.minWidth = 24;
    result.minHeight = 12;
  } else if (same(type, "plugin.widget")) {
    if (!permitted("plugin|widget|fallback|")) return Status::InvalidSchema;
    const char* plugin = string(json["plugin"]);
    const char* slot = string(json["widget"]);
    JsonObjectConst fallback = json["fallback"].as<JsonObjectConst>();
    constexpr const char* fallbackKeys[] = {"type", "panel", "scene"};
    if (!identifier(plugin, true, 64) || !identifier(slot, true, 64) ||
        !keys(fallback, fallbackKeys))
      return Status::InvalidSchema;
    const char* fallbackType = string(fallback["type"]);
    if (!same(fallbackType, "empty") && !same(fallbackType, "theme.scene"))
      return Status::InvalidSchema;
    if (!fallback["panel"].isUnbound()) {
      const char* fallbackPanel = string(fallback["panel"]);
      if (!same(fallbackPanel, "opaque") && !same(fallbackPanel, "transparent"))
        return Status::InvalidSchema;
      result.fallbackPanel = static_cast<uint8_t>(
          same(fallbackPanel, "transparent") ? Panel::Transparent : resources.opaquePanel);
    }
    if (same(fallbackType, "theme.scene")) {
      const char* name = string(fallback["scene"]);
      if (!identifier(name, true, 32)) return Status::InvalidSchema;
      result.fallback = Kind::Scene;
      result.fallbackResource = resources.scene ? resources.scene(resources.owner, name) : 0;
      if (!result.fallbackResource) return Status::MissingResource;
    } else if (!fallback["scene"].isUnbound()) {
      return Status::InvalidSchema;
    }
    result.kind = Kind::Plugin;
    result.resource = resources.provider ? resources.provider(resources.owner, plugin, slot) : 0;
  } else if (same(type, "empty")) {
    if (!permitted("")) return Status::InvalidSchema;
  } else {
    return Status::InvalidSchema;
  }
  if (first != (result.kind == Kind::Header)) return Status::InvalidLayout;
  target.widgets.widgets[index] = result;
  return Status::Ok;
}

inline Status background(JsonObjectConst obj, const Resources& resources, Parsed& target) {
  using namespace home_widgets;
  if (obj.isNull()) return Status::Ok;
  constexpr const char* allowed[] = {"mode", "color", "asset", "fit", "paint", "focus"};
  if (!keys(obj, allowed)) return Status::InvalidSchema;
  const char* mode = string(obj["mode"]);
  if (same(mode, "none")) {
    if (obj.size() != 1) return Status::InvalidSchema;
    return Status::Ok;
  }
  if (same(mode, "solid")) {
    if (obj.size() != 2) return Status::InvalidSchema;
    const char* color = string(obj["color"]);
    if (!same(color, "white") && !same(color, "black")) return Status::InvalidSchema;
    target.widgets.background.mode = same(color, "black") ? BackgroundMode::Black : BackgroundMode::White;
    return Status::Ok;
  }
  if (!same(mode, "image")) return Status::InvalidSchema;
  if (!obj["color"].isUnbound()) return Status::InvalidSchema;
  const char* role = string(obj["asset"]);
  if (!identifier(role, true, 64)) return Status::InvalidSchema;
  const char* fit = obj["fit"].isUnbound() ? "cover" : string(obj["fit"]);
  if (same(fit, "cover")) target.widgets.background.fit = ImageFit::Cover;
  else if (same(fit, "contain")) target.widgets.background.fit = ImageFit::Contain;
  else if (same(fit, "native")) target.widgets.background.fit = ImageFit::Native;
  else return Status::InvalidSchema;
  const char* paint = obj["paint"].isUnbound() ? "opaque" : string(obj["paint"]);
  if (!same(paint, "opaque") && !same(paint, "ink-only")) return Status::InvalidSchema;
  target.widgets.background.inkOnly = same(paint, "ink-only");
  if (!obj["focus"].isUnbound()) {
    JsonArrayConst focus = obj["focus"].as<JsonArrayConst>();
    int x = 50, y = 50;
    if (focus.isNull() || focus.size() != 2 || !number(focus[0], 0, 100, x) ||
        !number(focus[1], 0, 100, y)) return Status::InvalidSchema;
    target.widgets.background.focusX = static_cast<uint8_t>(x);
    target.widgets.background.focusY = static_cast<uint8_t>(y);
  }
  target.widgets.background.bitmap = resources.bitmap ? resources.bitmap(resources.owner, role) : 0;
  if (!target.widgets.background.bitmap) return Status::MissingResource;
  target.widgets.background.mode = BackgroundMode::Bitmap;
  return Status::Ok;
}
} // namespace detail

// Called only after a trusted source has checked the 8192-byte on-SD file cap.
// A single JSON tree exists during activation; no JSON is retained afterward.
inline Status parse(JsonVariantConst root, const Resources& resources, Parsed& output) {
  output = Parsed{};
  if (resources.opaquePanel != home_widgets::Panel::White &&
      resources.opaquePanel != home_widgets::Panel::Black)
    return Status::InvalidSchema;
  const JsonObjectConst doc = root.as<JsonObjectConst>();
  constexpr const char* allowed[] = {"schema_version", "background", "row_gap", "rows", "scene_underlays"};
  if (!detail::keys(doc, allowed) || !doc["schema_version"].is<int>() ||
      doc["schema_version"].as<int>() != 1)
    return Status::InvalidSchema;
  Parsed candidate;
  JsonVariantConst declaredBackground = doc["background"];
  if (!declaredBackground.isUnbound() && !declaredBackground.is<JsonObjectConst>())
    return Status::InvalidSchema;
  Status status = detail::background(declaredBackground.as<JsonObjectConst>(), resources, candidate);
  if (status != Status::Ok) return status;
  int rowGap = 0;
  if (!detail::maybeNumber(doc, "row_gap", 0, 32, rowGap)) return Status::InvalidSchema;
  candidate.layout.rowGap = static_cast<uint8_t>(rowGap);

  JsonArrayConst rows = doc["rows"].as<JsonArrayConst>();
  if (rows.isNull() || rows.size() < 1 || rows.size() > home_grid::kMaxRows)
    return Status::InvalidSchema;
  candidate.layout.rowCount = static_cast<uint8_t>(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    JsonObjectConst rowJson = rows[i].as<JsonObjectConst>();
    constexpr const char* rowKeys[] = {
      "id", "weight", "min_height", "max_height", "column_gap", "size_mode", "columns"};
    if (!detail::keys(rowJson, rowKeys)) return Status::InvalidSchema;
    const char* rowId = detail::string(rowJson["id"]);
    if (!detail::identifier(rowId)) return Status::InvalidSchema;
    for (size_t j = 0; j < i; ++j)
      if (detail::same(rowId, detail::string(rows[j]["id"]))) return Status::InvalidSchema;
    home_grid::Row& row = candidate.layout.rows[i];
    int weight = 0, minHeight = 0, maxHeight = 0, gap = 0;
    if (!detail::number(rowJson["weight"], 1, 10, weight) ||
        !detail::number(rowJson["min_height"], 1, home_grid::kMaxScreenSize, minHeight) ||
        !detail::maybeNumber(rowJson, "max_height", minHeight, home_grid::kMaxScreenSize, maxHeight) ||
        !detail::maybeNumber(rowJson, "column_gap", 0, 32, gap))
      return Status::InvalidSchema;
    row.weight = static_cast<uint16_t>(weight);
    row.minHeight = static_cast<uint16_t>(minHeight);
    row.maxHeight = static_cast<uint16_t>(maxHeight);
    row.columnGap = static_cast<uint8_t>(gap);
    const char* mode = rowJson["size_mode"].isUnbound() ? "weighted" : detail::string(rowJson["size_mode"]);
    if (detail::same(mode, "weighted")) row.mode = home_grid::RowMode::Weighted;
    else if (detail::same(mode, "fixed")) {
      if (!maxHeight || maxHeight != minHeight) return Status::InvalidLayout;
      row.mode = home_grid::RowMode::Fixed;
    } else if (detail::same(mode, "content")) {
      if (!maxHeight) return Status::InvalidLayout;
      row.mode = home_grid::RowMode::Content;
      row.preferredHeight = static_cast<uint16_t>(
          std::max(minHeight, std::min(maxHeight, static_cast<int>(resources.preferredMenuHeight))));
    } else return Status::InvalidSchema;
    if (i == 0 && minHeight < 40) return Status::InvalidLayout;

    JsonArrayConst columns = rowJson["columns"].as<JsonArrayConst>();
    if (columns.isNull() || columns.size() < 1 ||
        columns.size() > home_grid::kMaxColumnsPerRow ||
        candidate.layout.columnCount + columns.size() > home_grid::kMaxWidgets ||
        (i == 0 && columns.size() != 1))
      return Status::InvalidLayout;
    row.firstColumn = candidate.layout.columnCount;
    row.columnCount = static_cast<uint8_t>(columns.size());
    for (size_t j = 0; j < columns.size(); ++j) {
      JsonObjectConst col = columns[j].as<JsonObjectConst>();
      constexpr const char* colKeys[] = {"id", "weight", "min_width", "max_width", "widget"};
      if (!detail::keys(col, colKeys)) return Status::InvalidSchema;
      const char* colId = detail::string(col["id"]);
      if (!detail::identifier(colId)) return Status::InvalidSchema;
      for (size_t k = 0; k < j; ++k)
        if (detail::same(colId, detail::string(columns[k]["id"]))) return Status::InvalidSchema;
      home_grid::Column& nativeCol = candidate.layout.columns[candidate.layout.columnCount];
      int colWeight = 0, minWidth = 0, maxWidth = 0;
      if (!detail::number(col["weight"], 1, 10, colWeight) ||
          !detail::number(col["min_width"], 1, home_grid::kMaxScreenSize, minWidth) ||
          !detail::maybeNumber(col, "max_width", minWidth, home_grid::kMaxScreenSize, maxWidth))
        return Status::InvalidSchema;
      nativeCol.weight = static_cast<uint16_t>(colWeight);
      nativeCol.minWidth = static_cast<uint16_t>(minWidth);
      nativeCol.maxWidth = static_cast<uint16_t>(maxWidth);
      const bool isHeader = i == 0 && j == 0;
      status = detail::widget(col["widget"].as<JsonObjectConst>(), isHeader,
                              resources, candidate, candidate.layout.columnCount);
      if (status != Status::Ok) return status;
      if (row.mode == home_grid::RowMode::Content &&
          candidate.widgets.widgets[candidate.layout.columnCount].kind !=
              home_widgets::Kind::Shortcuts)
        return Status::InvalidLayout;
      ++candidate.layout.columnCount;
    }
    if (row.mode == home_grid::RowMode::Content && columns.size() != 1)
      return Status::InvalidLayout;
  }
  candidate.widgets.widgetCount = candidate.layout.columnCount;

  JsonVariantConst underlayField = doc["scene_underlays"];
  if (!underlayField.isUnbound()) {
    JsonArrayConst layers = underlayField.as<JsonArrayConst>();
    if (layers.isNull() || layers.size() > home_widgets::kMaxUnderlays)
      return Status::InvalidSchema;
    uint16_t usedRows = 0;
    for (JsonVariantConst layerValue : layers) {
      JsonObjectConst layer = layerValue.as<JsonObjectConst>();
      constexpr const char* layerKeys[] = {"scene", "from_row", "through_row"};
      if (!detail::keys(layer, layerKeys) || layer.size() != 3)
        return Status::InvalidSchema;
      const char* name = detail::string(layer["scene"]);
      const char* from = detail::string(layer["from_row"]);
      const char* through = detail::string(layer["through_row"]);
      if (!detail::identifier(name, true, 32) ||
          !detail::identifier(from) || !detail::identifier(through))
        return Status::InvalidSchema;
      int first = -1, last = -1;
      for (size_t i = 0; i < rows.size(); ++i) {
        const char* id = detail::string(rows[i]["id"]);
        if (detail::same(id, from)) first = static_cast<int>(i);
        if (detail::same(id, through)) last = static_cast<int>(i);
      }
      if (first < 1 || last < first || last >= candidate.layout.rowCount - 1)
        return Status::InvalidLayout;
      for (int index = first; index <= last; ++index) {
        const auto& row = candidate.layout.rows[index];
        if ((usedRows & (1u << index)) || row.columnCount != 1 ||
            candidate.widgets.widgets[row.firstColumn].kind == home_widgets::Kind::Shortcuts)
          return Status::InvalidLayout;
        usedRows |= static_cast<uint16_t>(1u << index);
      }
      uint16_t scene = resources.scene ? resources.scene(resources.owner, name) : 0;
      if (!scene) return Status::MissingResource;
      candidate.widgets.underlays[candidate.widgets.underlayCount++] = {
        static_cast<uint8_t>(first), static_cast<uint8_t>(last), scene};
    }
  }
  output = candidate;
  return Status::Ok;
}

// Observe the raw stream before ArduinoJson coalesces duplicate object keys.
// Keys in this schema are plain ASCII; escaped key spellings are rejected.
// One FNV32 hash per allowed key (max 13), for each nesting level (max 8).
// A hash collision fails closed rather than allowing a duplicate through.
struct RawKeyGuard {
  struct Frame {
    uint32_t hashes[13]{};
    uint8_t count = 0;
    bool object = false;
    bool expectsKey = false;
  };
  Frame frames[kNestingLimit]{};
  uint8_t depth = 0;
  bool inString = false;
  bool escaped = false;
  bool keyString = false;
  bool invalid = false;
  uint32_t hash = 2166136261u;

  void observe(unsigned char c) {
    if (inString) {
      if (escaped) {
        if (keyString) invalid = true;
        escaped = false;
        return;
      }
      if (c == '\\') { escaped = true; return; }
      if (c == '"') {
        if (keyString && depth) {
          auto& frame = frames[depth - 1];
          if (frame.count == 13) invalid = true;
          for (int i = 0; i < frame.count; ++i)
            if (frame.hashes[i] == hash) invalid = true;
          if (frame.count < 13) frame.hashes[frame.count++] = hash;
          frame.expectsKey = false;
        }
        inString = false;
        keyString = false;
        return;
      }
      if (keyString) hash = (hash ^ c) * 16777619u;
      return;
    }
    if (c == '"') {
      inString = true;
      keyString = depth && frames[depth - 1].object && frames[depth - 1].expectsKey;
      hash = 2166136261u;
    } else if (c == '{' || c == '[') {
      if (depth >= kNestingLimit) { invalid = true; return; }
      auto& frame = frames[depth++];
      frame = Frame{};
      frame.object = c == '{';
      frame.expectsKey = frame.object;
    } else if (c == '}' || c == ']') {
      if (!depth) { invalid = true; return; }
      if (frames[depth - 1].object != (c == '}')) invalid = true;
      --depth;
    } else if (c == ',' && depth && frames[depth - 1].object) {
      frames[depth - 1].expectsKey = true;
    }
  }

  bool valid() const { return !invalid && !inString && depth == 0; }
};
static_assert(sizeof(RawKeyGuard) <= 600, "Raw JSON key guard exceeds cold-path stack budget");

// Supplies a reader exposing read()/readBytes() (ArduinoJson's native Reader
// protocol). Limits bytes even if the source itself is untrusted.
template <typename Reader>
inline Status parseStream(Reader& source, size_t fileBytes, const Resources& resources,
                          Parsed& output) {
  output = Parsed{};
  if (fileBytes == 0 || fileBytes > kMaxLayoutBytes) return Status::TooLarge;
  struct BoundedReader {
    Reader& input;
    size_t remaining;
    RawKeyGuard keys{};
    int read() {
      if (!remaining) return -1;
      const int c = input.read();
      if (c >= 0) {
        --remaining;
        keys.observe(static_cast<unsigned char>(c));
      }
      return c;
    }
    size_t readBytes(char* buffer, size_t length) {
      if (length > remaining) length = remaining;
      const size_t count = std::min(input.readBytes(buffer, length), length);
      remaining -= count;
      for (size_t i = 0; i < count; ++i)
        keys.observe(static_cast<unsigned char>(buffer[i]));
      return count;
    }
  } reader{source, fileBytes};
  JsonDocument document;
  const DeserializationError error =
      deserializeJson(document, reader, DeserializationOption::NestingLimit(kNestingLimit));
  if (error == DeserializationError::NoMemory) return Status::NoMemory;
  if (error != DeserializationError::Ok) return Status::InvalidJson;
  // ArduinoJson can accept one valid top-level object followed by junk.
  // Consume only whitespace after the root; reject appended executable/text.
  int c;
  while ((c = reader.read()) >= 0)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return Status::InvalidJson;
  if (reader.remaining != 0 || !reader.keys.valid()) return Status::InvalidJson;
  return parse(document.as<JsonVariantConst>(), resources, output);
}

// Cold-path file adapter for the real HalFile interface: fileSize64(),
// read(), and read(void*, size_t). The CALLER must have verified that the
// path is in the active theme's manifest before opening the file. This
// function performs no path-based trust decisions and never opens files.
template <typename File>
inline Status parseFile(File& file, const Resources& resources, Parsed& output) {
  output = Parsed{};
  const uint64_t fileSize = file.fileSize64();
  if (!fileSize || fileSize > kMaxLayoutBytes) return Status::TooLarge;
  struct FileReader {
    File& source;
    int read() { return source.read(); }
    size_t readBytes(char* destination, size_t count) {
      const int result = source.read(destination, count);
      if (result <= 0) return 0;
      return std::min(static_cast<size_t>(result), count);
    }
  } reader{file};
  return parseStream(reader, static_cast<size_t>(fileSize), resources, output);
}
}  // namespace home_grid::json
