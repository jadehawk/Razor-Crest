#include "HomeMenuRegistry.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/plugins/StatsDashboardActivity.h"
#include "util/HomeMenuGeometry.h"
#include "util/PluginLocations.h"

namespace {
constexpr const char* kPreferencesPath = "/.crosspoint/home-menu.json";
constexpr size_t kMaxPreferencesSize = 4096;
constexpr size_t kMaxPluginShortcuts = 10;
bool sameId(const HomeMenuRegistry::Entry& e, const std::string& id) { return e.id == id; }
}  // namespace

int HomeMenuRegistry::capacityFor(const GfxRenderer& renderer, const int menuTop, const int rowHeight,
                                  const int rowSpacing, const int hintsHeight, const int footerHeight) {
  return home_menu::capacityFor(renderer.getScreenHeight(), menuTop, rowHeight, rowSpacing, hintsHeight, footerHeight);
}

void HomeMenuRegistry::load(const int capacity, const bool pluginsEnabled, const bool opdsAvailable) {
  capacity_ = std::clamp(capacity, 1, 12);
  entries_.clear();
  entries_.push_back({"browse", I18N.get(StrId::STR_BROWSE_FILES), "", Target::Browse, Folder, true});
  entries_.push_back({"library", I18N.get(StrId::STR_LIBRARY), "", Target::Library, Library, true});
  if (pluginsEnabled)
    entries_.push_back({"plugins", I18N.get(StrId::STR_PLUGINS), "", Target::Plugins, Plugins, true});
  else if (opdsAvailable)
    entries_.push_back({"opds", I18N.get(StrId::STR_OPDS_BROWSER), "", Target::Opds, Plugins, true});
  entries_.push_back({"transfer", I18N.get(StrId::STR_FILE_TRANSFER), "", Target::Transfer, Transfer, true});

  if (pluginsEnabled) {
    for (const auto& install : PluginLocations::scanPlugins()) {
      if (entries_.size() >= kMaxPluginShortcuts + 4) break;
      if (!install.hasManifest || !install.hasDevice) continue;
      // A shortcut comes only from an installed, discoverable plugin directory.
      const std::string manifest = install.dir + "/manifest.json";
      const std::string device = install.dir + "/device.json";

      std::string raw;
      if (!Storage.readFileToString("HOME", manifest, kMaxPreferencesSize, raw)) continue;
      JsonDocument doc;
      if (deserializeJson(doc, raw) != DeserializationError::Ok) continue;
      JsonObjectConst shortcut = doc["home_shortcut"].as<JsonObjectConst>();
      const char* title = shortcut["label"].as<const char*>();
      const char* target = shortcut["target"].as<const char*>();
      const char* name = doc["name"].as<const char*>();
      if (!title || !target || !name || strlen(title) > 48 || !*title || strcmp(target, "dashboard") != 0 ||
          install.name != name)
        continue;
      if (!StatsDashboardActivity::supportsManifest(device)) continue;
      const char* icon = shortcut["icon"].as<const char*>();
      const UIIcon artwork = icon && strcmp(icon, "stats") == 0     ? Blocks
                             : icon && strcmp(icon, "library") == 0 ? Library
                             : icon && strcmp(icon, "folder") == 0  ? Folder
                                                                    : Book;
      entries_.push_back({std::string("plugin:") + install.name, title, device, Target::Dashboard, artwork, true});
    }
  }
  entries_.push_back({"settings", I18N.get(StrId::STR_SETTINGS_TITLE), "", Target::Settings, Settings, true});

  // Unconfigured plugins receive an automatic slot when one exists. Otherwise
  // they remain visible in Home Menu Buttons but not on Home itself.
  int defaultSlots = capacity_ - 1;
  for (auto& entry : entries_) {
    if (entry.target == Target::Settings) continue;
    entry.enabled = defaultSlots-- > 0;
  }

  std::string raw;
  if (!Storage.readFileToString("HOME", kPreferencesPath, kMaxPreferencesSize, raw)) return;
  JsonDocument saved;
  if (deserializeJson(saved, raw) != DeserializationError::Ok) return;
  JsonArrayConst ordered = saved["buttons"].as<JsonArrayConst>();
  if (ordered.isNull()) return;
  std::vector<Entry> result;
  result.reserve(entries_.size());
  for (JsonVariantConst element : ordered) {
    const char* id = element["id"].as<const char*>();
    if (!id || strlen(id) > 72 || strcmp(id, "settings") == 0) continue;
    auto it = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; });
    if (it == entries_.end() || std::any_of(result.begin(), result.end(), [id](const Entry& e) { return e.id == id; }))
      continue;
    Entry entry = *it;
    if (element["enabled"].is<bool>()) entry.enabled = element["enabled"].as<bool>();
    result.push_back(std::move(entry));
  }
  for (const auto& entry : entries_)
    if (entry.target != Target::Settings &&
        !std::any_of(result.begin(), result.end(), [&](const Entry& e) { return e.id == entry.id; }))
      result.push_back(entry);
  result.push_back(entries_.back());  // Settings is permanently last.
  entries_ = std::move(result);
  // Invalid or oversubscribed preferences must not push rows into the footer.
  int remaining = capacity_ - 1;
  for (auto& entry : entries_) {
    if (entry.target == Target::Settings) break;
    if (entry.enabled && remaining-- <= 0) entry.enabled = false;
  }
}

std::vector<HomeMenuRegistry::Entry> HomeMenuRegistry::visible() const {
  std::vector<Entry> rows;
  rows.reserve(std::min(entries_.size(), static_cast<size_t>(capacity_)));
  for (const auto& entry : entries_) {
    if (entry.target != Target::Settings && entry.enabled && rows.size() < static_cast<size_t>(capacity_ - 1))
      rows.push_back(entry);
  }
  if (!entries_.empty()) rows.push_back(entries_.back());  // Settings always reachable.
  return rows;
}

bool HomeMenuRegistry::save() const {
  JsonDocument doc;
  JsonArray items = doc["buttons"].to<JsonArray>();
  for (const auto& entry : entries_) {
    if (entry.target == Target::Settings) continue;
    JsonObject item = items.add<JsonObject>();
    item["id"] = entry.id;
    item["enabled"] = entry.enabled;
  }
  String json;
  serializeJson(doc, json);
  return json.length() <= kMaxPreferencesSize && Storage.writeFile(kPreferencesPath, json);
}

bool HomeMenuRegistry::setEnabled(const std::string& id, const bool enabled) {
  if (id == "settings") return false;
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return sameId(e, id); });
  if (it == entries_.end()) return false;
  if (enabled && !it->enabled && visible().size() >= static_cast<size_t>(capacity_)) return false;
  it->enabled = enabled;
  return save();
}

bool HomeMenuRegistry::move(const std::string& id, const int direction) {
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return sameId(e, id); });
  if (it == entries_.end() || it->target == Target::Settings) return false;
  const int index = static_cast<int>(it - entries_.begin());
  const int next = index + direction;
  if (next < 0 || next >= static_cast<int>(entries_.size()) - 1) return false;
  std::swap(entries_[index], entries_[next]);
  return save();
}

bool HomeMenuRegistry::reset() {
  // Remove only Home menu preferences, never installed plugins or statistics.
  if (!Storage.remove(kPreferencesPath)) return false;
  return true;
}
