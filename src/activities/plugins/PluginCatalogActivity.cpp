#include "PluginCatalogActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonListParser.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <XmlListParser.h>
#include <ZipFile.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <string_view>

#include "CrossPointSettings.h"
#include "JpegToBmpConverter.h"
#include "MappedInputManager.h"
#include "PluginCatalogPaging.h"
#include "PngToBmpConverter.h"
#include "activities/reader/DictionaryDefinitionActivity.h"  // plain-text README viewer
#include "activities/settings/PluginHubInstallActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/DetailsActivity.h"
#include "components/CatalogScreens.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"
#include "components/themes/ThemePackRegistry.h"
#include "components/wallpapers/WallpaperPackRegistry.h"
#include "fontIds.h"
#include "network/GithubReleaseRedirect.h"
#include "network/HttpDownloader.h"
#include "network/ProtectedPaths.h"
#include "util/BookCacheUtils.h"
#include "util/ExtensionPackages.h"
#include "util/FirmwareIdentity.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/PluginPickerLayout.h"
#include "util/QrUtils.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

// Template/JSON/transport primitives shared with the plugin event drain.
using pluginhttp::resolvePath;
using pluginhttp::substituteAll;
using pluginhttp::urlEncodeQuery;
using pluginhttp::variantToString;

namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
// In-DRAM responses (auth, download-url hops) are small; the cap bounds a
// misbehaving server, not normal use.
constexpr size_t MAX_API_RESPONSE = 48 * 1024;
// Browse responses stream to this SD temp file instead of DRAM: one page of
// raw catalog JSON can run 60+ KB (BookFusion inlines heavy per-book
// metadata), and buffering that in a std::string aborts on low heap. Keep the
// response until leaving the catalog so downloads can release the parsed rows.
constexpr char BROWSE_TMP_PATH[] = "/.pcat_tmp.json";
constexpr char PREVIEW_TMP_PATH[] = "/.pcat_img.tmp";
constexpr char FULL_PREVIEW_TMP_BASE[] = "/.pcat_full_preview";
constexpr size_t MAX_BROWSE_RESPONSE = 1024 * 1024;
constexpr size_t MAX_BROWSE_LIST_INDEX_RESPONSE = 16 * 1024;
constexpr char CACHE_SEED_TMP_PATH[] = "/.pcat_cache_seed.zip";
constexpr size_t MAX_PREVIEW_RESPONSE = 256 * 1024;
constexpr size_t MAX_ITEM_DETAILS_RESPONSE = 24 * 1024;
constexpr unsigned long PLUGIN_UNINSTALL_HOLD_MS = 1000;
constexpr int MAX_PAGE_SIZE = 16;
constexpr int GRID_PREVIEW_WIDTH = 200;
constexpr int GRID_PREVIEW_HEIGHT = 300;
constexpr int LIST_PREVIEW_WIDTH = 62;
constexpr int LIST_PREVIEW_HEIGHT = 84;
constexpr int MAX_PLUGIN_TABS = 4;
constexpr int MAX_PLUGIN_SETTINGS = 12;
constexpr int MAX_SETTING_OPTIONS = 6;
constexpr int16_t TOUCH_TAB_BAR_HEIGHT = 50;
constexpr int64_t MIN_VALID_UNIX_TIME = 1609459200LL;  // 2021-01-01

int64_t currentUnixTime() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  return now >= MIN_VALID_UNIX_TIME ? now : 0;
}

// JsonListParser has six generic field slots. The compatibility and details
// passes store other fields in the slots they do not need.
constexpr auto COMPAT_FAMILY = JsonListParser::F_AUTHOR;
constexpr auto COMPAT_API_MIN = JsonListParser::F_ID;
constexpr auto COMPAT_REQUIRES = JsonListParser::F_URL;
constexpr auto DETAIL_DESCRIPTION = JsonListParser::F_URL;
constexpr auto DETAIL_REPOSITORY = JsonListParser::F_BASE;
constexpr auto DETAIL_PAGE_URL = JsonListParser::F_URL;
constexpr auto DETAIL_PAGE_FORMAT = JsonListParser::F_BASE;

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  md5.calculate();
  return md5.toString().c_str();
}

bool endsWith(const std::string& value, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  return value.size() >= suffixLen && value.compare(value.size() - suffixLen, suffixLen, suffix) == 0;
}

enum class ImageFileKind : uint8_t { Unknown, Bmp, Jpeg, Png };

std::string lowerExtension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string extension = path.substr(dot);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

bool isImageExtension(const std::string& extension) {
  return extension == ".bmp" || extension == ".jpg" || extension == ".jpeg" || extension == ".png";
}

ImageFileKind detectImageFileKind(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("PCAT", path, file)) return ImageFileKind::Unknown;
  uint8_t signature[8] = {};
  const int bytesRead = file.read(signature, sizeof(signature));
  file.close();
  if (bytesRead >= 2 && signature[0] == 'B' && signature[1] == 'M') return ImageFileKind::Bmp;
  if (bytesRead >= 3 && signature[0] == 0xFF && signature[1] == 0xD8 && signature[2] == 0xFF)
    return ImageFileKind::Jpeg;
  constexpr uint8_t PNG_SIGNATURE[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (bytesRead >= static_cast<int>(sizeof(PNG_SIGNATURE)) &&
      memcmp(signature, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0)
    return ImageFileKind::Png;
  return ImageFileKind::Unknown;
}

const char* imageKindExtension(const ImageFileKind kind) {
  switch (kind) {
    case ImageFileKind::Bmp:
      return ".bmp";
    case ImageFileKind::Jpeg:
      return ".jpg";
    case ImageFileKind::Png:
      return ".png";
    default:
      return "";
  }
}

bool publishImageForDestination(const std::string& sourcePath, const std::string& destination) {
  const std::string extension = lowerExtension(destination);
  const ImageFileKind kind = detectImageFileKind(sourcePath);
  if (kind == ImageFileKind::Unknown) {
    LOG_ERR("PCAT", "unsupported downloaded image: %s", sourcePath.c_str());
    return false;
  }

  if (extension == ".bmp" && kind != ImageFileKind::Bmp) {
    const std::string converted = destination + ".new";
    Storage.remove(converted.c_str());
    HalFile src, dst;
    bool ok = Storage.openFileForRead("PCAT", sourcePath, src) && Storage.openFileForWrite("PCAT", converted, dst);
    if (ok) {
      ok = kind == ImageFileKind::Jpeg ? JpegToBmpConverter::jpegFileToBmpStream(src, dst, false)
                                       : PngToBmpConverter::pngFileToBmpStream(src, dst, false);
    }
    if (src.isOpen()) src.close();
    if (dst.isOpen()) dst.close();
    if (!ok || !Storage.replaceFile(converted.c_str(), destination.c_str())) {
      Storage.remove(converted.c_str());
      LOG_ERR("PCAT", "image conversion failed: %s -> %s", sourcePath.c_str(), destination.c_str());
      return false;
    }
    Storage.remove(sourcePath.c_str());
    return true;
  }

  const bool extensionMatches = (kind == ImageFileKind::Bmp && extension == ".bmp") ||
                                (kind == ImageFileKind::Jpeg && (extension == ".jpg" || extension == ".jpeg")) ||
                                (kind == ImageFileKind::Png && extension == ".png");
  if (!extensionMatches) {
    LOG_ERR("PCAT", "downloaded image format does not match destination: %s -> %s", sourcePath.c_str(),
            destination.c_str());
    return false;
  }
  return Storage.replaceFile(sourcePath.c_str(), destination.c_str());
}

// Returns `override` unless it's empty, in which case `fallback` applies.
const std::string& pick(const std::string& override, const std::string& fallback) {
  return override.empty() ? fallback : override;
}

std::string_view trimCsvToken(std::string_view token) {
  while (!token.empty() && (token.front() == ' ' || token.front() == '	')) token.remove_prefix(1);
  while (!token.empty() && (token.back() == ' ' || token.back() == '	')) token.remove_suffix(1);
  return token;
}

bool csvContainsToken(std::string_view csv, const std::string_view wanted) {
  size_t start = 0;
  while (start <= csv.size()) {
    size_t end = csv.find(',', start);
    if (end == std::string_view::npos) end = csv.size();
    if (trimCsvToken(csv.substr(start, end - start)) == wanted) return true;
    if (end == csv.size()) break;
    start = end + 1;
  }
  return false;
}

bool matchesCsvRestriction(const std::string& restriction, const std::string_view current) {
  return restriction.empty() || csvContainsToken(restriction, current);
}

bool matchesPluginApiMinimum(const std::string& minimum) {
  if (minimum.empty()) return true;
  char* end = nullptr;
  const long parsed = strtol(minimum.c_str(), &end, 10);
  return end != minimum.c_str() && *end == '\0' && parsed >= 0 && parsed <= firmwareidentity::PLUGIN_API_VERSION;
}

bool matchesCapabilities(const std::string& required) {
  if (required.empty()) return true;
  const std::string_view available = firmwareidentity::PLUGIN_CAPABILITIES;
  const std::string_view requested = required;
  size_t start = 0;
  while (start <= requested.size()) {
    size_t end = requested.find(',', start);
    if (end == std::string_view::npos) end = requested.size();
    const std::string_view capability = trimCsvToken(requested.substr(start, end - start));
    if (capability.empty() || !csvContainsToken(available, capability)) return false;
    if (end == requested.size()) break;
    start = end + 1;
  }
  return true;
}

bool catalogEntryCompatible(const std::string& family, const std::string& firmware, const std::string& apiMin,
                            const std::string& requiredCapabilities) {
  return matchesCsvRestriction(family, firmwareidentity::FAMILY) &&
         matchesCsvRestriction(firmware, firmwareidentity::ID) && matchesPluginApiMinimum(apiMin) &&
         matchesCapabilities(requiredCapabilities);
}

// Plugin folder name from its "<root>/<name>/device.json" manifest path.
std::string pluginNameFromManifestPath(const std::string& manifestPath) {
  const size_t slash = manifestPath.rfind('/');
  if (slash == std::string::npos || slash == 0) return "";
  const size_t parent = manifestPath.rfind('/', slash - 1);
  if (parent == std::string::npos) return "";
  return manifestPath.substr(parent + 1, slash - parent - 1);
}

// book.downloaded plugin event, fired after a catalog download lands on SD.
void emitBookDownloaded(const std::string& manifestPath, const std::string& path, const std::string& title) {
  if (!pluginevents::anySubscriber(pluginevents::Event::BookDownloaded)) return;
  const std::string plugin = pluginNameFromManifestPath(manifestPath);
  const pluginevents::Var vars[] = {{"path", path.c_str()}, {"title", title.c_str()}, {"plugin", plugin.c_str()}};
  pluginevents::emit(pluginevents::Event::BookDownloaded, vars, 3);
}

// Streams the browse temp file through a catalog list parser (null on OOM).
// False when the reader cannot start or the document is malformed.
template <typename Parser>
bool streamBrowseFile(Parser* parser, const char* path = BROWSE_TMP_PATH) {
  HalFile file;
  if (!parser || !Storage.openFileForRead("PCAT", path, file)) {
    LOG_ERR("PCAT", "Catalog list reader unavailable");
    return false;
  }
  if (parser->parse([](void* f, char* buf, size_t len) { return static_cast<HalFile*>(f)->read(buf, len); }, &file)) {
    return true;
  }
  LOG_ERR("PCAT", "Catalog list parse error");
  return false;
}

}  // namespace

namespace {
// Reads picker metadata, and classifies device manifests without making an
// events-only plugin look like an invalid catalog.
void readPluginMetadata(const std::string& path, PluginRef& ref, const bool classifyDevice = false) {
  std::string raw;
  if (!Storage.readFileToString("PCAT", path, MAX_MANIFEST_SIZE, raw)) return;
  JsonDocument filter;
  filter["title"] = true;
  filter["version"] = true;
  filter["description"] = true;
  if (classifyDevice) {
    filter["browse"]["url"] = true;
    filter["events"] = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return;
  if (doc["title"].is<const char*>()) ref.title = doc["title"].as<const char*>();
  if (doc["version"].is<const char*>()) {
    const char* version = doc["version"].as<const char*>();
    if (version && *version) {
      ref.version = version;
      if (ref.version.front() != 'v' && ref.version.front() != 'V') ref.version.insert(ref.version.begin(), 'v');
    }
  }
  if (doc["description"].is<const char*>()) ref.description = doc["description"].as<const char*>();
  if (classifyDevice) {
    const char* browseUrl = doc["browse"]["url"] | "";
    const bool hasEvents = !doc["events"].as<JsonObjectConst>().isNull() && doc["events"].size() > 0;
    ref.deviceKind = PluginLocations::classifyDeviceManifest(browseUrl[0] != '\0', hasEvents);
  }
}
}  // namespace

std::vector<PluginRef> discoverPlugins() {
  const auto entries = PluginLocations::scanPlugins();
  std::vector<PluginRef> plugins;
  plugins.reserve(entries.size());
  for (const auto& e : entries) {
    PluginRef ref;
    ref.name = e.name;
    ref.dir = e.dir;
    ref.title = e.name;
    // Browser-only plugins (no device.json) stay listed so an install is
    // visibly installed, but carry no manifest to open (empty manifestPath).
    if (e.hasDevice) ref.manifestPath = e.dir + "/device.json";
    const std::string readmePath = e.dir + "/README.md";
    if (Storage.exists(readmePath.c_str())) ref.readmePath = readmePath;
    // manifest.json first, then device.json overrides (on-device authority).
    if (e.hasManifest) readPluginMetadata(e.dir + "/manifest.json", ref);
    if (e.hasDevice) readPluginMetadata(ref.manifestPath, ref, true);
    plugins.push_back(std::move(ref));
  }
  std::sort(plugins.begin(), plugins.end(), [](const PluginRef& lhs, const PluginRef& rhs) {
    const bool lhsHub = lhs.name == "pluginhub";
    const bool rhsHub = rhs.name == "pluginhub";
    if (lhsHub != rhsHub) return lhsHub;
    const int titleOrder = strcasecmp(lhs.title.c_str(), rhs.title.c_str());
    return titleOrder != 0 ? titleOrder < 0 : strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
  });
  return plugins;
}

bool PluginCatalogActivity::loadManifest() {
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifestPath, MAX_MANIFEST_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;

  const std::string pluginDir = manifestPath.substr(0, manifestPath.rfind('/'));
  manifest.tokenFile = pluginhttp::inPluginDir(pluginDir, doc["token"]["file"] | "");
  manifest.tokenPath = doc["token"]["path"] | "token";
  manifest.tokenPersistent = doc["token"]["persist"] | true;
  manifest.configFile = pluginhttp::inPluginDir(pluginDir, doc["config"]["file"] | "");

  JsonVariantConst browse = doc["browse"];
  manifest.browseFormat = browse["format"] | "json";
  pluginhttp::readRequest(browse, "GET", manifest.browseReq);
  manifest.itemsPath = browse["items"] | "";
  manifest.titlePath = browse["fields"]["title"] | (manifest.isXmlList() ? "" : "title");
  manifest.authorPath = browse["fields"]["author"] | "";
  manifest.idPath = browse["fields"]["id"] | "";
  manifest.urlPath = browse["fields"]["url"] | "";
  manifest.thumbnailPath = browse["fields"]["thumbnail"] | "";
  manifest.versionPath = browse["fields"]["version"] | "";
  manifest.descriptionPath = browse["fields"]["description"] | "";
  manifest.detailAuthorPath = browse["fields"]["detail_author"] | "";
  if (manifest.detailAuthorPath.empty()) manifest.detailAuthorPath = manifest.authorPath;
  manifest.repositoryPath = browse["fields"]["repository"] | "";
  manifest.detailsUrlPath = browse["fields"]["details_url"] | "";
  manifest.detailsFormatPath = browse["fields"]["details_format"] | "";
  // Version-tracked catalogs predate item details. Conventional field names
  // keep those manifests working while newer catalogs can map the fields explicitly.
  if (manifest.tracksInstalls()) {
    if (manifest.descriptionPath.empty()) manifest.descriptionPath = "description";
    if (manifest.repositoryPath.empty()) manifest.repositoryPath = "repository";
    if (manifest.detailsUrlPath.empty()) manifest.detailsUrlPath = "details.url";
    if (manifest.detailsFormatPath.empty()) manifest.detailsFormatPath = "details.format";
    if (manifest.detailAuthorPath.empty() || manifest.detailAuthorPath == manifest.descriptionPath) {
      manifest.detailAuthorPath = "author";
    }
  }
  JsonVariantConst compatibility = browse["compatibility"];
  manifest.compatFamilyPath = compatibility["firmware_family"] | "";
  manifest.compatFirmwarePath = compatibility["firmware"] | "";
  manifest.compatApiMinPath = compatibility["plugin_api_min"] | "";
  manifest.compatRequiresPath = compatibility["requires"] | "";
  const std::string legacyPageSizeTpl = variantToString(browse["page_size"]);
  manifest.viewTpl = variantToString(browse["view"]);
  if (manifest.viewTpl.empty()) manifest.viewTpl = "list";
  manifest.gridColumnsTpl = variantToString(browse["grid_columns"]);
  if (manifest.gridColumnsTpl.empty()) manifest.gridColumnsTpl = "4";
  manifest.gridRowsTpl = variantToString(browse["grid_rows"]);
  if (manifest.gridRowsTpl.empty()) manifest.gridRowsTpl = "2";
  manifest.listThumbnailsTpl = variantToString(browse["list_thumbnail"]);
  manifest.refreshEnabled = browse["refresh"] | false;
  manifest.hidePaginationTpl = variantToString(browse["hide_pagination"]);
  if (manifest.hidePaginationTpl.empty()) manifest.hidePaginationTpl = "false";
  if (manifest.listThumbnailsTpl.empty()) manifest.listThumbnailsTpl = "false";
  manifest.selectionStyleTpl = variantToString(browse["selection_style"]);
  if (manifest.selectionStyleTpl.empty()) manifest.selectionStyleTpl = "theme";
  manifest.coverRadiusTpl = variantToString(browse["image"]["radius"]);
  if (manifest.coverRadiusTpl.empty()) manifest.coverRadiusTpl = "0";
  manifest.gridPageSizeTpl = variantToString(browse["grid_page_size"]);
  if (manifest.gridPageSizeTpl.empty()) manifest.gridPageSizeTpl = legacyPageSizeTpl.empty() ? "6" : legacyPageSizeTpl;
  manifest.listPageSizeTpl = variantToString(browse["list_page_size"]);
  if (manifest.listPageSizeTpl.empty()) manifest.listPageSizeTpl = legacyPageSizeTpl.empty() ? "10" : legacyPageSizeTpl;
  const JsonVariantConst cache = doc["cache"];
  manifest.cacheEnabled = cache["enabled"] | false;
  const char* cacheScope = cache["scope"] | "shared";
  manifest.cacheTokenScoped = strcasecmp(cacheScope, "token") == 0;
  manifest.cacheRefreshTpl = variantToString(cache["refresh"]);
  if (manifest.cacheRefreshTpl.empty()) manifest.cacheRefreshTpl = "7d";
  manifest.cacheLimitMbTpl = variantToString(cache["limit_mb"]);
  if (manifest.cacheLimitMbTpl.empty()) manifest.cacheLimitMbTpl = "32";
  manifest.cacheNamespaceTpl = variantToString(cache["namespace"]);
  manifest.cacheGenerationTpl = variantToString(cache["generation"]);
  manifest.cacheStaleFallback = cache["stale_fallback"] | true;
  const char* manualScope = cache["manual_refresh"]["scope"] | "catalog";
  if (strcasecmp(manualScope, "current") == 0)
    manifest.cacheManualRefreshScope = Manifest::CacheRefreshScope::Current;
  else if (strcasecmp(manualScope, "plugin") == 0)
    manifest.cacheManualRefreshScope = Manifest::CacheRefreshScope::Plugin;
  else
    manifest.cacheManualRefreshScope = Manifest::CacheRefreshScope::Catalog;
  const char* detailsPolicy = cache["resources"]["details"] | "off";
  if (strcasecmp(detailsPolicy, "ttl") == 0)
    manifest.cacheDetailsPolicy = Manifest::CacheResourcePolicy::Ttl;
  else if (strcasecmp(detailsPolicy, "immutable") == 0)
    manifest.cacheDetailsPolicy = Manifest::CacheResourcePolicy::Immutable;
  else
    manifest.cacheDetailsPolicy = Manifest::CacheResourcePolicy::Disabled;
  manifest.cacheSeedUrl = cache["seed"]["url"] | "";
  pluginhttp::readRequest(browse["image"], "GET", manifest.imageReq);
  const char* imageFormat = browse["image"]["format"] | "jpeg";
  if (strcasecmp(imageFormat, "auto") == 0) manifest.imageFormat = Manifest::ImageFormat::Auto;
  if (strcasecmp(imageFormat, "bmp") == 0) manifest.imageFormat = Manifest::ImageFormat::Bmp;
  if (strcasecmp(imageFormat, "png") == 0) manifest.imageFormat = Manifest::ImageFormat::Png;
  manifest.browseListsUrl = browse["lists_url"] | "";
  manifest.capabilityListsUrl = browse["capability_lists_url"] | "";
  manifest.browseLists.reserve(browse["lists"].size());
  for (JsonVariantConst l : browse["lists"].as<JsonArrayConst>()) {
    Manifest::BrowseList entry;
    entry.title = l["title"] | "";
    entry.url = l["url"] | "";
    entry.body = l["body"] | "";
    JsonVariantConst notice = l["notice"];
    entry.noticeTitle = notice["title"] | "";
    entry.noticeMessage = notice["message"] | "";
    entry.noticeConfirm = notice["confirm"] | "";
    entry.noticeCancel = notice["cancel"] | "";
    if (!entry.title.empty()) manifest.browseLists.push_back(std::move(entry));
  }
  manifest.searchUrl = browse["search"]["url"] | "";
  manifest.searchBody = browse["search"]["body"] | "";

  const JsonVariantConst ui = doc["ui"];
  manifest.defaultTab = ui["default_tab"] | "";
  manifest.tabs.reserve(std::min<int>(MAX_PLUGIN_TABS, ui["tabs"].size()));
  int tabCount = 0;
  for (JsonVariantConst t : ui["tabs"].as<JsonArrayConst>()) {
    if (tabCount++ >= MAX_PLUGIN_TABS) break;
    Manifest::UiTab tab;
    tab.id = t["id"] | "";
    tab.title = t["title"] | tab.id.c_str();
    const char* type = t["type"] | "browse";
    if (strcmp(type, "settings") == 0)
      tab.kind = Manifest::UiTab::Kind::Settings;
    else if (strcmp(type, "cache") == 0)
      tab.kind = Manifest::UiTab::Kind::Cache;
    else if (strcmp(type, "browse") != 0)
      continue;
    if (!tab.id.empty() && !tab.title.empty()) manifest.tabs.push_back(std::move(tab));
  }

  manifest.settings.reserve(std::min<int>(MAX_PLUGIN_SETTINGS, doc["settings"].size()));
  int settingCount = 0;
  for (JsonVariantConst n : doc["settings"].as<JsonArrayConst>()) {
    if (settingCount++ >= MAX_PLUGIN_SETTINGS) break;
    Manifest::Setting setting;
    setting.key = n["key"] | "";
    setting.title = n["title"] | setting.key.c_str();
    setting.defaultValue = variantToString(n["default"]);
    setting.visibleDeviceCapability = n["visible_if"]["device_capability"] | "";
    const char* type = n["type"] | "choice";
    if (strcmp(type, "toggle") == 0) {
      setting.type = Manifest::Setting::Type::Toggle;
      if (setting.defaultValue.empty()) setting.defaultValue = "false";
    } else if (strcmp(type, "choice") == 0) {
      setting.type = Manifest::Setting::Type::Choice;
      setting.options.reserve(std::min<int>(MAX_SETTING_OPTIONS, n["options"].size()));
      int optionCount = 0;
      for (JsonVariantConst o : n["options"].as<JsonArrayConst>()) {
        if (optionCount++ >= MAX_SETTING_OPTIONS) break;
        Manifest::SettingOption option;
        option.title = o["title"] | "";
        option.value = variantToString(o["value"]);
        if (!option.title.empty() && !option.value.empty()) setting.options.push_back(std::move(option));
      }
      if (setting.defaultValue.empty() && !setting.options.empty())
        setting.defaultValue = setting.options.front().value;
      if (setting.options.empty()) continue;
    } else {
      continue;
    }
    if (!setting.key.empty() && !setting.title.empty()) manifest.settings.push_back(std::move(setting));
  }
  manifest.xmlItem = browse["item"] | "";
  manifest.xmlContainer = browse["container_element"] | "";
  manifest.xmlSkipSelf = browse["skip_self"] | false;
  manifest.xmlResolveUrls = browse["resolve_urls"] | false;
  manifest.xmlExtensions.reserve(browse["extensions"].size());
  for (JsonVariantConst ext : browse["extensions"].as<JsonArrayConst>()) {
    if (ext.is<const char*>()) manifest.xmlExtensions.emplace_back(ext.as<const char*>());
  }

  JsonVariantConst dl = doc["download"];
  pluginhttp::readRequest(dl, "GET", manifest.downloadReq);
  manifest.dlUrlPath = dl["url_path"] | "";
  manifest.dlUser = dl["username"] | "";
  manifest.dlPass = dl["password"] | "";
  manifest.destDir = pluginhttp::inPluginDir(pluginDir, dl["dest_dir"] | "");
  if (!manifest.destDir.empty() && !protectedpaths::isPluginPath(manifest.destDir)) {
    LOG_ERR("PCAT", "dest_dir outside plugin space: %s", manifest.destDir.c_str());
    return false;
  }
  manifest.filenameTpl = dl["filename"] | "{title}.epub";
  const JsonVariantConst previewOnSelect = dl["preview_on_select"];
  if (!previewOnSelect.isNull()) {
    if (!previewOnSelect.is<const char*>()) {
      LOG_ERR("PCAT", "preview_on_select must be a {cfg.KEY} template");
      return false;
    }
    manifest.previewOnSelectTpl = previewOnSelect.as<const char*>();
    if (manifest.previewOnSelectTpl.size() < 7 || manifest.previewOnSelectTpl.rfind("{cfg.", 0) != 0 ||
        manifest.previewOnSelectTpl.back() != '}' ||
        manifest.previewOnSelectTpl.find('}') != manifest.previewOnSelectTpl.size() - 1) {
      LOG_ERR("PCAT", "preview_on_select must be a {cfg.KEY} template");
      return false;
    }
  }
  // Multi-file bundle install (generic): base URL + a files array per item.
  manifest.bundleBasePath = dl["bundle"]["base"] | "";
  manifest.bundleFilesPath = dl["bundle"]["files"] | "";
  manifest.bundleSubdir = dl["bundle"]["subdir"] | "{id}";
  // XML-list items already carry the file URL; default the template to it.
  if (manifest.isXmlList() && manifest.downloadReq.url.empty()) manifest.downloadReq.url = "{url}";
  manifest.sidecarPath = dl["sidecar"]["path"] | "";
  manifest.sidecarBody = dl["sidecar"]["body"] | "";

  JsonVariantConst auth = doc["auth"];
  manifest.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", manifest.authReq);
  pluginhttp::readRequest(auth["poll"], "POST", manifest.pollReq);
  manifest.authCodePath = auth["code_path"] | "user_code";
  manifest.authVerifyPath = auth["verify_url_path"] | "verification_uri";
  manifest.authDeviceCodePath = auth["device_code_path"] | "device_code";
  manifest.authIntervalPath = auth["interval_path"] | "interval";
  manifest.authExpiresPath = auth["expires_path"] | "expires_in";
  manifest.authTokenPath = auth["token_path"] | "access_token";
  manifest.authErrorPath = auth["error_path"] | "error";
  manifest.authDisplayUrl = auth["display_url"] | "";
  manifest.authQrHint = auth["qr_hint"] | "";
  manifest.authManualHint = auth["manual_hint"] | "";

  return !manifest.browseReq.url.empty();
}

bool PluginCatalogActivity::saveToken(const std::string& value) {
  return pluginhttp::saveTokenToFile(manifest.tokenFile, manifest.tokenPath, value);
}

bool PluginCatalogActivity::loadToken() {
  // Keep an already-loaded ephemeral session value in RAM while this catalog is open.
  // Its backing file may also be browse scratch storage that moves into the page cache.
  if (!manifest.tokenPersistent && !token.empty()) return true;
  return pluginhttp::loadTokenFromFile(manifest.tokenFile, manifest.tokenPath, token);
}

bool PluginCatalogActivity::clearEphemeralToken() {
  if (manifest.tokenPersistent || manifest.tokenFile.empty()) return true;
  const bool cleared = pluginhttp::removeTokenFile(manifest.tokenFile);
  token.clear();
  return cleared;
}

void PluginCatalogActivity::startDeviceAuth() {
  if (!wifiConnected()) {
    launchWifiSelection();
    return;
  }
  beginAuth();
}

std::string PluginCatalogActivity::configValue(const std::string& key, const std::string& fallback) const {
  for (const auto& kv : config) {
    if (kv.first == key) return kv.second;
  }
  return fallback;
}

void PluginCatalogActivity::resolvePresentation() {
  const std::string view = substituted(manifest.viewTpl, nullptr);
  manifest.gridView = strcasecmp(view.c_str(), "grid") == 0;
  const std::string& layoutPageSizeTpl = manifest.gridView ? manifest.gridPageSizeTpl : manifest.listPageSizeTpl;
  manifest.pageSize = std::clamp(atoi(substituted(layoutPageSizeTpl, nullptr).c_str()), 1, MAX_PAGE_SIZE);
  manifest.gridColumns =
      static_cast<uint8_t>(std::clamp(atoi(substituted(manifest.gridColumnsTpl, nullptr).c_str()), 1, 4));
  manifest.gridRows = static_cast<uint8_t>(std::clamp(atoi(substituted(manifest.gridRowsTpl, nullptr).c_str()), 1, 4));
  const std::string thumbnails = substituted(manifest.listThumbnailsTpl, nullptr);
  manifest.listThumbnails =
      strcasecmp(thumbnails.c_str(), "true") == 0 || thumbnails == "1" || strcasecmp(thumbnails.c_str(), "on") == 0;

  const std::string hidePagination = substituted(manifest.hidePaginationTpl, nullptr);
  const bool wantsHiddenPagination = strcasecmp(hidePagination.c_str(), "true") == 0 || hidePagination == "1" ||
                                     strcasecmp(hidePagination.c_str(), "on") == 0;
  manifest.paginationHidden = wantsHiddenPagination;

  const std::string previewOnSelect = substituted(manifest.previewOnSelectTpl, nullptr);
  manifest.previewOnSelect = strcasecmp(previewOnSelect.c_str(), "true") == 0 || previewOnSelect == "1" ||
                             strcasecmp(previewOnSelect.c_str(), "on") == 0;

  const std::string selection = substituted(manifest.selectionStyleTpl, nullptr);
  manifest.selectionStyle = Manifest::SelectionStyle::Theme;
  if (strcasecmp(selection.c_str(), "light") == 0)
    manifest.selectionStyle = Manifest::SelectionStyle::Light;
  else if (strcasecmp(selection.c_str(), "dark") == 0)
    manifest.selectionStyle = Manifest::SelectionStyle::Dark;
  else if (strcasecmp(selection.c_str(), "outline") == 0)
    manifest.selectionStyle = Manifest::SelectionStyle::Outline;

  manifest.coverRadius =
      static_cast<uint8_t>(std::clamp(atoi(substituted(manifest.coverRadiusTpl, nullptr).c_str()), 0, 16));

  const std::string refresh = substituted(manifest.cacheRefreshTpl, nullptr);
  if (strcasecmp(refresh.c_str(), "always") == 0) {
    manifest.cacheRefreshDays = 0;
  } else {
    const int days = atoi(refresh.c_str());
    manifest.cacheRefreshDays = static_cast<uint16_t>(std::clamp(days > 0 ? days : 7, 1, 30));
  }
  const int limit = atoi(substituted(manifest.cacheLimitMbTpl, nullptr).c_str());
  manifest.cacheLimitMb = static_cast<uint16_t>(std::clamp(limit > 0 ? limit : 32, 8, 128));
  manifest.cacheNamespace = substituted(manifest.cacheNamespaceTpl, nullptr);
  manifest.cacheGeneration =
      remoteCacheGeneration.empty() ? substituted(manifest.cacheGenerationTpl, nullptr) : remoteCacheGeneration;
}

void PluginCatalogActivity::loadConfig() {
  pluginhttp::loadConfigFile(manifest.configFile, config);
  for (const auto& setting : manifest.settings) {
    auto existing = std::find_if(config.begin(), config.end(), [&](const auto& kv) { return kv.first == setting.key; });
    if (existing == config.end()) {
      config.emplace_back(setting.key, setting.defaultValue);
      continue;
    }
    if (setting.type != Manifest::Setting::Type::Choice || setting.options.empty()) continue;
    const bool supported = std::any_of(setting.options.begin(), setting.options.end(),
                                       [&](const auto& option) { return option.value == existing->second; });
    if (!supported) existing->second = setting.defaultValue;
  }
  resolvePresentation();
}

std::string PluginCatalogActivity::settingValue(const Manifest::Setting& setting) const {
  return configValue(setting.key, setting.defaultValue);
}

bool PluginCatalogActivity::deviceCapabilityAvailable(const std::string& capability) const {
  if (capability.empty()) return true;
  if (strcasecmp(capability.c_str(), "touch") == 0) return mappedInput.hasTouch();
  return false;
}

bool PluginCatalogActivity::settingVisible(const Manifest::Setting& setting) const {
  return deviceCapabilityAvailable(setting.visibleDeviceCapability);
}

bool PluginCatalogActivity::saveSettingValue(const Manifest::Setting& setting, const std::string& value) {
  const bool isToggle = setting.type == Manifest::Setting::Type::Toggle;
  if (!pluginhttp::saveConfigValue(manifest.configFile, setting.key, value, isToggle)) return false;
  bool found = false;
  for (auto& kv : config) {
    if (kv.first != setting.key) continue;
    kv.second = value;
    found = true;
    break;
  }
  if (!found) config.emplace_back(setting.key, value);
  const bool cacheLimitChanged = manifest.cacheLimitMbTpl == "{cfg." + setting.key + "}";
  resolvePresentation();
  clearPreviews();
  releaseRows();
  browseRefreshNeeded = true;
  cacheStatsDirty = true;
  // A seed-size, refresh-age, or UI preference change never needs a full
  // directory scan. Only lowering/changing the storage cap can require eviction.
  if (cacheUsable() && cacheLimitChanged) enforceCacheLimit();
  return true;
}

fui::StyleSet PluginCatalogActivity::selectionStyles(const fui::StyleSet& base) const {
  if (manifest.selectionStyle == Manifest::SelectionStyle::Theme) return base;
  fui::StyleSet out = base;
  out.selected = out.normal;
  if (manifest.selectionStyle == Manifest::SelectionStyle::Light) {
    out.selected.background = fui::Paint::dither(fui::Color::LightGray);
    out.selected.foreground = fui::Paint::solid(fui::Color::Black);
  } else if (manifest.selectionStyle == Manifest::SelectionStyle::Dark) {
    out.selected.background = fui::Paint::dither(fui::Color::DarkGray);
    out.selected.foreground = fui::Paint::solid(fui::Color::White);
  } else {
    out.selected.border = fui::Paint::solid(fui::Color::Black);
    out.selected.borderWidth = 2;
    out.selected.foreground = fui::Paint::solid(fui::Color::Black);
  }
  out.focused = out.selected;
  out.active = out.selected;
  return out;
}

pluginhttp::Headers PluginCatalogActivity::substitutedHeaders(const pluginhttp::Headers& headers,
                                                              const Item* item) const {
  pluginhttp::Headers out;
  out.reserve(headers.size());
  for (const auto& h : headers) out.emplace_back(h.first, substituted(h.second, item));
  return out;
}

pluginhttp::RequestSpec PluginCatalogActivity::substitutedRequest(const pluginhttp::RequestSpec& req,
                                                                  const Item* item) const {
  return {substituted(req.url, item), req.method, substituted(req.body, item), substitutedHeaders(req.headers, item)};
}

std::string PluginCatalogActivity::substituted(std::string tpl, const Item* item) const {
  substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) substituteAll(tpl, ("{cfg." + kv.first + "}").c_str(), kv.second);
  substituteAll(tpl, "{firmware_family}", firmwareidentity::FAMILY);
  substituteAll(tpl, "{firmware_id}", firmwareidentity::ID);
  substituteAll(tpl, "{firmware_version}", CROSSPOINT_VERSION);
  substituteAll(tpl, "{plugin_capabilities}", firmwareidentity::PLUGIN_CAPABILITIES);
  char num[16];
  snprintf(num, sizeof(num), "%d", firmwareidentity::PLUGIN_API_VERSION);
  substituteAll(tpl, "{plugin_api}", num);
  snprintf(num, sizeof(num), "%d", page);
  substituteAll(tpl, "{page}", num);
  snprintf(num, sizeof(num), "%d", manifest.pageSize + 1);
  substituteAll(tpl, "{limit}", num);
  substituteAll(tpl, "{query}", urlEncodeQuery(searchQuery));
  substituteAll(tpl, "{query_raw}", searchQuery);
  if (item) {
    substituteAll(tpl, "{id}", item->id);
    substituteAll(tpl, "{title}", item->title);
    substituteAll(tpl, "{author}", item->author);
    substituteAll(tpl, "{url}", item->url);
    substituteAll(tpl, "{thumbnail}", item->thumbnail);
  }
  return tpl;
}

std::string PluginCatalogActivity::downloadDir() const {
  for (const auto& kv : config) {
    if (kv.first != "dest_dir" || kv.second.empty()) continue;
    std::string dir = kv.second.front() == '/' ? kv.second : "/" + kv.second;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (dir == "/") return "";  // SD root
    if (protectedpaths::isPluginPath(dir)) return dir;
    LOG_ERR("PCAT", "config dest_dir rejected: %s", kv.second.c_str());
  }
  return manifest.destDir;
}

PluginCatalogActivity::PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool showOpds, const bool rootMode)
    : CatalogActivity("PluginCatalog", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      showOpds(showOpds),
      rootMode(rootMode) {}

PluginCatalogActivity::~PluginCatalogActivity() = default;

int PluginCatalogActivity::apiRequest(const pluginhttp::RequestSpec& req, String& out) {
  return pluginhttp::request(session.get(), req.url, req.method, req.body, req.headers, out, MAX_API_RESPONSE);
}

void PluginCatalogActivity::onEnter() {
  CatalogActivity::onEnter();
  app.on(ACTION_PLUGIN_TAB, &PluginCatalogActivity::onPluginTabEvent, this);
  app.on(ACTION_PAGE, &PluginCatalogActivity::onPageEvent, this);
  app.on(ACTION_REFRESH, &PluginCatalogActivity::onRefreshEvent, this);
  enterPluginPicker();
}

int PluginCatalogActivity::pinnedPluginCount() const {
  return !installedPlugins.empty() && installedPlugins.front().name == "pluginhub" ? 1 : 0;
}

int PluginCatalogActivity::pluginPickerStart() const {
  const int prefix = (showOpds ? 1 : 0) + (showPluginHubInstallRow ? 1 : 0);
  const int pinned = pinnedPluginCount();
  return prefix + pinned + (static_cast<int>(installedPlugins.size()) > pinned ? 1 : 0);
}

int PluginCatalogActivity::pickerRowForPlugin(const int index) const {
  const int prefix = (showOpds ? 1 : 0) + (showPluginHubInstallRow ? 1 : 0);
  return index < pinnedPluginCount() ? prefix + index : pluginPickerStart() + index - pinnedPluginCount();
}

int PluginCatalogActivity::pickerPluginForRow(const int row) const {
  const int prefix = (showOpds ? 1 : 0) + (showPluginHubInstallRow ? 1 : 0);
  const int pinned = pinnedPluginCount();
  if (row >= prefix && row < prefix + pinned) return row - prefix;
  if (row < pluginPickerStart() || row >= extensionPickerStart()) return -1;
  return pinned + row - pluginPickerStart();
}

int PluginCatalogActivity::extensionPickerStart() const {
  return pluginPickerStart() + static_cast<int>(installedPlugins.size()) - pinnedPluginCount();
}

bool PluginCatalogActivity::pickerHeaderRow(const int index) const {
  if (state != State::PLUGIN_PICKER) return false;
  if (static_cast<int>(installedPlugins.size()) > pinnedPluginCount() && index == pluginPickerStart() - 1) return true;
  const int extIndex = index - extensionPickerStart();
  return extIndex >= 0 && extIndex < static_cast<int>(installedExtensions.size()) &&
         installedExtensions[extIndex].kind == InstalledExtension::Kind::Header;
}

int PluginCatalogActivity::nextPickerRow(const int current, const int direction, const int steps) const {
  const int count = rowCount();
  if (count <= 0) return 0;
  int index = current;
  for (int step = 0; step < steps; ++step) {
    for (int check = 0; check < count; ++check) {
      index = direction > 0 ? ButtonNavigator::nextIndex(index, count) : ButtonNavigator::previousIndex(index, count);
      if (!pickerHeaderRow(index)) break;
    }
  }
  return index;
}

void PluginCatalogActivity::discoverInstalledExtensions() {
  installedExtensions.clear();
  const auto addPackage = [this](InstalledExtension::Kind kind, const std::string& id, const std::string& label,
                                 const std::string& dir) {
    // A theme pack may expose several themes; show the package only once.
    for (const auto& prior : installedExtensions) {
      if (prior.kind == kind && prior.id == id) return;
    }
    std::string raw;
    if (!Storage.readFileToString("PCAT", dir + "/manifest.json", MAX_MANIFEST_SIZE, raw)) return;
    JsonDocument doc;
    JsonDocument filter;
    filter["kind"] = true;
    filter["name"] = true;
    filter["version"] = true;
    filter["title"] = true;
    filter["author"] = true;
    filter["description"] = true;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok ||
        id != (doc["name"] | "") ||
        strcmp(doc["kind"] | "", kind == InstalledExtension::Kind::Theme ? "theme-pack" : "wallpaper-pack") != 0)
      return;
    InstalledExtension entry;
    entry.kind = kind;
    entry.id = id;
    entry.dir = dir;
    entry.title = kind == InstalledExtension::Kind::Theme ? label : doc["title"].as<std::string>();
    if (entry.title.empty()) entry.title = label.empty() ? id : label;
    entry.version = doc["version"] | "";
    entry.author = doc["author"] | "";
    entry.description = doc["description"] | "";
    // Wallpaper v1 manifests cannot declare description. Use the plain
    // package README's first descriptive line without changing that schema.
    if (entry.description.empty()) {
      std::string readme;
      if (Storage.readFileToString("PCAT", dir + "/README.md", 2 * 1024, readme)) {
        bool first = true;
        size_t offset = 0;
        while (offset < readme.size()) {
          const size_t end = readme.find('\n', offset);
          const std::string line = readme.substr(offset, end == std::string::npos ? end : end - offset);
          offset = end == std::string::npos ? readme.size() : end + 1;
          if (line.empty() || line == "\r") continue;
          if (first) { first = false; continue; }  // Skip package title
          entry.description = line.substr(0, 180);
          break;
        }
      }
    }
    installedExtensions.push_back(std::move(entry));
  };
  // Registry entries have already passed firmware-compatible payload checks.
  auto& themes = ThemePackRegistry::getInstance();
  if (themes.choices().empty()) themes.scan();
  for (const auto& theme : themes.choices()) {
    const size_t slash = theme.id.find('/');
    if (slash == std::string::npos) continue;
    const std::string package = theme.id.substr(0, slash);
    addPackage(InstalledExtension::Kind::Theme, package, theme.label,
               "/.crosspoint/themes/" + package + ".crosspoint-theme");
  }
  if (!installedExtensions.empty()) {
    InstalledExtension heading;
    heading.kind = InstalledExtension::Kind::Header;
    heading.title = "Themes";
    installedExtensions.insert(installedExtensions.begin(), std::move(heading));
  }
  const size_t afterThemes = installedExtensions.size();
  auto& wallpapers = WallpaperPackRegistry::getInstance();
  if (wallpapers.choices().empty()) wallpapers.scan();
  for (const auto& pack : wallpapers.choices())
    addPackage(InstalledExtension::Kind::Wallpaper, pack.id, pack.label,
               "/.crosspoint/wallpapers/" + pack.id + ".crosspoint-wallpaper");
  if (installedExtensions.size() > afterThemes) {
    InstalledExtension heading;
    heading.kind = InstalledExtension::Kind::Header;
    heading.title = "Wallpaper Packs";
    installedExtensions.insert(installedExtensions.begin() + afterThemes, std::move(heading));
  }
}

void PluginCatalogActivity::showInstalledExtensionDetails(const int pickerIndex) {
  const int index = pickerIndex - extensionPickerStart();
  if (index < 0 || index >= static_cast<int>(installedExtensions.size())) return;
  const auto& entry = installedExtensions[index];
  if (entry.kind == InstalledExtension::Kind::Header) return;

  DetailsSpec spec;
  spec.header = entry.kind == InstalledExtension::Kind::Theme ? "Installed Theme" : "Installed Wallpaper Pack";
  spec.title = entry.title;
  spec.author = entry.author;
  spec.source = "SD card";
  spec.metadata = "Version " + (entry.version.empty() ? "unknown" : entry.version) + " · Installed";
  spec.descriptionHeading = "About";
  spec.description = entry.description;
  if (spec.description.empty()) {
    // README is optional, capped to protect internal RAM on the X3.
    Storage.readFileToString("PCAT", entry.dir + "/README.md", 2 * 1024, spec.description);
  }
  if (spec.description.empty()) spec.description = "Installed collection. Press and hold its row to remove it.";
  // Removal is offered from the picker by press-and-hold with confirmation.
  auto details = makeUniqueNoThrow<DetailsActivity>(renderer, mappedInput, std::move(spec));
  if (!details) {
    LOG_ERR("PCAT", "OOM: installed extension details");
    return;
  }
  app.clearTapFlash();
  startActivityForResult(std::move(details), [](const ActivityResult&) {});
}

void PluginCatalogActivity::confirmUninstallInstalledExtension(const int pickerIndex) {
  const int index = pickerIndex - extensionPickerStart();
  if (index < 0 || index >= static_cast<int>(installedExtensions.size())) return;
  const InstalledExtension entry = installedExtensions[index];
  if (entry.kind == InstalledExtension::Kind::Header) return;
  // Themes may reference live fonts/bitmaps; wallpaper packs can be removed
  // regardless of the current sleep mode or saved collection.
  const bool selected = entry.kind == InstalledExtension::Kind::Theme &&
                        std::string(SETTINGS.sdThemeId).rfind(entry.id + "/", 0) == 0;
  const bool selectedWallpaper = entry.kind == InstalledExtension::Kind::Wallpaper &&
                                 strcmp(SETTINGS.wallpaperPackId, entry.id.c_str()) == 0;
  const std::string message =
      selected ? "Switch to another theme in Settings before removing this one."
               : selectedWallpaper ? "Remove " + entry.title + "? All Wallpapers will be selected instead."
                                   : "Remove " + entry.title + " from the SD card?";
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput,
                                                        selected ? "Collection in use" : "Remove installed collection",
                                                        message, tr(STR_CANCEL), selected ? "OK" : "Remove");
  if (!dialog) return;
  app.clearTapFlash();
  startActivityForResult(std::move(dialog), [this, entry, selected](const ActivityResult& result) {
    if (result.isCancelled || selected) return;
    // Only registry-verified package IDs under their own install roots are removable.
    const std::string expected = (entry.kind == InstalledExtension::Kind::Theme
                                      ? "/.crosspoint/themes/" + entry.id + ".crosspoint-theme"
                                      : "/.crosspoint/wallpapers/" + entry.id + ".crosspoint-wallpaper");
    if (entry.dir != expected || !Storage.removeDir(expected.c_str())) {
      LOG_ERR("PCAT", "Failed to uninstall installed extension: %s", expected.c_str());
      return;
    }
    if (entry.kind == InstalledExtension::Kind::Theme) {
      ThemePackRegistry::getInstance().scan();
    } else {
      auto& wallpapers = WallpaperPackRegistry::getInstance();
      wallpapers.scan();
      wallpapers.reconcileSelection();
    }
    enterPluginPicker();
  });
}

void PluginCatalogActivity::enterPluginPicker() {
  Storage.remove(BROWSE_TMP_PATH);
  installedPlugins = discoverPlugins();
  discoverInstalledExtensions();
  pluginHubAvailable = PluginHubInstallActivity::isAvailable();
  showPluginHubInstallRow = !pluginHubAvailable && !SETTINGS.pluginHubPromptHidden;
  // Discovery just re-read the plugin folders; keep the event subscription
  // table in step so a plugin installed since boot starts receiving events
  // (and a removed one stops) without a restart.
  pluginevents::refreshSubscriptions();
  // Disclose which device events each plugin receives (and so what reading
  // activity it can send off the device). Events precede the optional summary.
  static constexpr StrId EVENT_LABELS[] = {StrId::STR_EVENT_BOOK_OPEN, StrId::STR_EVENT_BOOK_CLOSE,
                                           StrId::STR_EVENT_READING_SESSION, StrId::STR_EVENT_DOWNLOAD,
                                           StrId::STR_EVENT_SLEEP};
  static_assert(sizeof(EVENT_LABELS) / sizeof(EVENT_LABELS[0]) == static_cast<size_t>(pluginevents::Event::COUNT));
  for (auto& plugin : installedPlugins) {
    const uint8_t mask = pluginevents::subscriptionMask(plugin.name.c_str());
    if (mask == 0) continue;
    std::string events;
    for (size_t i = 0; i < std::size(EVENT_LABELS); i++) {
      if (!(mask & (1u << i))) continue;
      if (!events.empty()) events += ", ";
      events += I18N.get(EVENT_LABELS[i]);
    }
    char line[192];
    snprintf(line, sizeof(line), tr(STR_PLUGIN_RECEIVES_EVENTS), events.c_str());
    plugin.description = plugin.description.empty() ? line : std::string(line) + ". " + plugin.description;
  }

  manifestPath.clear();
  manifest = Manifest{};
  remoteCacheGeneration.clear();
  manifestLoaded = false;
  activePluginTab = -1;
  pluginTabNavs.clear();
  catalogTitle = tr(STR_PLUGINS);
  token.clear();
  config.clear();
  resetBrowse();
  browseHistory.clear();
  browseCurrentUrl.clear();
  errorMessage.clear();
  session.reset();  // no TLS while only picking
  state = State::PLUGIN_PICKER;
  bool restoredSelection = false;
  for (size_t i = 0; i < installedPlugins.size(); ++i) {
    if (installedPlugins[i].name != pickerReturnName) continue;
    moveSelectionTo(pickerRowForPlugin(static_cast<int>(i)));
    restoredSelection = true;
    break;
  }
  if (!restoredSelection && rowCount() > 0) moveSelectionTo(nextPickerRow(-1, 1));
  requestUpdate();
}

void PluginCatalogActivity::confirmUninstallPlugin(const int pickerIndex) {
  if (state != State::PLUGIN_PICKER) return;
  if (pickerIndex >= extensionPickerStart()) {
    confirmUninstallInstalledExtension(pickerIndex);
    return;
  }
  const int pluginIndex = pickerPluginForRow(pickerIndex);
  if (pluginIndex < 0 || pluginIndex >= static_cast<int>(installedPlugins.size())) return;

  const PluginRef plugin = installedPlugins[pluginIndex];
  bool safeDir = false;
  for (const char* root : PluginLocations::ROOTS) {
    if (plugin.dir == std::string(root) + "/" + plugin.name) {
      safeDir = true;
      break;
    }
  }
  if (!safeDir) {
    LOG_ERR("PCAT", "Refusing unsafe plugin uninstall path: %s", plugin.dir.c_str());
    return;
  }

  char message[256];
  snprintf(message, sizeof(message), tr(STR_PLUGIN_UNINSTALL_CONFIRM), plugin.title.c_str());
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_PLUGIN_UNINSTALL_TITLE),
                                                              message, tr(STR_CANCEL), tr(STR_PLUGIN_UNINSTALL));
  if (!confirmation) {
    LOG_ERR("PCAT", "OOM: plugin uninstall confirmation");
    return;
  }

  std::string returnPluginName;
  if (pluginIndex + 1 < static_cast<int>(installedPlugins.size()))
    returnPluginName = installedPlugins[pluginIndex + 1].name;
  else if (pluginIndex > 0)
    returnPluginName = installedPlugins[pluginIndex - 1].name;

  app.clearTapFlash();
  startActivityForResult(std::move(confirmation), [this, plugin, returnPluginName](const ActivityResult& result) {
    if (result.isCancelled) return;
    const bool removed = !Storage.exists(plugin.dir.c_str()) || Storage.removeDir(plugin.dir.c_str());
    if (!removed) LOG_ERR("PCAT", "Failed to recursively uninstall plugin: %s", plugin.dir.c_str());

    pickerReturnName = returnPluginName;
    enterPluginPicker();
  });
}

void PluginCatalogActivity::enterCatalog() {
  state = State::CHECK_WIFI;
  resetBrowse();
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  session.reset(new (std::nothrow) freeink::SecureHttpClient());
  if (session) session->setReuse(true);

  manifestLoaded = false;
  if (!loadManifest()) {
    manifest = Manifest{};
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
    return;
  }
  manifestLoaded = true;
  if (!clearEphemeralToken()) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  loadConfig();
  if (!manifest.tabs.empty()) {
    activePluginTab = 0;
    for (size_t i = 0; i < manifest.tabs.size(); ++i) {
      if (manifest.tabs[i].id == manifest.defaultTab) {
        activePluginTab = static_cast<int>(i);
        break;
      }
    }
    pluginTabNavs.assign(manifest.tabs.size(), fui::ListNav{});
    for (auto& tabNav : pluginTabNavs) tabNav.reset();
  }
  // Cache statistics require a full directory walk. Defer that work until a
  // cache/settings surface actually needs the values so browse startup stays fast.
  cacheStatsDirty = true;
  requestUpdate();
  // Let fetchPage decide whether the requested page can be served from SD.
  // Wi-Fi is only requested on an actual cache miss/stale refresh.
  startBrowse();
}

// Leaving the open catalog (Back at its root, or Back out of an error /
// sign-in screen): return to the plugin picker. Wi-Fi stays up so the next
// pick connects instantly; onExit tears it down when the activity ends.
void PluginCatalogActivity::exitCatalog() {
  clearEphemeralToken();
  enterPluginPicker();
}

bool PluginCatalogActivity::pluginTabsActive() const {
  return state == State::BROWSING && activePluginTab >= 0 && activePluginTab < static_cast<int>(manifest.tabs.size()) &&
         pluginTabNavs.size() == manifest.tabs.size();
}

bool PluginCatalogActivity::browseTabActive() const {
  return pluginTabsActive() && manifest.tabs[activePluginTab].kind == Manifest::UiTab::Kind::Browse;
}

bool PluginCatalogActivity::cacheTabActive() const {
  return pluginTabsActive() && manifest.tabs[activePluginTab].kind == Manifest::UiTab::Kind::Cache;
}

bool PluginCatalogActivity::settingsTabActive() const {
  return pluginTabsActive() && manifest.tabs[activePluginTab].kind == Manifest::UiTab::Kind::Settings;
}

bool PluginCatalogActivity::tabsFocused() const {
  return pluginTabsActive() && pluginTabNavs[activePluginTab].selected == 0;
}

fui::ListNav& PluginCatalogActivity::activeNav() {
  if (pluginTabsActive()) return pluginTabNavs[activePluginTab];
  return nav;
}

bool PluginCatalogActivity::hasSearch() const {
  // The tab ring owns Previous/Next while present. Non-tabbed catalogs retain
  // the legacy top-row Search shortcut unchanged.
  return manifest.hasSearch() && manifest.tabs.empty();
}

void PluginCatalogActivity::switchPluginTab(const int index, const bool keepFocus) {
  if (index < 0 || index >= static_cast<int>(manifest.tabs.size()) || index == activePluginTab) return;
  const bool focused = tabsFocused();
  activePluginTab = index;
  if (!settingsTabActive()) cacheSettingsExpanded = false;
  auto& target = pluginTabNavs[activePluginTab];
  if (keepFocus && focused)
    target.selected = 0;
  else if (target.selected == 0 && rowCount() > 0)
    target.selected = 1;
  releaseRows();
  if (cacheTabActive()) {
    // Opening the Cache tab must not scan thousands of seed files.
    requestUpdate();
    return;
  }
  const bool refreshEmptyBrowse = browseTabActive() && items.empty();
  if (browseTabActive() && (browseRefreshNeeded || refreshEmptyBrowse)) {
    browseRefreshNeeded = false;
    if (refreshEmptyBrowse) forceCacheRefresh = true;
    beginLoading();
    fetchPage(1);
    return;
  }
  if (browseTabActive() && wantsPreviewImages() && !preparePreviews()) return;
  requestUpdate();
}

void PluginCatalogActivity::stepPluginTab(const int direction) {
  if (!pluginTabsActive() || manifest.tabs.size() < 2) return;
  const int count = static_cast<int>(manifest.tabs.size());
  const int next = direction > 0 ? ButtonNavigator::nextIndex(activePluginTab, count)
                                 : ButtonNavigator::previousIndex(activePluginTab, count);
  switchPluginTab(next, true);
}

void PluginCatalogActivity::onPluginTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<PluginCatalogActivity*>(user);
  if (!self->pluginTabsActive() || event.value < 0 || event.value >= static_cast<int>(self->manifest.tabs.size()))
    return;
  self->switchPluginTab(event.value, false);
}

void PluginCatalogActivity::onPageEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<PluginCatalogActivity*>(user);
  if (!self->nativeFooterActive() || (event.value != -1 && event.value != 1)) return;
  if ((event.value < 0 && !self->prevRowVisible()) || (event.value > 0 && !self->nextRowVisible())) return;
  self->app.clearTapFlash();
  self->beginLoading();
  self->fetchPage(self->page + event.value);
}

void PluginCatalogActivity::onRefreshEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<PluginCatalogActivity*>(user);
  if (!self->nativeFooterActive() || !self->manifest.refreshEnabled) return;
  self->app.clearTapFlash();
  self->forceCacheRefresh = true;
  self->beginLoading();
  self->fetchPage(std::max(1, self->page));
}

void PluginCatalogActivity::resetBrowse() {
  clearPreviews();
  items.clear();
  page = 1;
  hasMore = false;
  currentList = -1;
  browseListsRemoteAttempted = false;
  searchActive = false;
  searchQuery.clear();
  browseRefreshNeeded = false;
  browseLoadedFromCache = false;
  forceCacheRefresh = false;
  browseRequestedTop.store(-1);
  browseViewportTop.store(0);
  browseViewportItems.store(1);
  pendingNetworkPage = 0;
  pendingItemActivation = -1;
  pendingItemDownload = -1;
  pendingCacheSeedInstall = false;
  pendingPreviewFill = false;
  cacheSettingsExpanded = false;
  browseSourcePath.clear();
  releaseRows();
  nav.reset();
  for (auto& tabNav : pluginTabNavs) tabNav.reset();
}

// Shows the list's notice first when it has one. Declining stays on the
// picker, or leaves the catalog when this list is the only one.
void PluginCatalogActivity::openList(const int index) {
  const auto& list = manifest.browseLists[index];
  if (!list.hasNotice()) {
    currentList = index;
    startBrowse();
    return;
  }
  const std::string heading = substituted(list.noticeTitle.empty() ? list.title : list.noticeTitle, nullptr);
  const std::string message = substituted(list.noticeMessage, nullptr);
  const std::string cancel = substituted(list.noticeCancel, nullptr);
  const std::string confirm = substituted(list.noticeConfirm, nullptr);
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, message, cancel, confirm);
  if (!confirmation) {
    LOG_ERR("PCAT", "OOM: browse-list notice");
    if (manifest.browseLists.size() == 1) exitCatalog();
    return;
  }
  startActivityForResult(std::move(confirmation), [this, index](const ActivityResult& result) {
    if (result.isCancelled) {
      if (manifest.browseLists.size() == 1) exitCatalog();
      return;
    }
    currentList = index;
    startBrowse();
  });
}

bool PluginCatalogActivity::wantsListPicker() const {
  return !manifest.browseLists.empty() && !manifest.isXmlList() && currentList < 0;
}

bool PluginCatalogActivity::loadBrowseListIndex() {
  if (manifest.browseListsUrl.empty()) return true;
  if (!wifiConnected()) return false;

  String response;
  const bool preferV2 = !manifest.capabilityListsUrl.empty();
  std::string url = substituted(preferV2 ? manifest.capabilityListsUrl : manifest.browseListsUrl, nullptr);
  int status = pluginhttp::request(session.get(), url, "GET", "", {}, response, MAX_BROWSE_LIST_INDEX_RESPONSE);
  if ((status < 200 || status >= 300) && preferV2 && !manifest.browseListsUrl.empty()) {
    response = "";
    url = substituted(manifest.browseListsUrl, nullptr);
    status = pluginhttp::request(session.get(), url, "GET", "", {}, response, MAX_BROWSE_LIST_INDEX_RESPONSE);
  }
  if (status < 200 || status >= 300) {
    LOG_ERR("PCAT", "browse list index fetch failed: status=%d %s", status, url.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    LOG_ERR("PCAT", "browse list index parse failed: %s", url.c_str());
    return false;
  }

  std::string generation = variantToString(doc["generation"]);
  if (generation.size() > 96) generation.resize(96);
  remoteCacheGeneration = std::move(generation);
  manifest.cacheGeneration =
      remoteCacheGeneration.empty() ? substituted(manifest.cacheGenerationTpl, nullptr) : remoteCacheGeneration;

  const bool v2 = doc["schema_version"].as<int>() == 2;
  JsonArrayConst lists = doc["lists"].as<JsonArrayConst>();
  if (lists.isNull()) lists = doc["catalogs"].as<JsonArrayConst>();
  if (lists.isNull() || lists.size() == 0) {
    LOG_ERR("PCAT", "browse list index has no lists: %s", url.c_str());
    return false;
  }

  constexpr size_t MAX_REMOTE_BROWSE_LISTS = 64;
  std::vector<Manifest::BrowseList> remoteLists;
  remoteLists.reserve(std::min<size_t>(lists.size(), MAX_REMOTE_BROWSE_LISTS));
  for (JsonVariantConst l : lists) {
    if (remoteLists.size() >= MAX_REMOTE_BROWSE_LISTS) break;
    Manifest::BrowseList entry;
    entry.title = l["title"] | "";
    entry.url = l["url"] | "";
    entry.body = l["body"] | "";
    if (v2) {
      entry.kind = l["kind"] | "";
      entry.itemsPath = l["items"] | "";
      if ((entry.kind != "plugin" && entry.kind != "theme-pack" && entry.kind != "wallpaper-pack") ||
          !extensionpackages::supportsAll(l["requires"].as<JsonArrayConst>()))
        continue;
      if (entry.kind != "plugin" && entry.itemsPath != "items") continue;
      if (entry.kind == "plugin" && entry.itemsPath != "plugins") continue;
    }
    JsonVariantConst notice = l["notice"];
    entry.noticeTitle = notice["title"] | "";
    entry.noticeMessage = notice["message"] | "";
    entry.noticeConfirm = notice["confirm"] | "";
    entry.noticeCancel = notice["cancel"] | "";
    if (!entry.title.empty() && !entry.url.empty()) remoteLists.push_back(std::move(entry));
  }
  if (remoteLists.empty()) {
    LOG_ERR("PCAT", "browse list index has no usable lists: %s", url.c_str());
    return false;
  }
  manifest.browseLists = std::move(remoteLists);
  return true;
}

void PluginCatalogActivity::onExit() {
  clearPreviews();
  items.clear();
  session.reset();  // drop browse TLS before Wi-Fi teardown
  clearEphemeralToken();
  Storage.remove(BROWSE_TMP_PATH);
  Storage.remove(CACHE_SEED_TMP_PATH);
  CatalogActivity::onExit();
}

void PluginCatalogActivity::startBrowse() {
  if (pendingCacheSeedInstall) {
    if (!wifiConnected()) {
      launchWifiSelection();
      return;
    }
    pendingCacheSeedInstall = false;
    installCacheSeed();
    return;
  }
  if (pendingPreviewFill) {
    if (!wifiConnected()) {
      launchWifiSelection();
      return;
    }
    pendingPreviewFill = false;
    if (!preparePreviews()) return;
    state = State::BROWSING;
    requestUpdate();
    return;
  }
  if (pendingItemDownload >= 0) {
    if (!wifiConnected()) {
      launchWifiSelection();
      return;
    }
    const int itemIndex = pendingItemDownload;
    pendingItemDownload = -1;
    state = State::BROWSING;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) {
      requestUpdate();
      return;
    }
    downloadItem(itemIndex);
    return;
  }
  if (pendingItemActivation >= 0) {
    if (!wifiConnected()) {
      launchWifiSelection();
      return;
    }
    const int itemIndex = pendingItemActivation;
    pendingItemActivation = -1;
    state = State::BROWSING;
    activateItem(itemIndex);
    return;
  }
  if (pendingNetworkPage > 0 && wifiConnected()) {
    const int requestedPage = pendingNetworkPage;
    pendingNetworkPage = 0;
    beginLoading();
    fetchPage(requestedPage);
    return;
  }

  if (manifest.hasDeviceCode() && !manifest.tokenPersistent) {
    loadConfig();
    if (!loadToken()) {
      startDeviceAuth();
      return;
    }
  }

  const bool remoteListsAvailable =
      !manifest.isXmlList() && !manifest.browseListsUrl.empty() && !browseListsRemoteAttempted;
  // A manifest-provided list is an intentional offline/fast-start fallback. Do not
  // force Wi-Fi merely to refresh lists_url before the plugin can show that list.
  // Reopening while already connected still refreshes the dynamic index normally.
  if (remoteListsAvailable && !manifest.browseLists.empty() && !wifiConnected()) {
    browseListsRemoteAttempted = true;
    LOG_INF("PCAT", "browse list index: using manifest fallback while offline");
  }
  const bool needsRemoteLists = remoteListsAvailable && !browseListsRemoteAttempted;
  const bool needsListAuth = needsRemoteLists || wantsListPicker();
  if (needsListAuth) {
    // Dynamic list indexes and list pickers may both use {token}/{cfg.KEY}, so
    // resolve credentials before either path. Token-less catalogs skip the gate.
    loadConfig();
    if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
      if (manifest.hasDeviceCode()) {
        startDeviceAuth();
      } else {
        state = State::NO_TOKEN;
        requestUpdate();
      }
      return;
    }
  }

  if (needsRemoteLists) {
    if (!wifiConnected()) {
      launchWifiSelection();
      return;
    }
    browseListsRemoteAttempted = true;
    if (!loadBrowseListIndex() && manifest.browseLists.empty()) {
      fail(StrId::STR_FETCH_FEED_FAILED);
      return;
    }
  }

  // Browse lists apply to JSON catalogs; XML lists navigate by folder instead.
  if (wantsListPicker()) {
    clearPreviews();
    items.clear();
    page = 1;
    hasMore = false;
    releaseRows();
    nav.reset();
    state = State::LIST_PICKER;
    requestUpdate();
    return;
  }
  if (currentList >= 0 && browseRefreshNeeded) {
    browseRefreshNeeded = false;
    forceCacheRefresh = true;
  }
  beginLoading();
  fetchPage(1);
}

void PluginCatalogActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    requestUpdate();
    return;
  }
  searchQuery = query;
  searchActive = true;
  currentList = -1;  // search spans the whole catalog, not a single list
  releaseRows();
  nav.reset();
  beginLoading();
  fetchPage(1);
}

bool PluginCatalogActivity::refreshCredentialToken() {
  loadConfig();
  std::string minted;
  if (!pluginhttp::mintPasswordToken(session.get(), substituted(manifest.authReq.url, nullptr), manifest.authReq.method,
                                     substituted(manifest.authReq.body, nullptr),
                                     substitutedHeaders(manifest.authReq.headers, nullptr), manifest.authTokenPath,
                                     minted)) {
    return false;
  }
  if (!saveToken(minted)) return false;
  token = minted;  // usable immediately, without re-reading the file
  return true;
}

bool PluginCatalogActivity::fetchBrowseResponse() {
  loadConfig();
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return false;
  }
  if (manifest.isXmlList()) {
    if (manifest.xmlItem.empty()) {
      fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
      return false;
    }
    if (browseCurrentUrl.empty()) browseCurrentUrl = substituted(manifest.browseReq.url, nullptr);
  }
  const auto run = [&] {
    const pluginhttp::RequestSpec req = {
        substituted(manifest.isXmlList() ? browseCurrentUrl : activeBrowseUrl(), nullptr), manifest.browseReq.method,
        substituted(manifest.isXmlList() ? manifest.browseReq.body : activeBrowseBody(), nullptr),
        substitutedHeaders(manifest.browseReq.headers, nullptr)};
    return pluginhttp::requestToFile(session.get(), req.url, req.method, req.body, req.headers, BROWSE_TMP_PATH,
                                     MAX_BROWSE_RESPONSE);
  };
  int status = run();
  // Rebuild templates with the refreshed token; retry only once.
  if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) status = run();
  if (status >= 200 && status < 300) return true;  // includes WebDAV's 207 Multi-Status
  Storage.remove(BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    state = State::NO_TOKEN;
    requestUpdate();
  }
  return false;
}

bool PluginCatalogActivity::parseXmlList() {
  releaseRows();
  items.clear();
  auto append = [&](XmlListParser::RawItem& row) {
    Item item;
    item.isDir = row.isDir;
    item.url = std::move(row.field[XmlListParser::F_URL]);
    item.title = std::move(row.field[XmlListParser::F_TITLE]);
    item.author = std::move(row.field[XmlListParser::F_AUTHOR]);
    item.id = std::move(row.field[XmlListParser::F_ID]);
    if (items.size() == items.capacity()) {
      items.reserve(std::min(XmlListParser::MAX_ITEMS, std::max<size_t>(manifest.pageSize, items.capacity() * 2)));
    }
    items.push_back(std::move(item));
  };
  const std::string* const selectors[] = {&manifest.urlPath, &manifest.titlePath, &manifest.authorPath,
                                          &manifest.idPath};
  auto parser = makeUniqueNoThrow<XmlListParser>(
      manifest.xmlItem, manifest.xmlContainer, selectors,
      [](void* context, XmlListParser::RawItem& row) { (*static_cast<decltype(append)*>(context))(row); }, &append);
  if (parser) {
    XmlListParser::UrlOptions urls;
    urls.requestUrl = browseCurrentUrl;
    urls.skipSelf = manifest.xmlSkipSelf;
    urls.resolveUrls = manifest.xmlResolveUrls;
    urls.extensions = manifest.xmlExtensions;
    parser->setUrlOptions(std::move(urls));
  }
  if (!streamBrowseFile(parser.get())) {
    fail(StrId::STR_PARSE_FEED_FAILED);
    return false;
  }

  // Folders first, then files, each alphabetical — matches how file managers list.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  hasMore = false;
  return true;
}

bool PluginCatalogActivity::prevRowVisible() const {
  return state == State::BROWSING && !settingsTabActive() && !manifest.isXmlList() && page > 1;
}

bool PluginCatalogActivity::nextRowVisible() const {
  return state == State::BROWSING && !settingsTabActive() && !manifest.isXmlList() && hasMore;
}

int PluginCatalogActivity::rowCount() const {
  if (state == State::PLUGIN_PICKER) return extensionPickerStart() + static_cast<int>(installedExtensions.size());
  if (state == State::LIST_PICKER) return static_cast<int>(manifest.browseLists.size());
  if (state != State::BROWSING) return 0;
  if (cacheTabActive()) return cacheRowCount();
  if (settingsTabActive()) return settingsRowCount();
  if (nativeFooterActive()) return static_cast<int>(items.size());
  return static_cast<int>(items.size()) + (prevRowVisible() ? 1 : 0) + (nextRowVisible() ? 1 : 0);
}

const std::string& PluginCatalogActivity::activeBrowseUrl() const {
  // Search overrides the list view, reusing the browse url when unspecified.
  if (searchActive) return pick(manifest.searchUrl, manifest.browseReq.url);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseReq.url;
  return pick(manifest.browseLists[currentList].url, manifest.browseReq.url);
}

const std::string& PluginCatalogActivity::activeBrowseBody() const {
  if (searchActive) return pick(manifest.searchBody, manifest.browseReq.body);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseReq.body;
  return pick(manifest.browseLists[currentList].body, manifest.browseReq.body);
}

bool PluginCatalogActivity::wantsPreviewImages() const {
  return !settingsTabActive() && !cacheTabActive() && !manifest.isXmlList() && manifest.hasImages() &&
         (manifest.gridView || manifest.listThumbnails);
}

bool PluginCatalogActivity::nativeFooterActive() const {
  if (state != State::BROWSING || settingsTabActive() || cacheTabActive() || manifest.isXmlList()) return false;
  // Enhanced plugin UIs use the SDK footer as their action surface. Plain
  // legacy catalogs keep their existing synthetic Previous/Next rows.
  return pluginTabsActive() || manifest.hasImages() || manifest.refreshEnabled;
}

int PluginCatalogActivity::contentSelectionOffset() const { return pluginTabsActive() ? 1 : 0; }

int PluginCatalogActivity::footerPreviousSelection() const {
  if (!nativeFooterActive() || manifest.paginationHidden || !prevRowVisible()) return -1;
  return contentSelectionOffset() + static_cast<int>(items.size());
}

int PluginCatalogActivity::footerRefreshSelection() const {
  if (!nativeFooterActive() || !manifest.refreshEnabled) return -1;
  return contentSelectionOffset() + static_cast<int>(items.size()) +
         (!manifest.paginationHidden && prevRowVisible() ? 1 : 0);
}

int PluginCatalogActivity::footerNextSelection() const {
  if (!nativeFooterActive() || manifest.paginationHidden || !nextRowVisible()) return -1;
  return contentSelectionOffset() + static_cast<int>(items.size()) + (prevRowVisible() ? 1 : 0) +
         (manifest.refreshEnabled ? 1 : 0);
}

int PluginCatalogActivity::navigationCount() const {
  const int offset = contentSelectionOffset();
  if (nativeFooterActive()) {
    const bool showPagination = !manifest.paginationHidden;
    return offset + static_cast<int>(items.size()) + (showPagination && prevRowVisible() ? 1 : 0) +
           (manifest.refreshEnabled ? 1 : 0) + (showPagination && nextRowVisible() ? 1 : 0);
  }
  return offset + rowCount();
}

std::string PluginCatalogActivity::previewPath(const int itemIndex) {
  char path[32];
  snprintf(path, sizeof(path), "/.pcat_thumb_%02d.bmp", itemIndex);
  return path;
}

bool PluginCatalogActivity::cacheUsable() const {
  return manifest.cacheEnabled && !manifest.isXmlList() && !searchActive && !manifestPath.empty() &&
         (!manifest.cacheTokenScoped || !token.empty());
}

std::string PluginCatalogActivity::cacheDir() const {
  const size_t slash = manifestPath.rfind('/');
  return slash == std::string::npos ? "" : manifestPath.substr(0, slash) + "/.cache";
}

std::string PluginCatalogActivity::cacheSubstituted(std::string tpl, const Item* item, const int requestedPage) const {
  for (const auto& kv : config) substituteAll(tpl, ("{cfg." + kv.first + "}").c_str(), kv.second);
  char num[16];
  snprintf(num, sizeof(num), "%d", requestedPage > 0 ? requestedPage : page);
  substituteAll(tpl, "{page}", num);
  snprintf(num, sizeof(num), "%d", manifest.pageSize + 1);
  substituteAll(tpl, "{limit}", num);
  substituteAll(tpl, "{query}", urlEncodeQuery(searchQuery));
  substituteAll(tpl, "{query_raw}", searchQuery);
  if (item) {
    substituteAll(tpl, "{id}", item->id);
    substituteAll(tpl, "{title}", item->title);
    substituteAll(tpl, "{author}", item->author);
    substituteAll(tpl, "{url}", item->url);
    substituteAll(tpl, "{thumbnail}", item->thumbnail);
  }
  // Deliberately leave {token} unresolved: rotating credentials must not
  // invalidate otherwise identical cached catalog pages or thumbnails.
  return tpl;
}

std::string PluginCatalogActivity::cacheCatalogKey() const {
  if (!cacheUsable()) return "";
  std::string key = "catalog-v1|" + manifest.browseReq.method + "|" + cacheSubstituted(activeBrowseUrl(), nullptr, 1) +
                    "|" + cacheSubstituted(activeBrowseBody(), nullptr, 1) + "|" + std::to_string(currentList) + "|" +
                    std::to_string(manifest.pageSize) + "|" + manifest.itemsPath + "|" + manifest.titlePath + "|" +
                    manifest.authorPath + "|" + manifest.idPath + "|" + manifest.urlPath + "|" + manifest.versionPath;
  if (!manifest.cacheNamespace.empty()) key += "|namespace|" + manifest.cacheNamespace;
  if (manifest.cacheTokenScoped) key += "|token|" + md5Hex(token);
  return key;
}

std::string PluginCatalogActivity::cacheRequestKey(const pluginhttp::RequestSpec& request, const Item* item,
                                                   const std::string& identity) const {
  if (!cacheUsable()) return "";
  std::string key = identity + "|" + request.method + "|" + cacheSubstituted(request.url, item) + "|" +
                    cacheSubstituted(request.body, item);
  for (const auto& header : request.headers) key += "|h|" + header.first + "|" + cacheSubstituted(header.second, item);
  if (!manifest.cacheNamespace.empty()) key += "|namespace|" + manifest.cacheNamespace;
  if (manifest.cacheTokenScoped) key += "|token|" + md5Hex(token);
  return key;
}

std::string PluginCatalogActivity::cachePagePath(const int requestedPage) const {
  if (!cacheUsable()) return "";
  std::string key =
      "page-v1|" + manifest.browseReq.method + "|" + cacheSubstituted(activeBrowseUrl(), nullptr, requestedPage) + "|" +
      cacheSubstituted(activeBrowseBody(), nullptr, requestedPage) + "|" + std::to_string(currentList) + "|" +
      std::to_string(manifest.pageSize) + "|" + manifest.itemsPath + "|" + manifest.titlePath + "|" +
      manifest.authorPath + "|" + manifest.idPath + "|" + manifest.urlPath + "|" + manifest.versionPath;
  if (!manifest.cacheNamespace.empty()) key += "|namespace|" + manifest.cacheNamespace;
  if (manifest.cacheTokenScoped) key += "|token|" + md5Hex(token);
  return cacheDir() + "/p_" + md5Hex(key) + ".json";
}

std::string PluginCatalogActivity::cachePageMetaPath(const int requestedPage) const {
  std::string path = cachePagePath(requestedPage);
  if (path.size() >= 5) path.replace(path.size() - 5, 5, ".meta");
  return path;
}

std::string PluginCatalogActivity::cacheResourcePath(const pluginhttp::RequestSpec& request, const Item* item,
                                                     const std::string& identity, const char* prefix,
                                                     const char* extension) const {
  const std::string key = cacheRequestKey(request, item, identity);
  if (key.empty()) return "";
  return cacheDir() + "/" + (prefix ? prefix : "r_") + md5Hex(key) + (extension ? extension : ".dat");
}

std::string PluginCatalogActivity::cacheResourceMetaPath(const std::string& resourcePath) const {
  if (resourcePath.empty()) return "";
  std::string path = resourcePath;
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    path.replace(dot, std::string::npos, ".meta");
  else
    path += ".meta";
  return path;
}

std::string PluginCatalogActivity::cacheImagePath(const Item& item) const {
  if (!cacheUsable() || !manifest.hasImages()) return "";
  // Preserve the legacy image-v1 identity for existing caches/seeds. Namespace
  // is the explicit opt-in boundary for a plugin that intentionally changes it.
  std::string key = "image-v1|200x300|" + std::to_string(static_cast<int>(manifest.imageFormat)) + "|" +
                    manifest.imageReq.method + "|" + cacheSubstituted(manifest.imageReq.url, &item) + "|" +
                    cacheSubstituted(manifest.imageReq.body, &item);
  if (!manifest.cacheNamespace.empty()) key += "|namespace|" + manifest.cacheNamespace;
  if (manifest.cacheTokenScoped) key += "|token|" + md5Hex(token);
  return cacheDir() + "/i_" + md5Hex(key) + ".bmp";
}

std::string PluginCatalogActivity::cachePluginEpochPath() const {
  const std::string dir = cacheDir();
  return dir.empty() ? "" : dir + "/e_plugin.meta";
}

std::string PluginCatalogActivity::cacheCatalogEpochPath() const {
  const std::string key = cacheCatalogKey();
  return key.empty() ? "" : cacheDir() + "/e_" + md5Hex(key) + ".meta";
}

bool PluginCatalogActivity::ensureCacheDir() {
  const std::string dir = cacheDir();
  return !dir.empty() && Storage.ensureDirectoryExists(dir.c_str());
}

bool PluginCatalogActivity::readCacheMetadata(const std::string& path, CacheMetadata& metadata) const {
  metadata = {};
  std::string raw;
  if (path.empty() || !Storage.readFileToString("PCAT", path, 512, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  metadata.capturedAt = doc["captured_at"] | static_cast<int64_t>(0);
  metadata.epoch = doc["epoch"] | static_cast<uint32_t>(0);
  metadata.generation = variantToString(doc["generation"]);
  return true;
}

bool PluginCatalogActivity::writeCacheMetadata(const std::string& path, const CacheMetadata& metadata) {
  if (path.empty()) return false;
  JsonDocument doc;
  doc["captured_at"] = metadata.capturedAt;
  if (metadata.epoch > 0) doc["epoch"] = metadata.epoch;
  if (!metadata.generation.empty()) doc["generation"] = metadata.generation;
  std::string raw;
  serializeJson(doc, raw);
  const std::string tmp = path + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(raw.c_str()))) return false;
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool PluginCatalogActivity::readCacheEpoch(const std::string& path, uint32_t& epoch) const {
  epoch = 0;
  if (path.empty() || !Storage.exists(path.c_str())) return true;
  std::string raw;
  if (!Storage.readFileToString("PCAT", path, 128, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  epoch = doc["epoch"] | static_cast<uint32_t>(0);
  return true;
}

bool PluginCatalogActivity::writeCacheEpoch(const std::string& path, const uint32_t epoch) {
  if (path.empty()) return false;
  JsonDocument doc;
  doc["epoch"] = epoch;
  std::string raw;
  serializeJson(doc, raw);
  const std::string tmp = path + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), String(raw.c_str()))) return false;
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

uint32_t PluginCatalogActivity::requiredCacheEpoch() const {
  uint32_t pluginEpoch = 0;
  uint32_t catalogEpoch = 0;
  readCacheEpoch(cachePluginEpochPath(), pluginEpoch);
  readCacheEpoch(cacheCatalogEpochPath(), catalogEpoch);
  return std::max(pluginEpoch, catalogEpoch);
}

bool PluginCatalogActivity::bumpCacheEpoch(const Manifest::CacheRefreshScope scope) {
  if (scope == Manifest::CacheRefreshScope::Current) return true;
  if (!cacheUsable() || !ensureCacheDir()) return false;

  const std::string clockPath = cacheDir() + "/e_clock.meta";
  uint32_t clock = 0;
  uint32_t pluginEpoch = 0;
  uint32_t catalogEpoch = 0;
  readCacheEpoch(clockPath, clock);
  readCacheEpoch(cachePluginEpochPath(), pluginEpoch);
  readCacheEpoch(cacheCatalogEpochPath(), catalogEpoch);
  clock = std::max(clock, std::max(pluginEpoch, catalogEpoch));
  if (clock == 0xFFFFFFFFu) return false;

  const uint32_t next = clock + 1;
  if (!writeCacheEpoch(clockPath, next)) return false;
  const std::string target =
      scope == Manifest::CacheRefreshScope::Plugin ? cachePluginEpochPath() : cacheCatalogEpochPath();
  return !target.empty() && writeCacheEpoch(target, next);
}

bool PluginCatalogActivity::cacheTimestampFresh(const int64_t capturedAt) const {
  if (!cacheUsable() || manifest.cacheRefreshDays == 0) return false;
  const int64_t now = currentUnixTime();
  if (now == 0) return true;
  if (capturedAt <= 0 || capturedAt > now) return false;
  return now - capturedAt < static_cast<int64_t>(manifest.cacheRefreshDays) * 24 * 60 * 60;
}

bool PluginCatalogActivity::cacheEntryFresh(const CacheMetadata& metadata, const bool applyTtl) const {
  if (metadata.epoch < requiredCacheEpoch()) return false;
  if (!manifest.cacheGeneration.empty() && metadata.generation != manifest.cacheGeneration) return false;
  return !applyTtl || cacheTimestampFresh(metadata.capturedAt);
}

bool PluginCatalogActivity::tryLoadCachedPage(const int requestedPage, const bool allowStale) {
  if (!cacheUsable()) return false;
  const std::string path = cachePagePath(requestedPage);
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  if (!allowStale) {
    CacheMetadata metadata;
    const bool hasMetadata = readCacheMetadata(cachePageMetaPath(requestedPage), metadata);
    const bool needsMetadata =
        !manifest.cacheTokenScoped || requiredCacheEpoch() > 0 || !manifest.cacheGeneration.empty();
    if ((!hasMetadata && needsMetadata) || (hasMetadata && !cacheEntryFresh(metadata, !manifest.cacheTokenScoped)))
      return false;
  }
  page = requestedPage;
  browseSourcePath = path;
  browseLoadedFromCache = true;
  return true;
}

bool PluginCatalogActivity::storeFetchedPage(const int requestedPage) {
  if (!cacheUsable() || !ensureCacheDir() || !Storage.exists(BROWSE_TMP_PATH)) return false;
  const std::string path = cachePagePath(requestedPage);
  if (path.empty() || !Storage.replaceFile(BROWSE_TMP_PATH, path.c_str())) return false;
  browseSourcePath = path;
  browseLoadedFromCache = false;
  writeCacheMetadata(cachePageMetaPath(requestedPage),
                     CacheMetadata{currentUnixTime(), requiredCacheEpoch(), manifest.cacheGeneration});
  cacheStatsDirty = true;
  return true;
}

bool PluginCatalogActivity::tryLoadCachedTextResource(const std::string& path,
                                                      const Manifest::CacheResourcePolicy policy, const bool allowStale,
                                                      std::string& out) const {
  out.clear();
  if (!cacheUsable() || policy == Manifest::CacheResourcePolicy::Disabled || path.empty() ||
      !Storage.exists(path.c_str()))
    return false;
  if (!allowStale) {
    CacheMetadata metadata;
    if (!readCacheMetadata(cacheResourceMetaPath(path), metadata)) return false;
    const bool applyTtl = policy == Manifest::CacheResourcePolicy::Ttl && !manifest.cacheTokenScoped;
    if (!cacheEntryFresh(metadata, applyTtl)) return false;
  }
  return Storage.readFileToString("PCAT", path, MAX_ITEM_DETAILS_RESPONSE, out);
}

bool PluginCatalogActivity::storeCachedTextResource(const std::string& path, const std::string& body) {
  if (!cacheUsable() || path.empty() || body.empty() || !ensureCacheDir()) return false;
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("PCAT", tmp, file)) return false;
  const size_t written = file.write(body.data(), body.size());
  file.flush();
  const bool closed = file.close();
  if (written != body.size() || !closed) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.replaceFile(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!writeCacheMetadata(cacheResourceMetaPath(path),
                          CacheMetadata{currentUnixTime(), requiredCacheEpoch(), manifest.cacheGeneration}))
    return false;
  cacheStatsDirty = true;
  return true;
}

std::string PluginCatalogActivity::cacheAgeShort() const {
  if (cacheStats.lastRefreshAt <= 0) return "";
  const int64_t now = currentUnixTime();
  if (now == 0 || now < cacheStats.lastRefreshAt) return "?";
  const int64_t days = (now - cacheStats.lastRefreshAt) / (24 * 60 * 60);
  if (days < 1) return "<1d";
  char out[12];
  snprintf(out, sizeof(out), "%lldd", static_cast<long long>(days));
  return out;
}

void PluginCatalogActivity::refreshCacheStats() {
  cacheStats = {};
  if (!cacheUsable()) {
    cacheStatsDirty = false;
    cacheTabLabel = "Cache";
    cacheLastRefreshValue = "--";
    cachePagesValue = "0";
    cacheImagesValue = "0";
    cacheStorageValue = "0 MB";
    cacheSeedValue = tr(STR_PLUGIN_CACHE_SEED_NOT_INSTALLED);
    return;
  }

  const std::string dirPath = cacheDir();
  auto dir = Storage.open(dirPath.c_str());
  if (dir && dir.isDirectory()) {
    char name[96];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      const std::string fileName(name);
      const bool isFile = !entry.isDirectory();
      const uint64_t size = isFile ? entry.fileSize64() : 0;
      if (entry.isOpen()) entry.close();
      if (!isFile) continue;

      cacheStats.bytes += size;
      if ((fileName.rfind("i_", 0) == 0 && endsWith(fileName, ".bmp")) ||
          (fileName.rfind("r_", 0) == 0 && endsWith(fileName, ".dat"))) {
        ++cacheStats.images;
      } else if (fileName.rfind("p_", 0) == 0 && endsWith(fileName, ".json")) {
        ++cacheStats.pages;
      } else if ((fileName.rfind("p_", 0) == 0 || fileName.rfind("r_", 0) == 0) && endsWith(fileName, ".meta")) {
        CacheMetadata metadata;
        if (readCacheMetadata(dirPath + "/" + fileName, metadata))
          cacheStats.lastRefreshAt = std::max(cacheStats.lastRefreshAt, metadata.capturedAt);
      }
    }
  }
  if (dir.isOpen()) dir.close();

  char buf[48];
  cacheLastRefreshValue = cacheAgeShort();
  if (cacheLastRefreshValue.empty()) cacheLastRefreshValue = "--";
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(cacheStats.pages));
  cachePagesValue = buf;
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(cacheStats.images));
  cacheImagesValue = buf;
  snprintf(buf, sizeof(buf), "%.1f MB / %u MB", static_cast<double>(cacheStats.bytes) / (1024.0 * 1024.0),
           static_cast<unsigned>(manifest.cacheLimitMb));
  cacheStorageValue = buf;

  cacheSeedValue = tr(STR_PLUGIN_CACHE_SEED_NOT_INSTALLED);
  if (manifest.hasCacheSeed()) {
    std::string seedRaw;
    if (Storage.readFileToString("PCAT", cacheSeedMarkerPath(), 8 * 1024, seedRaw)) {
      JsonDocument seedDoc;
      if (deserializeJson(seedDoc, seedRaw) == DeserializationError::Ok) {
        const uint32_t built = seedDoc["built_images"] | static_cast<uint32_t>(0);
        if (built > 0) {
          snprintf(buf, sizeof(buf), tr(STR_PLUGIN_CACHE_SEED_IMAGES), static_cast<unsigned long>(built));
          cacheSeedValue = buf;
        }
      }
    }
  }

  cacheTabLabel = "Cache";
  for (const auto& tab : manifest.tabs) {
    if (tab.kind == Manifest::UiTab::Kind::Cache) {
      cacheTabLabel = tab.title;
      break;
    }
  }
  const std::string age = cacheAgeShort();
  if (!age.empty()) cacheTabLabel += " (" + age + ")";
  cacheStatsDirty = false;
}

void PluginCatalogActivity::noteCacheRefresh() { cacheStatsDirty = true; }

void PluginCatalogActivity::enforceCacheLimit() {
  if (!cacheUsable()) return;
  refreshCacheStats();
  const uint64_t limit = static_cast<uint64_t>(manifest.cacheLimitMb) * 1024 * 1024;
  if (cacheStats.bytes <= limit) return;

  struct Candidate {
    std::string path;
    uint32_t modified = 0;
  };
  const std::string dirPath = cacheDir();
  const std::string protectedPage = cachePagePath(page);
  std::vector<std::string> protectedImages;
  protectedImages.reserve(items.size());
  for (const auto& item : items) protectedImages.push_back(cacheImagePath(item));

  const auto collect = [&](const bool images) {
    std::vector<Candidate> candidates;
    candidates.reserve(64);
    auto dir = Storage.open(dirPath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir.isOpen()) dir.close();
      return candidates;
    }
    char name[96];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      const std::string fileName(name);
      const std::string path = dirPath + "/" + fileName;
      const bool wanted = images ? ((fileName.rfind("i_", 0) == 0 && endsWith(fileName, ".bmp")) ||
                                    (fileName.rfind("r_", 0) == 0 && endsWith(fileName, ".dat")))
                                 : (fileName.rfind("p_", 0) == 0 && endsWith(fileName, ".json"));
      const bool protectedFile =
          images ? std::find(protectedImages.begin(), protectedImages.end(), path) != protectedImages.end()
                 : path == protectedPage;
      if (wanted && !protectedFile) {
        Candidate candidate{path, entry.modificationTime()};
        if (candidates.size() < 64) {
          candidates.push_back(std::move(candidate));
        } else {
          auto newest =
              std::max_element(candidates.begin(), candidates.end(),
                               [](const Candidate& a, const Candidate& b) { return a.modified < b.modified; });
          if (candidate.modified < newest->modified) *newest = std::move(candidate);
        }
      }
      if (entry.isOpen()) entry.close();
    }
    if (dir.isOpen()) dir.close();
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.modified < b.modified; });
    return candidates;
  };

  while (cacheStats.bytes > limit) {
    auto candidates = collect(true);
    bool removingPages = false;
    if (candidates.empty()) {
      candidates = collect(false);
      removingPages = true;
    }
    if (candidates.empty()) break;
    bool removed = false;
    for (const auto& candidate : candidates) {
      if (cacheStats.bytes <= limit) break;
      if (!Storage.remove(candidate.path.c_str())) continue;
      removed = true;
      if (removingPages) {
        std::string meta = candidate.path;
        if (meta.size() >= 5) meta.replace(meta.size() - 5, 5, ".meta");
        Storage.remove(meta.c_str());
      } else {
        const std::string resourcePrefix = dirPath + "/r_";
        if (candidate.path.rfind(resourcePrefix, 0) == 0) {
          const std::string meta = cacheResourceMetaPath(candidate.path);
          Storage.remove(meta.c_str());
        }
      }
      refreshCacheStats();
    }
    if (!removed) break;
  }
}

void PluginCatalogActivity::clearCatalogCache() {
  clearPreviews();
  const std::string dirPath = cacheDir();
  size_t totalFiles = 0;

  if (!dirPath.empty() && Storage.exists(dirPath.c_str())) {
    auto dir = Storage.open(dirPath.c_str());
    if (dir && dir.isDirectory()) {
      for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (!entry.isDirectory()) ++totalFiles;
        if (entry.isOpen()) entry.close();
      }
    }
    if (dir.isOpen()) dir.close();
  }

  beginDownload("", tr(STR_PLUGIN_CACHE_CLEAR));
  downloadProgress = 0;
  downloadTotal = totalFiles;
  requestUpdateAndWait();

  size_t processed = 0;
  int lastRenderedPercent = -1;
  if (!dirPath.empty() && totalFiles > 0) {
    auto dir = Storage.open(dirPath.c_str());
    if (dir && dir.isDirectory()) {
      char name[96];
      for (auto entry = dir.openNextFile(); entry && !cancelDownload; entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
          if (entry.isOpen()) entry.close();
          continue;
        }
        entry.getName(name, sizeof(name));
        if (entry.isOpen()) entry.close();
        const std::string path = dirPath + "/" + name;
        if (!Storage.remove(path.c_str())) LOG_ERR("PCAT", "failed to remove cached file: %s", path.c_str());
        ++processed;
        downloadProgress = processed;
        downloadTotal = totalFiles;
        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
        if (mappedInput.wasHomeGesture()) cancelDownload = true;
        routeTouch(mappedInput);
        const int percent = static_cast<int>(static_cast<uint64_t>(processed) * 100 / totalFiles);
        if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + 5) {
          lastRenderedPercent = percent;
          requestUpdateAndWait();
        }
        yield();
      }
    }
    if (dir.isOpen()) dir.close();
  }

  if (!cancelDownload && !dirPath.empty() && Storage.exists(dirPath.c_str())) Storage.removeDir(dirPath.c_str());
  cacheStatsDirty = true;
  browseRefreshNeeded = true;
  releaseRows();
  state = State::BROWSING;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);
}

void PluginCatalogActivity::refreshCacheNow() {
  if (!cacheUsable()) return;
  if (!bumpCacheEpoch(manifest.cacheManualRefreshScope)) LOG_ERR("PCAT", "failed to advance cache refresh epoch");
  forceCacheRefresh = true;
  const int requestedPage = std::max(1, page);
  beginLoading();
  fetchPage(requestedPage);
}

void PluginCatalogActivity::confirmClearCache() {
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_PLUGIN_CACHE_CLEAR),
                                                         tr(STR_PLUGIN_CACHE_CLEAR_CONFIRM));
  if (!confirm) return;
  app.clearTapFlash();
  startActivityForResult(std::move(confirm), [this](const ActivityResult& result) {
    if (!result.isCancelled) clearCatalogCache();
    requestUpdate();
  });
}

int PluginCatalogActivity::cacheRowCount() const {
  if (!cacheUsable()) return 0;
  // Seed status + download action, followed by Refresh and Clear.
  return 2 + (manifest.hasCacheSeed() ? 2 : 0);
}

bool PluginCatalogActivity::isCacheSetting(const Manifest::Setting& setting) const {
  const std::string token = "{cfg." + setting.key + "}";
  return manifest.cacheRefreshTpl == token || manifest.cacheLimitMbTpl == token || manifest.cacheSeedUrl == token;
}

int PluginCatalogActivity::cacheSettingCount() const {
  int count = 0;
  for (const auto& setting : manifest.settings)
    if (settingVisible(setting) && isCacheSetting(setting)) ++count;
  return count;
}

int PluginCatalogActivity::settingsRowCount() const {
  if (cacheSettingsExpanded) return cacheSettingCount() + cacheRowCount();
  int count = 0;
  for (const auto& setting : manifest.settings)
    if (settingVisible(setting) && !isCacheSetting(setting)) ++count;
  if (cacheUsable()) ++count;  // expandable Cache section
  return count;
}

int PluginCatalogActivity::settingsManifestIndex(const int displayIndex, const bool cacheOnly) const {
  int visible = 0;
  for (size_t i = 0; i < manifest.settings.size(); ++i) {
    if (!settingVisible(manifest.settings[i])) continue;
    const bool cacheSetting = isCacheSetting(manifest.settings[i]);
    if (cacheSetting != cacheOnly) continue;
    if (visible++ == displayIndex) return static_cast<int>(i);
  }
  return -1;
}

void PluginCatalogActivity::activateSetting(const int manifestIndex) {
  if (manifestIndex < 0 || manifestIndex >= static_cast<int>(manifest.settings.size())) return;
  const auto& setting = manifest.settings[manifestIndex];
  if (!settingVisible(setting)) return;
  const std::string current = settingValue(setting);
  // Choosing a cache-seed archive is intentional: do not silently cycle to
  // the next (potentially much larger) download on a single tap.
  if (setting.type == Manifest::Setting::Type::Choice && manifest.cacheSeedUrl == "{cfg." + setting.key + "}") {
    const size_t count = std::min<size_t>(setting.options.size(), 16);
    if (count == 0) return;
    std::vector<const char*> labels;
    labels.reserve(count);
    int selected = 0;
    for (size_t i = 0; i < count; ++i) {
      labels.push_back(setting.options[i].title.c_str());
      if (setting.options[i].value == current) selected = static_cast<int>(i);
    }
    app.clearTapFlash();
    pluginHubPopup.showMessage(
        setting.title.c_str(),
        "Larger cache seeds take longer to install. Installation can take several minutes, not seconds.", labels.data(),
        static_cast<int>(count), selected, [this, manifestIndex](const int choice) {
          const auto& selectedSetting = manifest.settings[manifestIndex];
          if (choice < 0 || choice >= static_cast<int>(selectedSetting.options.size())) return;
          const std::string& value = selectedSetting.options[choice].value;
          if (value != settingValue(selectedSetting) && !saveSettingValue(selectedSetting, value))
            LOG_ERR("PCAT", "failed to save setting %s", selectedSetting.key.c_str());
        });
    requestUpdate();
    return;
  }
  std::string next;
  if (setting.type == Manifest::Setting::Type::Toggle) {
    next = (strcasecmp(current.c_str(), "true") == 0 || current == "1") ? "false" : "true";
  } else {
    size_t option = 0;
    for (size_t i = 0; i < setting.options.size(); ++i) {
      if (setting.options[i].value == current) {
        option = (i + 1) % setting.options.size();
        break;
      }
    }
    if (setting.options.empty()) return;
    next = setting.options[option].value;
  }
  if (!saveSettingValue(setting, next)) LOG_ERR("PCAT", "failed to save setting %s", setting.key.c_str());
  requestUpdate();
}

void PluginCatalogActivity::activateCacheRow(const int index) {
  if (!cacheUsable()) return;
  if (manifest.hasCacheSeed() && index == 1) {
    installCacheSeed();
    return;
  }
  const int actionOffset = manifest.hasCacheSeed() ? 2 : 0;
  if (index == actionOffset)
    refreshCacheNow();
  else if (index == actionOffset + 1)
    confirmClearCache();
}

std::string PluginCatalogActivity::cacheSeedMarkerPath() const {
  const std::string dir = cacheDir();
  return dir.empty() ? "" : dir + "/seed.json";
}

bool PluginCatalogActivity::extractCacheSeed(const std::string& archivePath) {
  if (!ensureCacheDir()) return false;

  constexpr size_t BATCH_SIZE = 64;
  constexpr size_t MAX_SEED_IMAGES = 4096;
  constexpr size_t MAX_SEED_PAGES = 1024;
  constexpr size_t MAX_SEED_IMAGE_BYTES = 64 * 1024;
  constexpr size_t MAX_SEED_PAGE_BYTES = MAX_BROWSE_RESPONSE;
  constexpr size_t MAX_SEED_META_BYTES = 512;
  constexpr size_t MAX_SEED_SUMMARY_BYTES = 8 * 1024;
  constexpr size_t ZIP_CHUNK_SIZE = 1024;
  constexpr std::string_view IMAGE_PREFIX = ".cache/i_";
  constexpr std::string_view PAGE_PREFIX = ".cache/p_";
  constexpr std::string_view IMAGE_SUFFIX = ".bmp";
  constexpr std::string_view PAGE_SUFFIX = ".json";
  constexpr std::string_view META_SUFFIX = ".meta";

  enum class SeedEntryKind : uint8_t { None, Image, Page, Meta };
  const auto matchesFlatPath = [&](const std::string_view path, const std::string_view prefix,
                                   const std::string_view suffix) {
    if (path.size() <= prefix.size() + suffix.size() || path.substr(0, prefix.size()) != prefix ||
        path.substr(path.size() - suffix.size()) != suffix)
      return false;
    return path.find("/", prefix.size()) == std::string_view::npos && path.find("..") == std::string_view::npos;
  };
  const auto classifyPath = [&](const std::string_view path) {
    if (matchesFlatPath(path, IMAGE_PREFIX, IMAGE_SUFFIX)) return SeedEntryKind::Image;
    if (matchesFlatPath(path, PAGE_PREFIX, PAGE_SUFFIX)) return SeedEntryKind::Page;
    if (matchesFlatPath(path, PAGE_PREFIX, META_SUFFIX)) return SeedEntryKind::Meta;
    return SeedEntryKind::None;
  };

  size_t totalImages = 0;
  size_t totalPages = 0;
  size_t totalMetas = 0;
  bool tooMany = false;
  {
    ZipFile listing(archivePath);
    const bool listed = listing.enumerateFilePaths([&](const std::string_view path) {
      switch (classifyPath(path)) {
        case SeedEntryKind::Image:
          if (totalImages >= MAX_SEED_IMAGES)
            tooMany = true;
          else
            ++totalImages;
          break;
        case SeedEntryKind::Page:
          if (totalPages >= MAX_SEED_PAGES)
            tooMany = true;
          else
            ++totalPages;
          break;
        case SeedEntryKind::Meta:
          if (totalMetas >= MAX_SEED_PAGES)
            tooMany = true;
          else
            ++totalMetas;
          break;
        case SeedEntryKind::None:
          break;
      }
    });
    if (!listed || tooMany || totalImages == 0 || totalPages != totalMetas) {
      LOG_ERR("PCAT", "cache seed zip listing invalid: images=%u pages=%u meta=%u", static_cast<unsigned>(totalImages),
              static_cast<unsigned>(totalPages), static_cast<unsigned>(totalMetas));
      return false;
    }
  }

  const size_t totalEntries = totalImages + totalPages + totalMetas;
  const std::string dir = cacheDir();
  size_t skip = 0;
  size_t extractedCount = 0;
  size_t extractedImages = 0;
  size_t extractedPages = 0;
  ZipFile extractor(archivePath);
  if (!extractor.open()) {
    LOG_ERR("PCAT", "cache seed zip open failed");
    return false;
  }

  size_t summarySize = 0;
  if (!extractor.getInflatedFileSize("wallpaper-cache-seed.json", &summarySize) || summarySize == 0 ||
      summarySize > MAX_SEED_SUMMARY_BYTES) {
    LOG_ERR("PCAT", "cache seed summary size invalid: %u", static_cast<unsigned>(summarySize));
    extractor.close();
    return false;
  }

  downloadProgress = 0;
  downloadTotal = totalEntries;
  requestUpdateAndWait();

  const auto checkCancelInput = [this] {
    mappedInput.update(true);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture())
      cancelDownload = true;
    routeTouch(mappedInput);
    return cancelDownload;
  };

  while (extractedCount < totalEntries) {
    std::vector<std::string> batch;
    batch.reserve(BATCH_SIZE);
    size_t eligible = 0;
    ZipFile listing(archivePath);
    const bool listed = listing.enumerateFilePaths([&](const std::string_view path) {
      if (classifyPath(path) == SeedEntryKind::None) return;
      const size_t itemIndex = eligible++;
      if (itemIndex < skip || batch.size() >= BATCH_SIZE) return;
      batch.emplace_back(path);
    });
    if (!listed || batch.empty()) {
      LOG_ERR("PCAT", "cache seed zip batch listing failed at %u", static_cast<unsigned>(skip));
      extractor.close();
      return false;
    }

    {
      // InflateStream can use the framebuffer loan as scratch for legacy deflated
      // seeds. Current seeds use stored ZIP entries and need no DEFLATE window.
      GfxRenderer::FrameBufferLoan framebufferLoan(renderer);
      for (const auto& entry : batch) {
        const SeedEntryKind kind = classifyPath(entry);
        const size_t slash = entry.find_last_of("/");
        const std::string filename = slash == std::string::npos ? entry : entry.substr(slash + 1);
        const std::string dest = dir + "/" + filename;
        const std::string tmp = dest + ".seedtmp";
        bool ok = false;
        {
          HalFile out;
          if (Storage.openFileForWrite("PCAT", tmp, out))
            ok = extractor.readFileToStream(entry.c_str(), out, ZIP_CHUNK_SIZE);
        }

        size_t extractedSize = 0;
        if (ok) {
          HalFile check;
          if (Storage.openFileForRead("PCAT", tmp, check)) extractedSize = check.fileSize();
        }
        const bool sizeValid =
            kind == SeedEntryKind::Image ? extractedSize >= 62 && extractedSize <= MAX_SEED_IMAGE_BYTES
            : kind == SeedEntryKind::Page
                ? extractedSize > 0 && extractedSize <= MAX_SEED_PAGE_BYTES
                : kind == SeedEntryKind::Meta && extractedSize > 0 && extractedSize <= MAX_SEED_META_BYTES;
        if (!ok || !sizeValid || !Storage.replaceFile(tmp.c_str(), dest.c_str())) {
          Storage.remove(tmp.c_str());
          LOG_ERR("PCAT", "cache seed entry failed: %s (%u bytes)", entry.c_str(),
                  static_cast<unsigned>(extractedSize));
          extractor.close();
          return false;
        }
        ++extractedCount;
        if (kind == SeedEntryKind::Image) ++extractedImages;
        if (kind == SeedEntryKind::Page) ++extractedPages;
        if (checkCancelInput()) break;
        yield();
      }
    }

    if (cancelDownload) {
      extractor.close();
      return false;
    }
    skip += batch.size();
    downloadProgress = extractedCount;
    downloadTotal = totalEntries;
    requestUpdateAndWait();

    if (checkCancelInput()) {
      extractor.close();
      return false;
    }
  }

  const std::string markerPath = cacheSeedMarkerPath();
  const std::string markerTmp = markerPath + ".tmp";
  bool markerOk = false;
  {
    GfxRenderer::FrameBufferLoan framebufferLoan(renderer);
    HalFile out;
    if (Storage.openFileForWrite("PCAT", markerTmp, out))
      markerOk = extractor.readFileToStream("wallpaper-cache-seed.json", out, ZIP_CHUNK_SIZE);
  }
  extractor.close();
  if (!markerOk || !Storage.replaceFile(markerTmp.c_str(), markerPath.c_str())) {
    Storage.remove(markerTmp.c_str());
    LOG_ERR("PCAT", "cache seed summary missing or invalid");
    return false;
  }

  LOG_INF("PCAT", "cache seed installed: %u images, %u pages", static_cast<unsigned>(extractedImages),
          static_cast<unsigned>(extractedPages));
  return extractedCount == totalEntries;
}

void PluginCatalogActivity::installCacheSeed() {
  if (!manifest.hasCacheSeed()) return;
  if (!wifiConnected()) {
    pendingCacheSeedInstall = true;
    launchWifiSelection();
    return;
  }

  beginDownload(tr(STR_PLUGIN_CACHE_SEED));
  requestUpdateAndWait();
  session.reset();
  Storage.remove(CACHE_SEED_TMP_PATH);
  const auto result = downloadFile(substituted(manifest.cacheSeedUrl, nullptr), CACHE_SEED_TMP_PATH);
  if (result == HttpDownloader::ABORTED) {
    Storage.remove(CACHE_SEED_TMP_PATH);
    state = State::BROWSING;
    requestUpdate();
    return;
  }
  if (result != HttpDownloader::OK) {
    Storage.remove(CACHE_SEED_TMP_PATH);
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }

  downloadHeading = tr(STR_PLUGIN_CACHE_SEED_INSTALLING);
  statusMessage.clear();
  requestUpdateAndWait();
  const bool installed = extractCacheSeed(CACHE_SEED_TMP_PATH);
  Storage.remove(CACHE_SEED_TMP_PATH);
  if (!installed) {
    if (cancelDownload) {
      state = State::BROWSING;
      requestUpdate();
      return;
    }
    fail(StrId::STR_PLUGIN_CACHE_SEED_INSTALL_FAILED);
    return;
  }

  cacheStatsDirty = true;
  enforceCacheLimit();
  releaseRows();
  state = State::BROWSING;
  requestUpdate();
}

void PluginCatalogActivity::clearPreviews() {
  if (previewFile.isOpen()) previewFile.close();
  Storage.remove(PREVIEW_TMP_PATH);
  for (int i = 0; i < MAX_PAGE_SIZE; ++i) Storage.remove(previewPath(i).c_str());
  previewReadyMask = 0;
}

bool PluginCatalogActivity::cachedPreviewValid(const std::string& path) const {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("PCAT", path, file)) return false;
  Bitmap bitmap(file);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  file.close();
  return valid;
}

bool PluginCatalogActivity::fetchPreviewToPath(const Item& item, const std::string& dest, const int targetWidth,
                                               const int targetHeight) {
  if (dest.empty()) return false;
  const auto req = substitutedRequest(manifest.imageReq, &item);
  if (req.url.empty()) return false;

  Storage.remove(PREVIEW_TMP_PATH);
  const int status = pluginhttp::requestToFile(nullptr, req.url, req.method, req.body, req.headers, PREVIEW_TMP_PATH,
                                               MAX_PREVIEW_RESPONSE);
  if (status < 200 || status >= 300) {
    LOG_ERR("PCAT", "thumbnail fetch failed: status=%d id=%s", status, item.id.c_str());
    Storage.remove(PREVIEW_TMP_PATH);
    return false;
  }

  auto imageFormat = manifest.imageFormat;
  if (imageFormat == Manifest::ImageFormat::Auto) {
    switch (detectImageFileKind(PREVIEW_TMP_PATH)) {
      case ImageFileKind::Bmp:
        imageFormat = Manifest::ImageFormat::Bmp;
        break;
      case ImageFileKind::Jpeg:
        imageFormat = Manifest::ImageFormat::Jpeg;
        break;
      case ImageFileKind::Png:
        imageFormat = Manifest::ImageFormat::Png;
        break;
      default:
        LOG_ERR("PCAT", "unsupported thumbnail image: id=%s", item.id.c_str());
        Storage.remove(PREVIEW_TMP_PATH);
        return false;
    }
  }

  Storage.remove(dest.c_str());
  bool ok = false;
  if (imageFormat == Manifest::ImageFormat::Bmp) {
    ok = Storage.rename(PREVIEW_TMP_PATH, dest.c_str()) && cachedPreviewValid(dest);
    if (!ok) Storage.remove(dest.c_str());
  } else {
    HalFile src, dst;
    if (Storage.openFileForRead("PCAT", PREVIEW_TMP_PATH, src) && Storage.openFileForWrite("PCAT", dest, dst)) {
      ok = imageFormat == Manifest::ImageFormat::Png
               ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight, false)
               : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight, false);
    }
    if (src.isOpen()) src.close();
    if (dst.isOpen()) dst.close();
    Storage.remove(PREVIEW_TMP_PATH);
    if (!ok || !cachedPreviewValid(dest)) {
      ok = false;
      LOG_ERR("PCAT", "thumbnail conversion failed: id=%s format=%d", item.id.c_str(), static_cast<int>(imageFormat));
      Storage.remove(dest.c_str());
    }
  }
  return ok;
}

bool PluginCatalogActivity::preparePreviews() {
  if (!wantsPreviewImages()) return true;

  const bool persistent = cacheUsable();
  if (persistent) ensureCacheDir();
  // Persistent thumbnails use one grid-quality conversion so switching between
  // List and Grid never requires another network fetch for the same image.
  const int targetWidth =
      persistent ? GRID_PREVIEW_WIDTH : (manifest.gridView ? GRID_PREVIEW_WIDTH : LIST_PREVIEW_WIDTH);
  const int targetHeight =
      persistent ? GRID_PREVIEW_HEIGHT : (manifest.gridView ? GRID_PREVIEW_HEIGHT : LIST_PREVIEW_HEIGHT);
  bool needsNetwork = false;
  bool releasedSession = false;
  for (size_t i = 0; i < items.size() && i < MAX_PAGE_SIZE; ++i) {
    const std::string dest = persistent ? cacheImagePath(items[i]) : previewPath(static_cast<int>(i));
    if (!dest.empty() && Storage.exists(dest.c_str())) {
      // Persistent thumbnails are already in the renderer-native BMP cache format.
      // Avoid opening every cached image once to validate and again to render it;
      // paintPreview() invalidates the cache file if the actual read/draw fails.
      previewReadyMask |= static_cast<uint16_t>(1u << i);
      continue;
    }
    if (dest.empty()) continue;

    // Missing previews are not an offline mode: catalogs that expose remote
    // images require Wi-Fi. Seed/cache hits render immediately, then any gaps
    // are filled over the network. If the connection dropped, resume this same
    // page after Wi-Fi selection instead of leaving permanent placeholders.
    if (!wifiConnected()) {
      needsNetwork = true;
      continue;
    }

    // Release the persistent browse TLS client before image conversion. The
    // decoder needs a contiguous heap block; keeping the browse client alive can
    // make otherwise-valid thumbnails fail conversion and leave gray slots.
    if (!releasedSession) {
      session.reset();
      releasedSession = true;
    }
    if (fetchPreviewToPath(items[i], dest, targetWidth, targetHeight))
      previewReadyMask |= static_cast<uint16_t>(1u << i);
  }
  if (persistent) cacheStatsDirty = true;
  if (releasedSession) {
    session = makeUniqueNoThrow<freeink::SecureHttpClient>();
    if (!session) LOG_ERR("PCAT", "OOM: browse client after thumbnails; using per-request connections");
  }

  if (needsNetwork) {
    pendingPreviewFill = true;
    launchWifiSelection();
    return false;
  }
  pendingPreviewFill = false;
  return true;
}

bool PluginCatalogActivity::paintPreview(const fui::Rect rect, const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= MAX_PAGE_SIZE || itemIndex >= static_cast<int>(items.size()) ||
      !(previewReadyMask & (1u << itemIndex)))
    return false;
  if (previewFile.isOpen()) previewFile.close();
  const std::string path = cacheUsable() ? cacheImagePath(items[itemIndex]) : previewPath(itemIndex);
  const auto invalidatePreview = [&] {
    previewReadyMask &= static_cast<uint16_t>(~static_cast<uint16_t>(1u << itemIndex));
    if (cacheUsable() && !path.empty()) {
      Storage.remove(path.c_str());
      cacheStatsDirty = true;
    }
  };
  if (path.empty() || !Storage.openFileForRead("PCAT", path, previewFile)) {
    invalidatePreview();
    return false;
  }
  if (previewBitmap.parseHeaders() != BmpReaderError::Ok) {
    if (previewFile.isOpen()) previewFile.close();
    invalidatePreview();
    return false;
  }
  const float scale = std::min(static_cast<float>(rect.width) / previewBitmap.getWidth(),
                               static_cast<float>(rect.height) / previewBitmap.getHeight());
  const int drawWidth = std::max(1, std::min<int>(rect.width, static_cast<int>(previewBitmap.getWidth() * scale)));
  const int drawHeight = std::max(1, std::min<int>(rect.height, static_cast<int>(previewBitmap.getHeight() * scale)));
  const int drawX = rect.x + (rect.width - drawWidth) / 2;
  const int drawY = rect.y + (rect.height - drawHeight) / 2;
  const bool allowUpscale = drawWidth > previewBitmap.getWidth() || drawHeight > previewBitmap.getHeight();
  const bool drawn = renderer.drawBitmap(previewBitmap, drawX, drawY, drawWidth, drawHeight, 0, 0, false, allowUpscale);
  if (previewFile.isOpen()) previewFile.close();
  if (!drawn) invalidatePreview();

  if (drawn) {
    if (manifest.gridView) {
      std::string dest;
      if (downloadDestination(items[itemIndex], dest) && Storage.exists(dest.c_str())) {
        constexpr char badge[] = "SAVED";
        constexpr int padX = 6;
        constexpr int padY = 4;
        constexpr int inset = 7;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, badge, EpdFontFamily::BOLD);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int badgeWidth = textWidth + padX * 2;
        const int badgeHeight = textHeight + padY * 2;
        const int badgeX = drawX + drawWidth - badgeWidth - inset;
        const int badgeY = drawY + inset;
        renderer.fillRect(badgeX, badgeY, badgeWidth, badgeHeight, false);
        renderer.drawRect(badgeX, badgeY, badgeWidth, badgeHeight);
        renderer.drawText(SMALL_FONT_ID, badgeX + padX, badgeY + padY, badge, true, EpdFontFamily::BOLD);
      }
    }
    if (manifest.coverRadius > 0)
      renderer.maskRoundedRectOutsideCorners(drawX, drawY, drawWidth, drawHeight, manifest.coverRadius);
  }
  return drawn;
}

void PluginCatalogActivity::fetchPage(const int newPage) {
  browseRequestedTop.store(-1);
  browseViewportTop.store(0);
  browseViewportItems.store(1);
  if (!manifest.isXmlList()) page = newPage;
  clearPreviews();
  browseSourcePath = BROWSE_TMP_PATH;
  browseLoadedFromCache = false;

  if (manifest.cacheTokenScoped) loadToken();
  const bool canCache = cacheUsable();
  if (canCache && !forceCacheRefresh && tryLoadCachedPage(newPage, false)) {
    if (!parseBrowseResponse()) return;
    if (!preparePreviews()) return;
    state = State::BROWSING;
    activeNav().reset(nativeFooterActive() ? contentSelectionOffset() : 0);
    requestUpdate();
    return;
  }

  if (!wifiConnected()) {
    pendingNetworkPage = newPage;
    launchWifiSelection();
    return;
  }

  const bool fetched = fetchBrowseResponse();
  const bool authFailed = state == State::NO_TOKEN;
  if (!fetched) {
    forceCacheRefresh = false;
    if (!authFailed && canCache && manifest.cacheStaleFallback && tryLoadCachedPage(newPage, true)) {
      if (!parseBrowseResponse()) return;
      if (!preparePreviews()) return;
      state = State::BROWSING;
      activeNav().reset(nativeFooterActive() ? contentSelectionOffset() : 0);
      requestUpdate();
      return;
    }
    if (authFailed && manifest.hasDeviceCode() && !manifest.tokenPersistent) {
      if (!clearEphemeralToken()) {
        fail(StrId::STR_PLUGIN_AUTH_FAILED);
        return;
      }
      startDeviceAuth();
      return;
    }
    if (!authFailed) fail(StrId::STR_FETCH_FEED_FAILED);
    return;
  }

  forceCacheRefresh = false;
  const bool canStoreCache = cacheUsable();
  if (canStoreCache && !storeFetchedPage(newPage)) browseSourcePath = BROWSE_TMP_PATH;
  if (!parseBrowseResponse()) return;
  if (!preparePreviews()) return;
  if (canStoreCache) {
    enforceCacheLimit();
    noteCacheRefresh();
  }
  state = State::BROWSING;
  activeNav().reset(nativeFooterActive() ? contentSelectionOffset() : 0);
  requestUpdate();
}

bool PluginCatalogActivity::parseBrowseResponse() {
  if (manifest.isXmlList()) return parseXmlList();

  const char* source = browseSourcePath.empty() ? BROWSE_TMP_PATH : browseSourcePath.c_str();
  const bool extension = currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
                         manifest.browseLists[currentList].kind != "plugin";
  const bool filterCompatibility = !extension && manifest.filtersCompatibility();
  const std::string extTitle = "title", extAuthor = "description", extId = "name", extUrl = "asset_url";
  const std::string& titles = extension ? extTitle : manifest.titlePath;
  const std::string& authors = extension ? extAuthor : manifest.authorPath;
  const std::string& ids = extension ? extId : manifest.idPath;
  const std::string& urls = extension ? extUrl : manifest.urlPath;
  const std::string& itemsPath = extension ? manifest.browseLists[currentList].itemsPath : manifest.itemsPath;

  // Server-paged API requests substitute {page}; static JSON feeds must
  // advance through the streamed response locally, without retaining all rows.
  const bool localPaging =
      extension || plugincatalogpaging::isStaticFeed(activeBrowseUrl(), activeBrowseBody(), manifest.browseReq.headers);
  const size_t pageOffset =
      localPaging ? static_cast<size_t>(std::max(0, page - 1)) * static_cast<size_t>(manifest.pageSize) : 0;

  // Filter first, then page over the compatible items in a static feed.
  // Both passes retain at most one page of row indexes or decoded items.
  std::vector<int> compatibleRows;
  bool rawHasMore = false;
  if (filterCompatibility) {
    compatibleRows.reserve(manifest.pageSize);
    int rowIndex = 0;
    plugincatalogpaging::PageWindow filteredWindow(pageOffset, manifest.pageSize);
    plugincatalogpaging::PageWindow serverWindow(0, manifest.pageSize);
    const std::string unused;
    auto selectCompatible = [&](JsonListParser::Row& row) {
      const int thisRow = rowIndex++;
      if (row.field[JsonListParser::F_TITLE].empty()) return;
      const bool compatible =
          catalogEntryCompatible(row.field[JsonListParser::F_AUTHOR], row.field[JsonListParser::F_ID],
                                 row.field[JsonListParser::F_URL], row.field[JsonListParser::F_VERSION]);
      if (localPaging) {
        if (filteredWindow.take(compatible)) compatibleRows.push_back(thisRow);
      } else if (serverWindow.take() && compatible) {
        compatibleRows.push_back(thisRow);
      }
    };
    const std::string* const compatibilityFields[] = {&manifest.titlePath,          &manifest.compatFamilyPath,
                                                     &manifest.compatFirmwarePath, &manifest.compatApiMinPath,
                                                     &manifest.compatRequiresPath, &unused};
    auto compatibilityParser = makeUniqueNoThrow<JsonListParser>(
        itemsPath, compatibilityFields, unused,
        [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(selectCompatible)*>(context))(row); },
        &selectCompatible);
    if (!streamBrowseFile(compatibilityParser.get(), source)) {
      fail(StrId::STR_PARSE_FEED_FAILED);
      return false;
    }
    rawHasMore = localPaging ? filteredWindow.hasMore() : serverWindow.hasMore();
  }

  releaseRows();
  items.clear();
  items.reserve(manifest.pageSize + 1);
  int rowIndex = 0;
  plugincatalogpaging::PageWindow visibleWindow(pageOffset, manifest.pageSize);
  size_t compatibleIndex = 0;
  auto append = [&](JsonListParser::Row& row) {
    const int thisRow = rowIndex++;
    if (filterCompatibility) {
      if (compatibleIndex >= compatibleRows.size() || compatibleRows[compatibleIndex] != thisRow) return;
      compatibleIndex++;
    } else if (!visibleWindow.take(!row.field[JsonListParser::F_TITLE].empty())) {
      return;
    }
    if (row.field[JsonListParser::F_TITLE].empty()) return;
    Item item;
    item.title = std::move(row.field[JsonListParser::F_TITLE]);
    item.author = std::move(row.field[JsonListParser::F_AUTHOR]);
    item.id = std::move(row.field[JsonListParser::F_ID]);
    item.url = std::move(row.field[JsonListParser::F_URL]);
    item.version = std::move(row.field[JsonListParser::F_VERSION]);
    item.base = std::move(row.field[JsonListParser::F_BASE]);
    item.files = std::move(row.files);
    items.push_back(std::move(item));
  };
  // Streamed straight from the SD temp file: neither the raw response nor a
  // parsed document occupies DRAM, only the rows kept.
  const std::string* const fields[] = {&titles, &authors, &ids, &urls, &manifest.versionPath, &manifest.bundleBasePath};
  auto parser = makeUniqueNoThrow<JsonListParser>(
      itemsPath, fields, manifest.bundleFilesPath,
      [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(append)*>(context))(row); }, &append);
  if (!streamBrowseFile(parser.get(), source)) {
    fail(StrId::STR_PARSE_FEED_FAILED);
    return false;
  }
  parser.reset();

  // A catalog may carry the exact preview URL as an item field (for example a
  // sharded hosted thumbnail). JsonListParser has a fixed field set shared with
  // bundle catalogs, so collect this optional field in a second streaming pass
  // instead of repurposing the bundle base slot in the primary parse.
  if (!manifest.thumbnailPath.empty() && !items.empty()) {
    const std::string emptyPath;
    const std::string* const thumbnailFields[] = {&manifest.titlePath, &emptyPath, &emptyPath,
                                                  &emptyPath,          &emptyPath, &manifest.thumbnailPath};
    size_t thumbnailRow = 0;
    int thumbnailSourceRow = 0;
    size_t thumbnailCompatibleIndex = 0;
    plugincatalogpaging::PageWindow thumbnailWindow(pageOffset, manifest.pageSize);
    auto appendThumbnail = [&](JsonListParser::Row& row) {
      const int thisRow = thumbnailSourceRow++;
      if (filterCompatibility) {
        if (thumbnailCompatibleIndex >= compatibleRows.size() || compatibleRows[thumbnailCompatibleIndex] != thisRow)
          return;
        thumbnailCompatibleIndex++;
      } else if (!thumbnailWindow.take(!row.field[JsonListParser::F_TITLE].empty())) {
        return;
      }
      if (row.field[JsonListParser::F_TITLE].empty() || thumbnailRow >= items.size()) return;
      items[thumbnailRow++].thumbnail = std::move(row.field[JsonListParser::F_BASE]);
    };
    auto thumbnailParser = makeUniqueNoThrow<JsonListParser>(
        itemsPath, thumbnailFields, std::string(),
        [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(appendThumbnail)*>(context))(row); },
        &appendThumbnail);
    if (!streamBrowseFile(thumbnailParser.get(), source)) {
      fail(StrId::STR_PARSE_FEED_FAILED);
      return false;
    }
  }

  hasMore = filterCompatibility ? rawHasMore : visibleWindow.hasMore();
  computeInstallStatus();
  return true;
}

// Badge each item by comparing its catalog version to the installed copy's
// manifest (located by folder id across the plugin roots). Only a newer
// catalog version is an update, so an older catalog never offers a downgrade.
// Runs once per page.
void PluginCatalogActivity::computeInstallStatus() {
  if (!manifest.tracksInstalls()) return;
  if (currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
      manifest.browseLists[currentList].kind != "plugin") {
    const auto& list = manifest.browseLists[currentList];
    const auto kind = list.kind == "theme-pack"       ? extensionpackages::Kind::Theme
                      : list.kind == "wallpaper-pack" ? extensionpackages::Kind::Wallpaper
                                                      : extensionpackages::Kind::Plugin;
    const std::string root =
        kind == extensionpackages::Kind::Theme ? "/.crosspoint/themes/" : "/.crosspoint/wallpapers/";
    const std::string suffix = kind == extensionpackages::Kind::Theme ? ".crosspoint-theme" : ".crosspoint-wallpaper";
    const char* source = browseSourcePath.empty() ? BROWSE_TMP_PATH : browseSourcePath.c_str();
    const uint16_t width = std::min(renderer.getDisplayWidth(), renderer.getDisplayHeight());
    const uint16_t height = std::max(renderer.getDisplayWidth(), renderer.getDisplayHeight());
    JsonDocument filter;
    filter["name"] = true;
    filter["version"] = true;
    filter["kind"] = true;
    for (auto& item : items) {
      item.status = item.version.empty() ? "" : "v" + item.version;
      if (kind == extensionpackages::Kind::Plugin || item.id.empty()) continue;
      // Catalog IDs are collection IDs. Wallpaper SD directories instead use
      // the single package ID matching this panel's portrait resolution.
      extensionpackages::Release release;
      if (!extensionpackages::selectRelease(source, kind, item.id, width, height, release)) continue;
      std::string raw;
      if (!Storage.readFileToString("PCAT", root + release.name + suffix + "/manifest.json", MAX_MANIFEST_SIZE, raw))
        continue;
      JsonDocument installed;
      if (deserializeJson(installed, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
      if (release.name != (installed["name"] | "") ||
          strcmp(installed["kind"] | "", kind == extensionpackages::Kind::Theme ? "theme-pack" : "wallpaper-pack") != 0)
        continue;
      const std::string installedVersion = installed["version"] | "";
      if (installedVersion.empty()) continue;
      item.status = PluginLocations::isNewerVersion(item.version, installedVersion) ? tr(STR_UPDATE_AVAILABLE)
                                                                                    : tr(STR_INSTALLED);
    }
    return;
  }
  JsonDocument filter;
  filter["version"] = true;
  for (auto& item : items) {
    item.status.clear();
    if (item.id.empty()) continue;
    // Locate the install across every plugin root (not just one), matching
    // the discovery the rest of the firmware uses.
    const std::string dir = PluginLocations::findPluginDir(item.id.c_str());
    std::string raw;
    if (dir.empty() || !Storage.readFileToString("PCAT", dir + "/manifest.json", MAX_MANIFEST_SIZE, raw)) {
      // Not installed: show the available version so the row is not blank.
      if (!item.version.empty()) item.status = "v" + item.version;
      continue;
    }
    JsonDocument doc;
    std::string installed;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
      installed = doc["version"] | "";
    }
    item.status =
        PluginLocations::isNewerVersion(item.version, installed) ? tr(STR_UPDATE_AVAILABLE) : tr(STR_INSTALLED);
  }
}

void PluginCatalogActivity::beginAuth() {
  beginLoading();

  String response;
  const int status = apiRequest(substitutedRequest(manifest.authReq), response);
  JsonDocument doc;
  if (status < 200 || status >= 300 || deserializeJson(doc, response) != DeserializationError::Ok) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  authUserCode = variantToString(resolvePath(root, manifest.authCodePath));
  authVerifyUrl = variantToString(resolvePath(root, manifest.authVerifyPath));
  authDeviceCode = variantToString(resolvePath(root, manifest.authDeviceCodePath));
  const long interval = resolvePath(root, manifest.authIntervalPath) | 5L;
  const long expires = resolvePath(root, manifest.authExpiresPath) | 900L;
  if (authUserCode.empty() || authDeviceCode.empty()) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  authIntervalMs = (interval < 3 ? 3 : interval) * 1000UL;
  authDeadlineMs = millis() + (expires < 60 ? 60 : expires) * 1000UL;
  authNextPollMs = millis() + authIntervalMs;
  state = State::AUTH;
  requestUpdate();
}

void PluginCatalogActivity::pollAuth() {
  // Checked before polling so an expired code fails even while every poll
  // hits a transport error (offline).
  if (static_cast<long>(millis() - authDeadlineMs) >= 0) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  authNextPollMs = millis() + authIntervalMs;

  auto req = substitutedRequest(manifest.pollReq);
  substituteAll(req.url, "{device_code}", authDeviceCode);
  substituteAll(req.body, "{device_code}", authDeviceCode);

  String response;
  const int status = apiRequest(req, response);
  if (status < 0) return;  // transient transport failure: keep polling

  JsonDocument doc;
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    const JsonVariantConst root = doc.as<JsonVariantConst>();
    const std::string newToken = variantToString(resolvePath(root, manifest.authTokenPath));
    if (!newToken.empty()) {
      if (!saveToken(newToken)) {
        fail(StrId::STR_PLUGIN_AUTH_FAILED);
        return;
      }
      token = newToken;
      startBrowse();
      return;
    }
    const std::string code = variantToString(resolvePath(root, manifest.authErrorPath));
    if (code == "slow_down") {
      authIntervalMs += 5000;
    } else if (code == "expired_token" || code == "access_denied") {
      fail(StrId::STR_PLUGIN_AUTH_FAILED);
      return;
    }
    // authorization_pending (or anything unrecognized): keep polling
  }
}

void PluginCatalogActivity::downloadItem(const int itemIndex) {
  // Own only the selected item's metadata while TLS needs the catalog's heap.
  Item item;
  {
    RenderLock lock;
    item = std::move(items[itemIndex]);
    beginDownload(item.title);
    releaseRows();
    std::vector<fui::ListItem>().swap(rowItems);
    std::vector<Item>().swap(items);
  }
  requestUpdateAndWait();
  const bool extension = currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
                         manifest.browseLists[currentList].kind != "plugin";
  const auto result = extension
                          ? downloadExtension(item)
                          : (manifest.isBundle() && !item.files.empty() ? downloadBundle(item) : downloadBook(item));
  session.reset();
  item = {};
  // Reuse the SD response without another network request or resetting navigation.
  if (!parseBrowseResponse()) return;
  session = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!session) LOG_ERR("PCAT", "OOM: browse client; using per-request connections");
  finishDownload(result);
}

void PluginCatalogActivity::downloadFinished(const bool cancelled) {
  state = cancelled ? State::BROWSING : State::DONE;
  requestUpdate();
}

void PluginCatalogActivity::confirmRemoveExtension(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size()) || currentList < 0 ||
      currentList >= static_cast<int>(manifest.browseLists.size()))
    return;
  const std::string& kindName = manifest.browseLists[currentList].kind;
  const auto kind = kindName == "theme-pack"       ? extensionpackages::Kind::Theme
                    : kindName == "wallpaper-pack" ? extensionpackages::Kind::Wallpaper
                                                   : extensionpackages::Kind::Plugin;
  if (kind == extensionpackages::Kind::Plugin) return;
  const Item& item = items[itemIndex];
  const char* source = browseSourcePath.empty() ? BROWSE_TMP_PATH : browseSourcePath.c_str();
  extensionpackages::Release release;
  if (!extensionpackages::selectRelease(source, kind, item.id,
                                        std::min(renderer.getDisplayWidth(), renderer.getDisplayHeight()),
                                        std::max(renderer.getDisplayWidth(), renderer.getDisplayHeight()), release))
    return;
  const std::string root = kind == extensionpackages::Kind::Theme ? "/.crosspoint/themes/" : "/.crosspoint/wallpapers/";
  const std::string path =
      root + release.name + (kind == extensionpackages::Kind::Theme ? ".crosspoint-theme" : ".crosspoint-wallpaper");
  if (!Storage.exists(path.c_str())) return;
  const bool selected = kind == extensionpackages::Kind::Theme &&
                        std::string(SETTINGS.sdThemeId).rfind(release.name + "/", 0) == 0;
  const bool selectedWallpaper = kind == extensionpackages::Kind::Wallpaper &&
                                 strcmp(SETTINGS.wallpaperPackId, release.name.c_str()) == 0;
  const std::string title = "Remove " + item.title;
  const std::string message =
      selected ? "Switch to another theme before removing it."
               : selectedWallpaper ? "Remove the pack? All Wallpapers will be selected instead."
                                   : "Remove the installed collection from the SD card?";
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, title, message, "Cancel",
                                                        selected ? "OK" : "Remove");
  if (!dialog) return;
  startActivityForResult(std::move(dialog), [this, path, kind, selected](const ActivityResult& answer) {
    if (selected || answer.isCancelled) return;
    if (!Storage.removeDir(path.c_str())) {
      LOG_ERR("PCAT", "Unable to remove extension: %s", path.c_str());
      return;
    }
    if (kind == extensionpackages::Kind::Theme) {
      ThemePackRegistry::getInstance().scan();
    } else {
      auto& wallpapers = WallpaperPackRegistry::getInstance();
      wallpapers.scan();
      wallpapers.reconcileSelection();
    }
    computeInstallStatus();
    requestUpdate();
  });
}

HttpDownloader::DownloadError PluginCatalogActivity::downloadExtension(const Item& item) {
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size()))
    return HttpDownloader::FILE_ERROR;
  const auto& list = manifest.browseLists[currentList];
  const auto kind = list.kind == "theme-pack"       ? extensionpackages::Kind::Theme
                    : list.kind == "wallpaper-pack" ? extensionpackages::Kind::Wallpaper
                                                    : extensionpackages::Kind::Plugin;
  if (kind == extensionpackages::Kind::Plugin) return HttpDownloader::FILE_ERROR;
  const char* source = browseSourcePath.empty() ? BROWSE_TMP_PATH : browseSourcePath.c_str();
  extensionpackages::Release release;
  if (!extensionpackages::selectRelease(source, kind, item.id,
                                        std::min(renderer.getDisplayWidth(), renderer.getDisplayHeight()),
                                        std::max(renderer.getDisplayWidth(), renderer.getDisplayHeight()), release)) {
    LOG_ERR("PCAT", "No compatible or trusted extension release for %s", item.id.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  // Active themes own live resources and cannot be replaced. Wallpaper
  // packs are read on sleep and may be reinstalled without changing modes.
  const bool currentlySelected = kind == extensionpackages::Kind::Theme &&
                                 std::string(SETTINGS.sdThemeId).rfind(release.name + "/", 0) == 0;
  if (currentlySelected) {
    // Re-selecting an already installed, identical release is not an update.
    // In particular, never swap a live SD theme's glyph/bitmap backing files.
    const std::string root =
        kind == extensionpackages::Kind::Theme ? "/.crosspoint/themes/" : "/.crosspoint/wallpapers/";
    const std::string packagePath =
        root + release.name + (kind == extensionpackages::Kind::Theme ? ".crosspoint-theme" : ".crosspoint-wallpaper");
    std::string raw;
    if (Storage.readFileToString("PCAT", packagePath + "/manifest.json", MAX_MANIFEST_SIZE, raw)) {
      JsonDocument installed;
      if (deserializeJson(installed, raw) == DeserializationError::Ok && release.name == (installed["name"] | "") &&
          release.version == (installed["version"] | "")) {
        LOG_INF("PCAT", "Extension already installed and selected: %s v%s", release.name.c_str(),
                release.version.c_str());
        statusMessage = item.title + " is already installed";
        return HttpDownloader::OK;
      }
    }
    LOG_ERR("PCAT", "Switch away from the active theme before updating %s", release.name.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  // Keep the firmware's collection limits; installing a new collection must
  // not silently displace another installed one.
  if (kind == extensionpackages::Kind::Theme) {
    auto& registry = ThemePackRegistry::getInstance();
    registry.scan();
    const std::string root = "/.crosspoint/themes/" + release.name + ".crosspoint-theme";
    if (!Storage.exists(root.c_str()) && registry.choices().size() >= 16) return HttpDownloader::FILE_ERROR;
  } else {
    auto& registry = WallpaperPackRegistry::getInstance();
    registry.scan();
    const std::string root = "/.crosspoint/wallpapers/" + release.name + ".crosspoint-wallpaper";
    if (!Storage.exists(root.c_str()) && registry.choices().size() >= 8) return HttpDownloader::FILE_ERROR;
  }
  session.reset();  // the firmware must not retain TLS allocations during ZIP inflation
  constexpr char ARCHIVE[] = "/.pcat_extension.zip";
  Storage.remove(ARCHIVE);
  // These large ZIP transfers must not repaint e-ink for every 5% as book
  // transfers do. Preserve all input/cancel polling and byte progress data.
  const unsigned long transferStarted = millis();
  // GitHub Release assets issue a 302 to their signed HTTPS CDN. The generic
  // downloader drains the intermediate redirect body; on device that can
  // stall ~30 seconds. Resolve only the vetted CDN Location header first,
  // then download the final asset with the existing verified file downloader.
  std::string directAssetUrl;
  const bool directAsset = githubreleasedirect::resolveAssetUrl(release.url, directAssetUrl, &cancelDownload);
  if (cancelDownload) return HttpDownloader::ABORTED;
  downloadPercentStep = catalogprogress::kExtensionZip.percentStep;
  downloadRefreshIntervalMs = catalogprogress::kExtensionZip.maxRefreshIntervalMs;
  auto downloaded = downloadFile(directAsset ? directAssetUrl : release.url, ARCHIVE);
  if (downloaded != HttpDownloader::OK && downloaded != HttpDownloader::ABORTED && directAsset) {
    // Preserve reliability if GitHub changes the CDN URL/redirect behavior.
    LOG_INF("EXT", "Direct asset transfer failed; retrying the original release URL");
    Storage.remove(ARCHIVE);
    downloaded = downloadFile(release.url, ARCHIVE);
  }
  downloadPercentStep = catalogprogress::kDefault.percentStep;
  downloadRefreshIntervalMs = catalogprogress::kDefault.maxRefreshIntervalMs;
  const unsigned long firstChunkDelay =
      downloadFirstChunkMs ? downloadFirstChunkMs - transferStarted : 0;
  LOG_INF("EXT", "Download %s: result=%d HTTP=%d first data=%lu ms transfer=%lu ms bytes=%u",
          release.name.c_str(), static_cast<int>(downloaded), HttpDownloader::lastStatus(),
          firstChunkDelay, millis() - transferStarted, static_cast<unsigned>(downloadProgress));
  if (downloaded != HttpDownloader::OK) {
    Storage.remove(ARCHIVE);
    return downloaded;
  }
  const unsigned long installStarted = millis();
  const bool installed = extensionpackages::installVerifiedZip(ARCHIVE, release);
  LOG_INF("EXT", "ZIP verify/extract %s: result=%d duration=%lu ms",
          release.name.c_str(), static_cast<int>(installed), millis() - installStarted);
  Storage.remove(ARCHIVE);
  if (!installed) return HttpDownloader::FILE_ERROR;
  if (kind == extensionpackages::Kind::Theme) {
    ThemePackRegistry::getInstance().scan();
  } else {
    auto& wallpapers = WallpaperPackRegistry::getInstance();
    wallpapers.scan();
    wallpapers.reconcileSelection();
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError PluginCatalogActivity::downloadBundle(const Item& item) {
  session.reset();
  const std::string subdir = substituted(manifest.bundleSubdir, &item);
  // Reject traversal, the current-directory alias, and ambiguous backslash separators.
  if (subdir.empty() || subdir == "." || subdir.find("..") != std::string::npos ||
      subdir.find('\\') != std::string::npos || subdir.front() == '/') {
    return HttpDownloader::FILE_ERROR;
  }
  std::string root = downloadDir();
  while (!root.empty() && root.back() == '/') root.pop_back();
  std::string dir;
  if (PluginLocations::shouldReuseInstalledBundleDir(root, subdir)) {
    dir = PluginLocations::findPluginDir(subdir.c_str());
  }
  if (dir.empty()) {
    dir = root;
    dir += '/';
    dir += subdir;
  }
  if (Storage.exists(dir.c_str())) {
    HalFile target = Storage.open(dir.c_str());
    if (!target || !target.isDirectory()) {
      LOG_ERR("PCAT", "bundle target is not a directory: %s", dir.c_str());
      return HttpDownloader::FILE_ERROR;
    }
  } else if (!Storage.mkdir(dir.c_str())) {
    LOG_ERR("PCAT", "bundle mkdir failed: %s", dir.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  std::string base = item.base;
  if (!base.empty() && base.back() != '/') base += '/';
  const size_t total = item.files.size();
  // Every file downloads to <dest>.new first and replaces <dest> only once the
  // whole bundle has arrived, so a failed or cancelled update leaves the
  // installed plugin/theme as it was (a half-installed folder would show up
  // broken in the next discovery scan).
  std::vector<std::string> staged;  // final destinations with a complete .new
  staged.reserve(total);
  bool complete = false;
  ScopedCleanup rollback{[&] {
    if (complete) return;
    for (const auto& path : staged) Storage.remove((path + ".new").c_str());
    Storage.rmdir(dir.c_str());  // only succeeds when the folder is empty (a fresh install)
  }};
  for (size_t i = 0; i < total; i++) {
    std::string rel = item.files[i];
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    const std::string dest = dir + "/" + rel;
    if (rel.empty() || !protectedpaths::isPluginPath(dest)) {
      // A manifest listing traversal or credential-store entries is hostile
      // or broken either way; abort rather than install a bundle with holes.
      LOG_ERR("PCAT", "unsafe bundle entry rejected: %s", item.files[i].c_str());
      return HttpDownloader::FILE_ERROR;
    }
    // Create any intermediate folders for nested files ("assets/icon.bin").
    const size_t slash = dest.find_last_of('/');
    if (slash != std::string::npos) {
      const std::string parent = dest.substr(0, slash);
      if (!Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str());
    }
    const auto result = downloadFile(base + rel, dest + ".new");
    if (result != HttpDownloader::OK) return result;
    staged.push_back(dest);
  }
  // ponytail: a failed swap mid-loop leaves a mixed old/new bundle; a
  // directory-level swap would close that, at the cost of copying the tree.
  for (size_t i = 0; i < staged.size(); i++) {
    if (!Storage.replaceFile((staged[i] + ".new").c_str(), staged[i].c_str())) {
      LOG_ERR("PCAT", "bundle swap failed: %s", staged[i].c_str());
      for (size_t j = i; j < staged.size(); j++) Storage.remove((staged[j] + ".new").c_str());
      complete = true;  // already-swapped files stay; the rollback must not touch them
      return HttpDownloader::FILE_ERROR;
    }
  }
  complete = true;
  emitBookDownloaded(manifestPath, staged.empty() ? "" : staged.front(), item.title);
  // Bundles are how plugins install (plugin-store); pick up any new event
  // subscriptions without a restart. Cheap: a few small manifest reads.
  pluginevents::refreshSubscriptions();
  return HttpDownloader::OK;
}

bool PluginCatalogActivity::downloadDestination(const Item& item, std::string& dest) {
  const std::string folderDir = downloadDir();
  const char* folder = folderDir.c_str();
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("PCAT", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // Sanitize after substitution so the byte limit applies to the complete
  // filename and does not remove an extension already present in {title}.
  const std::string filename =
      StringUtils::sanitizeFilenamePreservingExtension(substituted(manifest.filenameTpl, &item));
  if (filename.empty() || filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
    LOG_ERR("PCAT", "unsafe filename rejected: %s", filename.c_str());
    return false;
  }

  dest.clear();
  dest.reserve((haveFolder ? folderDir.size() : 0) + 1 + filename.size());
  if (haveFolder) dest += folder;
  dest += '/';
  dest += filename;
  if (!protectedpaths::isPluginPath(dest)) {
    LOG_ERR("PCAT", "unsafe download dest rejected: %s", dest.c_str());
    return false;
  }
  return true;
}

HttpDownloader::DownloadError PluginCatalogActivity::finalizeDownloadedFile(const Item& item, const std::string& dest) {
  clearBookCache(dest);
  if (library::isLibraryBookFile(dest)) library::markLibraryIndexDirty();

  // Optional per-book sidecar (e.g. a service book id keyed by the file's
  // path hash) so a later sync stage can associate the file with the service.
  if (!manifest.sidecarPath.empty() && !manifest.sidecarBody.empty()) {
    const std::string md5 = md5Hex(dest);
    std::string path = substituted(manifest.sidecarPath, &item);
    substituteAll(path, "{md5}", md5);
    substituteAll(path, "{dest}", dest);
    if (!protectedpaths::isPluginPath(path)) {
      LOG_ERR("PCAT", "unsafe sidecar path rejected: %s", path.c_str());
    } else {
      std::string body = substituted(manifest.sidecarBody, &item);
      substituteAll(body, "{md5}", md5);
      substituteAll(body, "{dest}", dest);
      if (!Storage.writeFile(path.c_str(), String(body.c_str())))
        LOG_ERR("PCAT", "Sidecar write failed: %s", path.c_str());
    }
  }

  emitBookDownloaded(manifestPath, dest, item.title);
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError PluginCatalogActivity::savePreviewFile(const Item& item, const std::string& previewPath) {
  std::string dest;
  if (!downloadDestination(item, dest)) return HttpDownloader::FILE_ERROR;
  if (!Storage.exists(previewPath.c_str())) return HttpDownloader::FILE_ERROR;
  const std::string extension = lowerExtension(dest);
  const bool published = isImageExtension(extension) ? publishImageForDestination(previewPath, dest)
                                                     : Storage.replaceFile(previewPath.c_str(), dest.c_str());
  if (!published) {
    LOG_ERR("PCAT", "preview save failed: %s -> %s", previewPath.c_str(), dest.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  return finalizeDownloadedFile(item, dest);
}

void PluginCatalogActivity::previewItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;

  // Preview mode intentionally stages one direct image download. Complex
  // bundle/url_path downloads retain the established download flow.
  if (manifest.isBundle() || !manifest.dlUrlPath.empty() || manifest.downloadReq.url.empty()) {
    downloadItem(itemIndex);
    return;
  }

  Item item = items[itemIndex];
  std::string dest;
  if (!downloadDestination(item, dest)) {
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }

  const std::string destinationExtension = lowerExtension(dest);
  if (!isImageExtension(destinationExtension)) {
    downloadItem(itemIndex);
    return;
  }

  const std::string rawPreviewPath = std::string(FULL_PREVIEW_TMP_BASE) + ".download";
  Storage.remove(rawPreviewPath.c_str());

  {
    RenderLock lock;
    beginDownload(item.title);
    releaseRows();
    std::vector<fui::ListItem>().swap(rowItems);
    std::vector<Item>().swap(items);
  }
  requestUpdateAndWait();

  session.reset();
  const std::string fileUrl = substituted(manifest.downloadReq.url, &item);
  const auto fetchFile = [&] {
    return downloadFile(fileUrl, rawPreviewPath, substituted(manifest.dlUser, &item),
                        substituted(manifest.dlPass, &item), substitutedHeaders(manifest.downloadReq.headers, &item));
  };
  auto result = fileUrl.empty() ? HttpDownloader::FILE_ERROR : fetchFile();
  if (result == HttpDownloader::UNAUTHORIZED && manifest.hasPasswordGrant() && refreshCredentialToken()) {
    result = fetchFile();
  }

  session.reset();
  if (!parseBrowseResponse()) {
    Storage.remove(rawPreviewPath.c_str());
    return;
  }
  if (result != HttpDownloader::OK) {
    Storage.remove(rawPreviewPath.c_str());
    finishDownload(result);
    return;
  }

  const ImageFileKind previewKind = detectImageFileKind(rawPreviewPath);
  const char* detectedExtension = imageKindExtension(previewKind);
  if (previewKind == ImageFileKind::Unknown || detectedExtension[0] == '\0') {
    Storage.remove(rawPreviewPath.c_str());
    finishDownload(HttpDownloader::FILE_ERROR);
    return;
  }

  std::string previewPath;
  if (destinationExtension == ".bmp" && previewKind != ImageFileKind::Bmp) {
    // Convert while the download/TLS session is released and before the image
    // viewer allocates grayscale buffers. Save can then publish the exact BMP
    // that was previewed instead of performing a second heavy conversion.
    previewPath = std::string(FULL_PREVIEW_TMP_BASE) + ".bmp";
    Storage.remove(previewPath.c_str());
    if (!publishImageForDestination(rawPreviewPath, previewPath)) {
      Storage.remove(rawPreviewPath.c_str());
      finishDownload(HttpDownloader::FILE_ERROR);
      return;
    }
  } else {
    previewPath = std::string(FULL_PREVIEW_TMP_BASE) + detectedExtension;
    Storage.remove(previewPath.c_str());
    if (!Storage.replaceFile(rawPreviewPath.c_str(), previewPath.c_str())) {
      Storage.remove(rawPreviewPath.c_str());
      finishDownload(HttpDownloader::FILE_ERROR);
      return;
    }
  }

  session = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!session) LOG_ERR("PCAT", "OOM: browse client; using per-request connections");

  state = State::BROWSING;
  auto viewer = std::make_unique<BmpViewerActivity>(renderer, mappedInput, previewPath, true, "Close", "Save");
  startActivityForResult(std::move(viewer), [this, item, previewPath](const ActivityResult& previewResult) {
    if (previewResult.isCancelled) {
      Storage.remove(previewPath.c_str());
      state = State::BROWSING;
      return;
    }

    const auto saveResult = savePreviewFile(item, previewPath);
    if (saveResult == HttpDownloader::OK) {
      // Preview saves are already an explicit completed action. Return to the
      // catalog immediately instead of requiring a second acknowledgement.
      state = State::BROWSING;
      requestUpdate();
    } else {
      Storage.remove(previewPath.c_str());
      fail(StrId::STR_DOWNLOAD_FAILED);
    }
  });
}

HttpDownloader::DownloadError PluginCatalogActivity::downloadBook(const Item& item) {
  // Resolve the file URL: either the template itself, or one API hop away.
  std::string fileUrl;
  if (!manifest.dlUrlPath.empty()) {
    String response;
    int status = apiRequest(substitutedRequest(manifest.downloadReq, &item), response);
    // An expired password-grant token: mint a fresh one and retry once.
    if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) {
      status = apiRequest(substitutedRequest(manifest.downloadReq, &item), response);
    }
    if (status < 200 || status >= 300) {
      return HttpDownloader::HTTP_ERROR;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      return HttpDownloader::HTTP_ERROR;
    }
    fileUrl = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.dlUrlPath));
  } else {
    fileUrl = substituted(manifest.downloadReq.url, &item);
  }
  if (fileUrl.empty()) {
    return HttpDownloader::FILE_ERROR;
  }

  std::string dest;
  if (!downloadDestination(item, dest)) return HttpDownloader::FILE_ERROR;
  const std::string destinationExtension = lowerExtension(dest);
  const bool imageDestination = isImageExtension(destinationExtension);
  const std::string downloadPath = imageDestination ? dest + ".download" : dest;
  if (imageDestination) Storage.remove(downloadPath.c_str());

  // url_path already authenticated the JSON hop; the resolved file URL must not
  // inherit those headers (S3 pre-signed GETs reject a second Authorization).
  const auto fetchFile = [&] {
    const std::vector<HttpDownloader::Header> fileHeaders =
        manifest.dlUrlPath.empty() ? substitutedHeaders(manifest.downloadReq.headers, &item)
                                   : std::vector<HttpDownloader::Header>{};
    return downloadFile(fileUrl, downloadPath, substituted(manifest.dlUser, &item), substituted(manifest.dlPass, &item),
                        fileHeaders);
  };
  session.reset();  // free browse TLS before the large file GET
  auto result = fetchFile();
  // The direct GET carries the catalog's token templates (never a pre-signed
  // url_path target): an expired password-grant token gets one mint-and-retry.
  if (result == HttpDownloader::UNAUTHORIZED && manifest.dlUrlPath.empty() && manifest.hasPasswordGrant() &&
      refreshCredentialToken()) {
    result = fetchFile();
  }
  if (result != HttpDownloader::OK) {
    if (imageDestination) Storage.remove(downloadPath.c_str());
    return result;
  }
  if (imageDestination && !publishImageForDestination(downloadPath, dest)) {
    Storage.remove(downloadPath.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  return finalizeDownloadedFile(item, dest);
}

void PluginCatalogActivity::onRowAction(const fui::ActionEvent& event) {
  if (state == State::PLUGIN_PICKER && event.longPress) {
    activeNav().selected = event.value;
    confirmUninstallPlugin(event.value);
    return;
  }
  if (state == State::BROWSING && event.longPress && currentList >= 0 &&
      currentList < static_cast<int>(manifest.browseLists.size()) &&
      manifest.browseLists[currentList].kind != "plugin") {
    confirmRemoveExtension(event.value);
    return;
  }
  if (pluginTabsActive()) {
    activeNav().selected = event.value + contentSelectionOffset();
    if (!event.longPress) activateIndex(event.value);
    return;
  }
  UiListActivity::onRowAction(event);
}

bool PluginCatalogActivity::stepBrowseViewport(const int direction, const bool moveSelection) {
  if (direction == 0 || state != State::BROWSING || !nativeFooterActive() || items.empty()) return false;

  const int count = static_cast<int>(items.size());
  const int visible = std::clamp(browseViewportItems.load(), 1, count);
  const int maxTop = std::max(0, count - visible);
  const int top = std::clamp(browseViewportTop.load(), 0, maxTop);
  int nextTop = top;

  if (direction > 0) {
    if (top < maxTop) {
      nextTop = std::min(maxTop, top + visible);
    } else if (nextRowVisible()) {
      onPageEvent(fui::ActionEvent{ACTION_PAGE, 1, fui::StateNormal}, this);
      return true;
    } else {
      return false;
    }
  } else {
    if (top > 0) {
      nextTop = std::max(0, top - visible);
    } else if (prevRowVisible()) {
      onPageEvent(fui::ActionEvent{ACTION_PAGE, -1, fui::StateNormal}, this);
      return true;
    } else {
      return false;
    }
  }

  browseRequestedTop.store(nextTop);
  if (moveSelection) {
    const int itemIndex = direction > 0 ? nextTop : std::min(count - 1, nextTop + visible - 1);
    activeNav().requestSelection(contentSelectionOffset() + itemIndex);
    // The requested viewport is authoritative for a hardware batch jump. Keep
    // the new selection visible without letting selection-follow choose a
    // different grid/list top on the render task.
    activeNav().followOnBuild.store(false);
  }
  requestUpdate();
  return true;
}

void PluginCatalogActivity::navigateButtons() {
  if (state == State::PLUGIN_PICKER) {
    // Header rows are display-only. Skip them for single presses and repeats,
    // including when a held button moves through an entire visible page.
    buttonNavigator.onNextPress([this] { moveSelectionTo(nextPickerRow(activeNav().selected, 1)); });
    buttonNavigator.onPreviousPress([this] { moveSelectionTo(nextPickerRow(activeNav().selected, -1)); });
    buttonNavigator.onNextContinuous(
        [this] { moveSelectionTo(nextPickerRow(activeNav().selected, 1, std::max(1, activeNav().inputPageRows()))); });
    buttonNavigator.onPreviousContinuous(
        [this] { moveSelectionTo(nextPickerRow(activeNav().selected, -1, std::max(1, activeNav().inputPageRows()))); });
    return;
  }

  if (!pluginTabsActive() && !nativeFooterActive()) {
    CatalogActivity::navigateButtons();
    return;
  }

  auto& current = activeNav();
  const int selectedItem = current.selected.load() - contentSelectionOffset();
  const bool browseItemFocused = state == State::BROWSING && nativeFooterActive() && selectedItem >= 0 &&
                                 selectedItem < static_cast<int>(items.size());
  const int ringSize = navigationCount();

  if (browseItemFocused) {
    // X3/X4 expose two physical navigation groups. The side/page-turn pair is
    // internally Up/Down; keep it as one-item navigation. The front/bottom
    // directional pair is internally Left/Right; use it as the hardware
    // equivalent of a vertical swipe, jumping one visible batch at a time.
    const auto nextItem = [this, ringSize] {
      if (activeNav().selected == footerNextSelection()) {
        onPageEvent(fui::ActionEvent{ACTION_PAGE, 1, fui::StateNormal}, this);
        return;
      }
      activeNav().requestSelection(ButtonNavigator::nextIndex(activeNav().selected, ringSize));
      requestUpdate();
    };
    const auto previousItem = [this, ringSize] {
      activeNav().requestSelection(ButtonNavigator::previousIndex(activeNav().selected, ringSize));
      requestUpdate();
    };

    ButtonNavigator::onPress({MappedInputManager::Button::Down}, nextItem);
    ButtonNavigator::onPress({MappedInputManager::Button::Up}, previousItem);
    buttonNavigator.onContinuous({MappedInputManager::Button::Down}, nextItem);
    buttonNavigator.onContinuous({MappedInputManager::Button::Up}, previousItem);
    ButtonNavigator::onPress({MappedInputManager::Button::Right}, [this] { stepBrowseViewport(1, true); });
    ButtonNavigator::onPress({MappedInputManager::Button::Left}, [this] { stepBrowseViewport(-1, true); });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = pluginTabsActive() && current.selected == 0;
  }
  buttonNavigator.onNextPress([this, ringSize] {
    if (activeNav().selected == footerNextSelection()) {
      onPageEvent(fui::ActionEvent{ACTION_PAGE, 1, fui::StateNormal}, this);
      return;
    }
    activeNav().requestSelection(ButtonNavigator::nextIndex(activeNav().selected, ringSize));
    requestUpdate();
  });
  buttonNavigator.onPreviousPress([this, ringSize] {
    activeNav().requestSelection(ButtonNavigator::previousIndex(activeNav().selected, ringSize));
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepPluginTab(1);
    }
  });
  buttonNavigator.onPreviousContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
      stepPluginTab(-1);
    }
  });
}

bool PluginCatalogActivity::handleButtons() {
  // Match the library's one-second hold behavior. wasLongPressed suppresses the
  // eventual release, so it cannot fall through and open the selected plugin.
  if (state == State::PLUGIN_PICKER &&
      mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, PLUGIN_UNINSTALL_HOLD_MS)) {
    confirmUninstallPlugin(activeNav().selected);
    return true;
  }
  if (!pluginTabsActive() && !nativeFooterActive()) return UiListActivity::handleButtons();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (settingsTabActive() && cacheSettingsExpanded) {
      cacheSettingsExpanded = false;
      activeNav().selected = contentSelectionOffset();
      releaseRows();
      requestUpdate();
    } else if (pluginTabsActive() && !tabsFocused()) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      onBackButton();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (tabsFocused()) {
      stepPluginTab(1);
      return true;
    }
    const int selected = activeNav().selected;
    if (selected == footerPreviousSelection()) {
      onPageEvent(fui::ActionEvent{ACTION_PAGE, -1, fui::StateNormal}, this);
      return true;
    }
    if (selected == footerRefreshSelection()) {
      onRefreshEvent(fui::ActionEvent{ACTION_REFRESH, 0, fui::StateNormal}, this);
      return true;
    }
    if (selected == footerNextSelection()) {
      onPageEvent(fui::ActionEvent{ACTION_PAGE, 1, fui::StateNormal}, this);
      return true;
    }
    activateIndex(selected - contentSelectionOffset());
    return true;
  }
  return false;
}

// Sign-in and completion states precede the shared catalog input handling.
bool PluginCatalogActivity::handleCustomInput() {
  if (ignoreHubInstallerBackRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ignoreHubInstallerBackRelease = false;
      return true;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) ignoreHubInstallerBackRelease = false;
  }
  if (pluginHubPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (state == State::AUTH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::NO_TOKEN;
      requestUpdate();
      return true;
    }
    if (static_cast<long>(millis() - authNextPollMs) >= 0) pollAuth();
    return true;
  }

  if (state == State::NO_TOKEN) {
    // Back first: a header back tap is also a screen tap, which means sign in here.
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitCatalog();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (!wifiConnected())
        launchWifiSelection();
      else
        retryBrowse();
    }
    return true;
  }

  if (state == State::DONE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      // The just-finished download may have installed/updated a plugin; refresh
      // the install badges so the row no longer reads "Update".
      computeInstallStatus();
      releaseRows();
      state = State::BROWSING;
      requestUpdate();
    }
    return true;
  }

  if (state == State::BROWSING && nativeFooterActive() && !items.empty()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      // Browse swipes and bottom hardware batch buttons share the same viewport
      // stepping. Touch intentionally leaves the focused item unchanged.
      const int direction = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;

      // A swipe can begin over a tappable cover/row. routeListTouch() normally
      // delivers the release edge that clears FreeInkUI active press state, but
      // this custom path returns before the base loop reaches it.
      routeTouch(mappedInput, true);
      stepBrowseViewport(direction, false);
      return true;
    }
  }

  return CatalogActivity::handleCustomInput();
}

void PluginCatalogActivity::retryBrowse() {
  if (!manifestLoaded) {
    enterCatalog();
    return;
  }
  if (state == State::NO_TOKEN && manifest.hasDeviceCode()) {
    startDeviceAuth();
  } else if ((!manifest.browseListsUrl.empty() && manifest.browseLists.empty()) || wantsListPicker()) {
    if (!manifest.browseListsUrl.empty() && manifest.browseLists.empty()) browseListsRemoteAttempted = false;
    startBrowse();
  } else {
    beginLoading();
    fetchPage(page);
  }
}

// Back on the picker leaves the activity; the base routes it here via
// handleButtons. The catalog states route their Back through exitCatalog()
// instead, landing back on the picker.
void PluginCatalogActivity::onBackButton() {
  if (settingsTabActive() && cacheSettingsExpanded) {
    cacheSettingsExpanded = false;
    activeNav().selected = contentSelectionOffset();
    releaseRows();
    requestUpdate();
    return;
  }
  if (state == State::ERROR || state == State::CHECK_WIFI || state == State::LOADING) {
    exitCatalog();
    return;
  }
  if (state == State::PLUGIN_PICKER) {
    if (rootMode) {
      onGoHome();  // home launch replaced the home screen (root)
    } else {
      finish();  // Settings launch pushed us
    }
    return;
  }
  if (searchActive) {
    // Leave the results and return to the pre-search view (picker or page 1).
    searchActive = false;
    searchQuery.clear();
    if (manifest.browseLists.size() == 1) currentList = 0;  // its notice was already accepted
    startBrowse();
    return;
  }
  if (manifest.isXmlList() && !browseHistory.empty()) {
    browseCurrentUrl = browseHistory.back();
    browseHistory.pop_back();
    beginLoading();
    fetchPage(page);
  } else if (state == State::BROWSING && currentList >= 0) {
    // Browsing a picked list: Back returns to the list picker, not out. If
    // the list was empty, remember that re-entering it should check the
    // network instead of accepting the cached empty page again.
    if (items.empty()) browseRefreshNeeded = true;
    currentList = -1;
    startBrowse();
  } else {
    exitCatalog();
  }
}

void PluginCatalogActivity::activateIndex(const int index) {
  if (cacheTabActive()) {
    activateCacheRow(index);
    return;
  }
  if (settingsTabActive()) {
    if (index < 0 || index >= settingsRowCount()) return;
    if (cacheSettingsExpanded) {
      const int cacheSettings = cacheSettingCount();
      if (index < cacheSettings)
        activateSetting(settingsManifestIndex(index, true));
      else
        activateCacheRow(index - cacheSettings);
      return;
    }

    int regularSettings = 0;
    for (const auto& setting : manifest.settings)
      if (settingVisible(setting) && !isCacheSetting(setting)) ++regularSettings;
    if (cacheUsable() && index == regularSettings) {
      cacheSettingsExpanded = true;
      activeNav().selected = contentSelectionOffset();
      releaseRows();
      requestUpdate();
      return;
    }
    activateSetting(settingsManifestIndex(index, false));
    return;
  }
  if (state == State::PLUGIN_PICKER) {
    if (index < 0 || index >= rowCount()) return;
    if (showOpds && index == 0) {
      app.clearTapFlash();            // the row leaves this screen
      activityManager.goToBrowser();  // replaces this screen with the OPDS browser
      return;
    }
    const int pluginStart = showOpds ? 1 : 0;
    if (showPluginHubInstallRow && index == pluginStart) {
      static constexpr StrId OPTIONS[] = {StrId::STR_INSTALL, StrId::STR_HIDE, StrId::STR_CANCEL};
      pluginHubPopup.show(StrId::STR_PLUGIN_HUB, OPTIONS, 3, 0, [this](const int selected) {
        if (selected == 0) {
          auto installer = makeUniqueNoThrow<PluginHubInstallActivity>(renderer, mappedInput, false);
          if (!installer) {
            LOG_ERR("PCAT", "OOM: Plugin Hub installer");
            return;
          }
          startActivityForResult(std::move(installer), [this](const ActivityResult& result) {
            ignoreHubInstallerBackRelease =
                result.isCancelled && mappedInput.isPressed(MappedInputManager::Button::Back);
            enterPluginPicker();
          });
        } else if (selected == 1) {
          SETTINGS.pluginHubPromptHidden = 1;
          SETTINGS.saveToFile();
          enterPluginPicker();
        }
      });
      requestUpdate();
      return;
    }
    if (index >= extensionPickerStart()) {
      showInstalledExtensionDetails(index);
      return;
    }
    if (pickerHeaderRow(index)) return;
    const int pluginIndex = pickerPluginForRow(index);
    if (pluginIndex < 0) return;
    const PluginRef& plugin = installedPlugins[pluginIndex];
    const auto action = PluginLocations::pickerAction(plugin.deviceKind, !plugin.readmePath.empty());
    if (action == PluginLocations::PickerAction::Readme) {
      // A plain paged text view, not the book reader: viewing instructions must
      // not touch the last-read book, recents, progress, or reader events.
      // Capped: a README is setup notes, not a book, and lives in RAM here.
      static constexpr size_t MAX_README_BYTES = 16 * 1024;
      std::string text;
      if (!Storage.readFileToString("PCAT", plugin.readmePath, MAX_README_BYTES, text)) {
        LOG_ERR("PCAT", "README unreadable, empty, or over %u bytes: %s", static_cast<unsigned>(MAX_README_BYTES),
                plugin.readmePath.c_str());
        return;
      }
      auto viewer =
          makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, plugin.title, std::move(text));
      if (!viewer) {
        LOG_ERR("PCAT", "OOM: README viewer");
        return;
      }
      app.clearTapFlash();
      startActivityForResult(std::move(viewer), [](const ActivityResult&) {});
      return;
    }
    if (action != PluginLocations::PickerAction::Catalog) return;
    app.clearTapFlash();  // the row leaves this screen
    pickerReturnName = plugin.name;
    manifestPath = plugin.manifestPath;
    catalogTitle = plugin.title;
    enterCatalog();
    return;
  }
  if (state == State::LIST_PICKER) {
    if (index < 0 || index >= static_cast<int>(manifest.browseLists.size())) return;
    app.clearTapFlash();
    const auto& list = manifest.browseLists[index];
    if (list.hasNotice()) {
      const std::string heading = substituted(list.noticeTitle.empty() ? list.title : list.noticeTitle, nullptr);
      const std::string message = substituted(list.noticeMessage, nullptr);
      const std::string cancel = substituted(list.noticeCancel, nullptr);
      const std::string confirm = substituted(list.noticeConfirm, nullptr);
      auto confirmation =
          makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, message, cancel, confirm);
      if (!confirmation) {
        LOG_ERR("PCAT", "OOM: browse-list notice");
        return;
      }
      startActivityForResult(std::move(confirmation), [this, index](const ActivityResult& result) {
        if (result.isCancelled) return;
        currentList = index;
        startBrowse();
      });
      return;
    }
    currentList = index;
    startBrowse();
    return;
  }
  if (nativeFooterActive()) {
    app.clearTapFlash();
    activateItem(index);
    return;
  }
  // The pager rows bracket the items: "Previous page" ahead of them past
  // page 1, "Next page" after them while more pages exist.
  const int itemIndex = index - (prevRowVisible() ? 1 : 0);
  if (itemIndex == -1 || (nextRowVisible() && itemIndex == static_cast<int>(items.size()))) {
    app.clearTapFlash();
    beginLoading();
    fetchPage(itemIndex == -1 ? page - 1 : page + 1);
    return;
  }
  app.clearTapFlash();  // the row leaves this screen (folder, download view)
  activateItem(itemIndex);
}

void PluginCatalogActivity::activateItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];
  if (manifest.isXmlList() && item.isDir) {
    browseHistory.push_back(browseCurrentUrl);
    browseCurrentUrl = item.url;
    beginLoading();
    fetchPage(page);
    return;
  }
  if (!manifest.isXmlList() && manifest.hasItemDetails()) {
    showItemDetails(itemIndex);
    return;
  }
  if (!wifiConnected()) {
    pendingItemActivation = itemIndex;
    launchWifiSelection();
    return;
  }
  if (manifest.previewOnSelect) {
    previewItem(itemIndex);
    return;
  }
  downloadItem(itemIndex);
}

bool PluginCatalogActivity::loadItemDetails(const int itemIndex, ItemDetails& details) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return false;
  const Item& target = items[itemIndex];
  const bool extension = currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size()) &&
                         manifest.browseLists[currentList].kind != "plugin";
  const std::string& itemsPath = extension ? manifest.browseLists[currentList].itemsPath : manifest.itemsPath;
  bool found = false;
  auto capture = [&](JsonListParser::Row& row) {
    if (found) return;
    const bool idMatch = !target.id.empty() && row.field[JsonListParser::F_AUTHOR] == target.id;
    const bool fallbackMatch = target.id.empty() && row.field[JsonListParser::F_TITLE] == target.title &&
                               (target.version.empty() || row.field[JsonListParser::F_VERSION] == target.version);
    if (!idMatch && !fallbackMatch) return;
    details.description = std::move(row.field[JsonListParser::F_ID]);
    details.author = std::move(row.field[JsonListParser::F_URL]);
    details.repository = std::move(row.field[JsonListParser::F_BASE]);
    found = true;
  };
  const std::string* const fields[] = {&manifest.titlePath,        &manifest.idPath,      &manifest.descriptionPath,
                                       &manifest.detailAuthorPath, &manifest.versionPath, &manifest.repositoryPath};
  const std::string unused;
  auto parser = makeUniqueNoThrow<JsonListParser>(
      itemsPath, fields, unused,
      [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(capture)*>(context))(row); }, &capture);
  const bool metadataFound = streamBrowseFile(parser.get()) && found;

  if (manifest.detailsUrlPath.empty()) return metadataFound;
  bool richFound = false;
  auto captureRich = [&](JsonListParser::Row& row) {
    if (richFound) return;
    const bool idMatch = !target.id.empty() && row.field[JsonListParser::F_AUTHOR] == target.id;
    const bool fallbackMatch = target.id.empty() && row.field[JsonListParser::F_TITLE] == target.title &&
                               (target.version.empty() || row.field[JsonListParser::F_VERSION] == target.version);
    if (!idMatch && !fallbackMatch) return;
    details.detailsUrl = std::move(row.field[JsonListParser::F_ID]);
    details.detailsFormat = std::move(row.field[JsonListParser::F_URL]);
    richFound = true;
  };
  const std::string* const richFields[] = {&manifest.titlePath,         &manifest.idPath,      &manifest.detailsUrlPath,
                                           &manifest.detailsFormatPath, &manifest.versionPath, &unused};
  auto richParser = makeUniqueNoThrow<JsonListParser>(
      itemsPath, richFields, unused,
      [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(captureRich)*>(context))(row); },
      &captureRich);
  return (streamBrowseFile(richParser.get()) && richFound) || metadataFound;
}

void PluginCatalogActivity::showItemDetails(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];

  ItemDetails details;
  loadItemDetails(itemIndex, details);

  std::string body;
  bool htmlDefinition = false;
  if (!details.detailsUrl.empty()) {
    const bool supportedFormat =
        details.detailsFormat.empty() || details.detailsFormat == "text" || details.detailsFormat == "html";
    if (!supportedFormat) {
      LOG_INF("PCAT", "unsupported item details format: %s", details.detailsFormat.c_str());
    } else {
      pluginhttp::RequestSpec detailsReq;
      detailsReq.url = details.detailsUrl;
      detailsReq.method = "GET";
      const std::string identity = "details-v1|" + details.detailsFormat;
      const std::string cachePath = cacheResourcePath(detailsReq, &item, identity, "r_", ".dat");
      const bool cached = tryLoadCachedTextResource(cachePath, manifest.cacheDetailsPolicy, false, body);
      if (!cached && !wifiConnected()) {
        if (!manifest.cacheStaleFallback ||
            !tryLoadCachedTextResource(cachePath, manifest.cacheDetailsPolicy, true, body)) {
          pendingItemActivation = itemIndex;
          launchWifiSelection();
          return;
        }
      }
      if (!cached && body.empty()) {
        const auto req = substitutedRequest(detailsReq, &item);
        String response;
        const int status = pluginhttp::request(session.get(), req.url, req.method, req.body, req.headers, response,
                                               MAX_ITEM_DETAILS_RESPONSE);
        if (status >= 200 && status < 300 && response.length() > 0) {
          body.assign(response.c_str(), response.length());
          if (manifest.cacheDetailsPolicy != Manifest::CacheResourcePolicy::Disabled &&
              storeCachedTextResource(cachePath, body))
            enforceCacheLimit();
        } else {
          LOG_INF("PCAT", "item details fetch failed: HTTP %d", status);
          if (manifest.cacheStaleFallback)
            tryLoadCachedTextResource(cachePath, manifest.cacheDetailsPolicy, true, body);
        }
      }
      htmlDefinition = details.detailsFormat == "html" && !body.empty();
    }
  }

  if (body.empty()) body = details.description;

  DetailsSpec spec;
  spec.header = catalogTitle;
  spec.title = item.title;
  if (!details.author.empty()) spec.author = std::string(tr(STR_AUTHOR)) + ": " + details.author;
  if (!details.repository.empty()) spec.source = std::string(tr(STR_REPOSITORY)) + ": " + details.repository;
  const auto addFact = [&](const std::string& fact) {
    if (fact.empty()) return;
    if (!spec.metadata.empty()) spec.metadata += " · ";
    spec.metadata += fact;
  };
  if (!item.version.empty()) addFact(std::string(tr(STR_VERSION)) + " " + item.version);
  if (item.status == tr(STR_INSTALLED) || item.status == tr(STR_UPDATE_AVAILABLE)) addFact(item.status);
  spec.description = std::move(body);
  spec.descriptionHtml = htmlDefinition;

  const bool updateAvailable = item.status == tr(STR_UPDATE_AVAILABLE);
  spec.actionLabel = updateAvailable ? tr(STR_UPDATE) : (manifest.tracksInstalls() ? tr(STR_INSTALL) : tr(STR_FETCH));
  auto detailsActivity = makeUniqueNoThrow<DetailsActivity>(renderer, mappedInput, std::move(spec));
  if (!detailsActivity) {
    LOG_ERR("PCAT", "OOM: item details");
    return;
  }
  startActivityForResult(std::move(detailsActivity), [this, itemIndex](const ActivityResult& result) {
    if (result.isCancelled || itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
    if (!wifiConnected()) {
      pendingItemDownload = itemIndex;
      launchWifiSelection();
      return;
    }
    downloadItem(itemIndex);
  });
}

void PluginCatalogActivity::drawFooter() {
  // The QR is a raw-renderer overlay (FreeInkUI has no QR component); the base
  // render() calls drawFooter() after the app has painted.
  if (state == State::AUTH && authQrRect.width > 0) {
    QrUtils::drawQrCode(renderer, Rect{authQrRect.x, authQrRect.y, authQrRect.width, authQrRect.height}, authVerifyUrl);
  }
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER: {
      const int count = rowCount();
      const int selectedRow = activeNav().selected - contentSelectionOffset();
      const int prevOff = nativeFooterActive() ? 0 : (prevRowVisible() ? 1 : 0);
      const int itemSel = selectedRow - prevOff;
      const char* confirmLabel;
      if (state != State::BROWSING) {
        confirmLabel = count > 0 ? tr(STR_OPEN) : "";
      } else if (pluginTabsActive() && tabsFocused()) {
        confirmLabel = manifest.tabs.size() > 1 ? tr(STR_SELECT) : "";
      } else if (cacheTabActive()) {
        confirmLabel = selectedRow >= 4 ? tr(STR_SELECT) : "";
      } else if (settingsTabActive()) {
        confirmLabel = count > 0 ? tr(STR_SELECT) : "";
      } else {
        const bool onItem = itemSel >= 0 && itemSel < static_cast<int>(items.size());
        const bool onDir = manifest.isXmlList() && onItem && items[itemSel].isDir;
        const bool onDetails = !manifest.isXmlList() && onItem && manifest.hasItemDetails();
        const int selected = activeNav().selected.load();
        const bool onFooterAction = selected == footerPreviousSelection() || selected == footerRefreshSelection() ||
                                    selected == footerNextSelection();
        confirmLabel =
            onFooterAction
                ? tr(STR_FETCH)
                : (count == 0 ? ""
                              : (onDir || manifest.previewOnSelect ? tr(STR_OPEN)
                                                                   : (onDetails ? tr(STR_DETAILS) : tr(STR_FETCH))));
      }
      // On the top row of a searchable catalog the previous-nav slot becomes
      // Search (front Left), mirroring the OPDS browser's side-button search.
      const bool searchable = state == State::BROWSING && hasSearch() && selectedRow == 0;
      const int navCount = navigationCount();
      const char* up = searchable ? tr(STR_SEARCH) : (navCount > 1 ? tr(STR_DIR_UP) : "");
      const char* down = navCount > 1 ? tr(STR_DIR_DOWN) : "";
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
      break;
    }
    case State::ERROR:
    case State::NO_TOKEN: {
      const bool canSignIn = state == State::NO_TOKEN && manifest.hasDeviceCode();
      labels = mappedInput.mapLabels(tr(STR_BACK), canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY), "", "");
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    default:  // CHECK_WIFI / LOADING / AUTH / DONE (and child-activity handoffs)
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PluginCatalogActivity::render(RenderLock&& lock) {
  if (pluginHubPopup.processRender(renderer, mappedInput)) return;
  CatalogActivity::render(std::move(lock));
}

void PluginCatalogActivity::buildScreen(UiScreen& screen) {
  const bool listState = state == State::BROWSING || state == State::LIST_PICKER;
  const std::string title = listState ? browsingHeaderLabel() : catalogTitle;
  screenHeader(screen, title.c_str());
  if (buildStatusScreen(screen, /*boldError=*/true, /*showDownloadTotal=*/true)) return;

  switch (state) {
    case State::BROWSING:
      if (pluginTabsActive()) buildPluginTabBar(screen);
      if (cacheTabActive())
        buildCacheScreen(screen);
      else if (settingsTabActive())
        buildPluginSettingsScreen(screen);
      else
        buildBrowsingScreen(screen);
      return;
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER:
      buildBrowsingScreen(screen);
      return;
    case State::AUTH:
      buildAuthScreen(screen);
      return;
    case State::DONE:
      catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}, {statusMessage.c_str()}});
      return;
    case State::NO_TOKEN:
      catalogCenteredBlock(screen,
                           {{manifest.hasDeviceCode() ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN)}});
      return;
    default:
      return;
  }
}

// Device-code sign-in. The QR payload remains the server-provided verification
// URL; optional manifest hints only control the human-readable presentation.
void PluginCatalogActivity::buildAuthScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  fui::TextStyle small = theme.smallText;
  small.align = fui::TextAlign::Center;
  fui::TextStyle manual = small;
  manual.maxLines = 2;
  fui::TextStyle code = theme.titleText;
  code.align = fui::TextAlign::Center;
  code.bold = true;
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, NOTOSANS_18_FONT_ID);
  code.font = fui::GfxRendererTarget::FONT_LABEL;

  constexpr int16_t qrSize = 198;
  const int16_t gap = theme.spaceSm;
  const int16_t pad = theme.spaceMd;
  const fui::Rect body = screen.body();
  const int16_t textWidth = std::max<int16_t>(1, static_cast<int16_t>(body.width - pad * 2));
  const char* displayUrl = manifest.authDisplayUrl.empty() ? authVerifyUrl.c_str() : manifest.authDisplayUrl.c_str();
  const char* qrHint = manifest.authQrHint.empty() ? nullptr : manifest.authQrHint.c_str();
  const char* manualHint = manifest.authManualHint.empty() ? nullptr : manifest.authManualHint.c_str();

  const auto heightOf = [&](const char* text, const fui::TextStyle& style) -> int16_t {
    if (!text || !text[0]) return 0;
    return std::max<int16_t>(screen.target().lineHeight(style.font),
                             fui::measureWrappedText(screen.target(), text, style, textWidth).height);
  };
  const int16_t qrHintH = heightOf(qrHint, centered);
  const int16_t codeH = heightOf(authUserCode.c_str(), code);
  const int16_t urlH = heightOf(displayUrl, centered);
  const int16_t manualH = heightOf(manualHint, manual);
  const int16_t waitingH = heightOf(tr(STR_PLUGIN_AUTH_WAITING), small);
  const int visibleTextRows = (qrHintH > 0 ? 1 : 0) + 1 + (urlH > 0 ? 1 : 0) + (manualH > 0 ? 1 : 0) + 1;
  const int16_t totalH =
      static_cast<int16_t>(qrHintH + codeH + qrSize + urlH + manualH + waitingH + gap * visibleTextRows);
  if (body.height > totalH) screen.spacer(static_cast<int16_t>((body.height - totalH) / 2));

  const auto drawCentered = [&](const char* text, const fui::TextStyle& style, const int16_t height) {
    if (!text || !text[0] || height <= 0) return;
    screen.target().text(screen.takeTop(height, gap).inset(fui::Insets{0, pad, 0, pad}), text, style);
  };

  drawCentered(qrHint, centered, qrHintH);
  drawCentered(authUserCode.c_str(), code, codeH);
  const fui::Rect band = screen.takeTop(qrSize, gap);
  authQrRect = fui::Rect{static_cast<int16_t>(band.x + (band.width - qrSize) / 2), band.y, qrSize, qrSize};
  drawCentered(displayUrl, centered, urlH);
  drawCentered(manualHint, manual, manualH);
  if (waitingH > 0)
    screen.target().text(screen.takeTop(waitingH).inset(fui::Insets{0, pad, 0, pad}), tr(STR_PLUGIN_AUTH_WAITING),
                         small);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, uiScaleSpec().smallFontId);
}

std::string PluginCatalogActivity::browsingHeaderLabel() const {
  std::string label = catalogTitle;
  if (searchActive) {
    label = std::string(tr(STR_SEARCH)) + ": " + searchQuery;
  } else if (state == State::BROWSING && currentList >= 0 &&
             currentList < static_cast<int>(manifest.browseLists.size())) {
    label = manifest.browseLists[currentList].title;
  }
  if (state == State::BROWSING && page > 1) {
    label += " - ";
    label += tr(STR_PLUGIN_CATALOG_PAGE);
    label += " ";
    label += std::to_string(page);
  }
  return label;
}

void PluginCatalogActivity::buildPluginTabBar(UiScreen& screen) {
  if (!pluginTabsActive()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  fui::TabItem tabs[MAX_PLUGIN_TABS];
  const int count = std::min<int>(MAX_PLUGIN_TABS, manifest.tabs.size());
  for (int i = 0; i < count; ++i) {
    tabs[i].label = manifest.tabs[i].title.c_str();
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = i == activePluginTab;
  }
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_PLUGIN_TAB;
  props.inputMask = fui::InputTouch;
  const bool focused = tabsFocused();
  if (metrics.tabPillFullSlot) {
    props.text = screen.theme().bodyText;
    props.tabInset = fui::Insets{4, 4, 7, 4};
    props.contentInset = fui::Insets{2, 0, 2, 0};
  } else {
    props.text = screen.theme().smallText;
    props.gap = static_cast<int16_t>(metrics.tabSpacing);
    props.tabInset = focused ? fui::Insets{2, 4, 4, 4} : fui::Insets{2, 4, 0, 4};
    props.contentInset = fui::Insets{2, 0, 2, 0};
  }
  const int16_t lineHeight = screen.target().lineHeight(props.text.font);
  const int16_t preferred = mappedInput.hasTouch() ? TOUCH_TAB_BAR_HEIGHT : static_cast<int16_t>(metrics.tabBarHeight);
  const int16_t tabBand = std::max<int16_t>(preferred, static_cast<int16_t>(lineHeight + 10));

  fui::StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  if (focused) {
    styles.selected.background = fui::Paint::solid(fui::Color::Black);
    styles.selected.foreground = fui::Paint::solid(fui::Color::White);
    styles.selected.radius = screen.theme().listRowRadius;
  } else if (metrics.tabPillFullSlot) {
    styles.selected.background = fui::Paint::dither(fui::Color::DarkGray);
    styles.selected.foreground = fui::Paint::solid(fui::Color::White);
    styles.selected.radius = screen.theme().listRowRadius;
  } else {
    styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
    styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    props.selectedUnderline = 2;
  }
  styles.focused = styles.selected;
  styles.active = styles.selected;
  props.tabStyles = styles;

  const fui::Rect contentBand = screen.takeTop(tabBand);
  const fui::Rect frame = screen.frame().screen();
  const fui::Rect fullBand{frame.x, contentBand.y, frame.width, contentBand.height};
  if (focused && !metrics.tabPillFullSlot) screen.target().fill(fullBand, fui::Paint::dither(fui::Color::LightGray));
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  const fui::Rect slots{static_cast<int16_t>(fullBand.x + side), fullBand.y,
                        static_cast<int16_t>(fullBand.width - side * 2), static_cast<int16_t>(fullBand.height - 1)};
  props.divider = false;
  fui::tabBar(screen.frame(), slots, props);
  screen.target().fill(fui::Rect{fullBand.x, static_cast<int16_t>(fullBand.bottom() - 1), fullBand.width, 1},
                       fui::Paint::solid(fui::Color::Black));
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

void PluginCatalogActivity::buildCatalogFooter(UiScreen& screen) {
  if (!nativeFooterActive()) return;
  fui::FooterAction actions[3];
  uint8_t actionCount = 0;

  if (!manifest.paginationHidden) {
    auto& previous = actions[actionCount++];
    previous.label = tr(STR_PREV_SHORT);
    previous.action = ACTION_PAGE;
    previous.value = -1;
    previous.enabled = prevRowVisible();
    if (activeNav().selected == footerPreviousSelection()) previous.state = fui::StateFocused;
  }

  if (manifest.refreshEnabled) {
    auto& refresh = actions[actionCount++];
    refresh.label = tr(STR_REFRESH);
    refresh.action = ACTION_REFRESH;
    if (activeNav().selected == footerRefreshSelection()) refresh.state = fui::StateFocused;
  }

  if (!manifest.paginationHidden) {
    auto& next = actions[actionCount++];
    next.label = tr(STR_NEXT_SHORT);
    next.action = ACTION_PAGE;
    next.value = 1;
    next.enabled = nextRowVisible();
    if (activeNav().selected == footerNextSelection()) next.state = fui::StateFocused;
  }

  if (actionCount == 0) return;
  fui::FooterProps props;
  props.actions = actions;
  props.count = actionCount;
  screen.footer(props);
}

void PluginCatalogActivity::buildCacheScreen(UiScreen& screen) { buildBrowsingScreen(screen); }

void PluginCatalogActivity::buildPluginSettingsScreen(UiScreen& screen) { buildBrowsingScreen(screen); }

void PluginCatalogActivity::buildPlainListScreen(UiScreen& screen) {
  buildCatalogFooter(screen);
  const auto& theme = screen.theme();
  const fui::Rect body = screen.body();
  const int offset = contentSelectionOffset();
  const int selected = activeNav().selected - offset;
  const int selectedItem = selected >= 0 && selected < static_cast<int>(items.size()) ? selected : -1;
  const bool touch = screen.frame().device().hasTouch;
  const int16_t rowHeight = touch ? std::max(theme.rowHeight, theme.listTouchMinRowHeight) : theme.rowHeight;
  const int16_t rowGap = touch ? std::max(theme.listRowGap, theme.listTouchRowGap) : theme.listRowGap;
  const uint16_t visible = std::max<uint16_t>(1, fui::listVisibleRows(body, rowHeight, rowGap));
  const int requestedTop = browseRequestedTop.exchange(-1);
  uint16_t top = static_cast<uint16_t>(std::max(0, requestedTop >= 0 ? requestedTop : activeNav().top));
  const bool followSelection = activeNav().followOnBuild.exchange(false);
  if (selectedItem >= 0 && followSelection)
    top = fui::listTopIndexFor(selectedItem, top, visible, static_cast<uint16_t>(items.size()));
  const uint16_t visibleItems = std::max<uint16_t>(1, std::min<uint16_t>(visible, static_cast<uint16_t>(items.size())));
  const uint16_t maxTop = items.size() > visibleItems ? static_cast<uint16_t>(items.size() - visibleItems) : 0;
  top = std::min(top, maxTop);
  activeNav().top = top;
  browseViewportTop.store(top);
  browseViewportItems.store(visibleItems);

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(items.size());
  props.topIndex = top;
  props.selectedIndex = static_cast<int16_t>(selectedItem);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.rowStyles = selectionStyles(theme.listRow);
  screen.list(props);
}

void PluginCatalogActivity::buildBrowsingScreen(UiScreen& screen) {
  if (rowsDirty) {
    rebuildRowItems();
    rowsDirty = false;
  }

  if (rowItems.empty()) {
    if (nativeFooterActive()) buildCatalogFooter(screen);
    screen.centeredText(state == State::PLUGIN_PICKER ? tr(STR_NO_PLUGINS_INSTALLED) : tr(STR_NO_ENTRIES),
                        screen.theme().bodyText);
    return;
  }

  const bool previewImages = state == State::BROWSING && wantsPreviewImages();
  if (previewImages && manifest.gridView) {
    buildGridScreen(screen);
    return;
  }
  if (previewImages && manifest.listThumbnails) {
    buildThumbnailListScreen(screen);
    return;
  }
  if (nativeFooterActive()) {
    buildPlainListScreen(screen);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = state == State::PLUGIN_PICKER ? static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress)
                                                  : static_cast<uint16_t>(fui::InputTouch);
  props.valueInset = 8;  // air between the nav chevron and the row edge
  if (state == State::BROWSING) props.rowStyles = selectionStyles(screen.theme().listRow);
  if (state == State::PLUGIN_PICKER) {
    props.rowStyles = selectionStyles(screen.theme().listRow);
    props.rowStyles.disabled.background = fui::Paint::solid(fui::Color::LightGray);
    props.rowStyles.disabled.foreground = fui::Paint::solid(fui::Color::Black);
    // Mark the subtitle style caller-owned before resolving the theme.
    props.subtitleText = screen.theme().smallText;
    props.subtitleText.maxLines = 2;
  } else if (state == State::BROWSING && !manifest.isXmlList()) {
    // Catalog descriptions get enough room to be useful without replacing
    // the explicit Details page.
    props.subtitleText = screen.theme().smallText;
    props.subtitleText.maxLines = 3;
  }
  syncListViewport(screen, props, pluginTabsActive() ? 1 : 0);
  if (state == State::PLUGIN_PICKER) {
    fitPluginSubtitleToViewport(screen.target(), screen.body().height, props);
  }
  screen.list(props);
}

void PluginCatalogActivity::buildGridScreen(UiScreen& screen) {
  buildCatalogFooter(screen);
  const auto& theme = screen.theme();
  const uint8_t columns = manifest.gridColumns;
  const uint8_t targetRows = manifest.gridRows;
  const int offset = contentSelectionOffset();
  const int selected = activeNav().selected - offset;
  const int selectedItem = selected >= 0 && selected < static_cast<int>(items.size()) ? selected : -1;

  const int16_t gap = std::max<int16_t>(2, theme.spaceSm / 2);
  const int16_t edgeInset = std::max<int16_t>(8, static_cast<int16_t>(theme.spaceSm * 2));
  const int16_t verticalInset = std::max<int16_t>(2, gap);
  const int16_t gridTopGap = std::max<int16_t>(4, gap);
  fui::Rect gridRect = screen.body().inset(fui::Insets{verticalInset, edgeInset, verticalInset, edgeInset});
  if (gridRect.height > gridTopGap) {
    gridRect.y = static_cast<int16_t>(gridRect.y + gridTopGap);
    gridRect.height = static_cast<int16_t>(gridRect.height - gridTopGap);
  }

  fui::TextStyle titleText = theme.smallText;
  titleText.maxLines = 2;
  const int16_t labelGap = std::max<int16_t>(2, gap / 2);
  const int16_t labelHeight = static_cast<int16_t>(screen.target().lineHeight(titleText.font) * titleText.maxLines);
  const int16_t targetRowHeight =
      std::max<int16_t>(1, static_cast<int16_t>((gridRect.height - gap * (targetRows - 1)) / targetRows));
  const int16_t cellWidth =
      std::max<int16_t>(1, static_cast<int16_t>((gridRect.width - gap * (columns - 1)) / columns));
  const int16_t maxCoverHeight = std::max<int16_t>(36, static_cast<int16_t>(targetRowHeight - labelHeight - labelGap));
  const bool singleCell = columns == 1 && targetRows == 1;
  const int16_t previewWidthLimit =
      singleCell ? std::max<int16_t>(24, static_cast<int16_t>(cellWidth - 12)) : GRID_PREVIEW_WIDTH;
  const int16_t previewHeightLimit = singleCell ? maxCoverHeight : GRID_PREVIEW_HEIGHT;
  int16_t coverWidth = std::min<int16_t>(previewWidthLimit, static_cast<int16_t>(cellWidth - 4));
  coverWidth = std::min<int16_t>(coverWidth, static_cast<int16_t>(maxCoverHeight * 2 / 3));
  coverWidth = std::max<int16_t>(24, coverWidth);
  const int16_t coverHeight = std::max<int16_t>(
      36, std::min<int16_t>(previewHeightLimit,
                            std::min<int16_t>(maxCoverHeight, static_cast<int16_t>(coverWidth * 3 / 2))));
  const int16_t gridRowHeight =
      std::max<int16_t>(static_cast<int16_t>(coverHeight + labelHeight + labelGap), targetRowHeight);
  const uint16_t pageItems = fui::coverGridVisibleCells(gridRect, columns, gridRowHeight, gap);

  const uint16_t visibleItems =
      std::max<uint16_t>(1, std::min<uint16_t>(std::max<uint16_t>(1, pageItems), static_cast<uint16_t>(items.size())));
  const uint16_t maxTop = items.size() > visibleItems ? static_cast<uint16_t>(items.size() - visibleItems) : 0;
  const int requestedTop = browseRequestedTop.exchange(-1);
  uint16_t top = static_cast<uint16_t>(std::max(0, requestedTop >= 0 ? requestedTop : activeNav().top));
  const bool followSelection = activeNav().followOnBuild.exchange(false);
  if (selectedItem >= 0 && followSelection) {
    top = fui::coverGridTopIndexFor(static_cast<uint16_t>(selectedItem), static_cast<uint16_t>(items.size()), columns,
                                    pageItems);
  }
  // Keep the final viewport full whenever enough items exist. With a 16-item
  // source page and a 6-cover viewport, swipes land at 1-6, 7-12, then 11-16.
  top = std::min(top, maxTop);
  activeNav().top = top;
  browseViewportTop.store(top);
  browseViewportItems.store(visibleItems);

  fui::CoverGridProps props;
  props.count = static_cast<uint16_t>(items.size());
  props.topIndex = top;
  props.selectedIndex = static_cast<int16_t>(selectedItem);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.columns = columns;
  props.columnLayout = fui::CoverGridColumnLayout::EqualWidth;
  props.coverSize = fui::Size{coverWidth, coverHeight};
  props.rowHeight = gridRowHeight;
  props.gap = gap;
  props.rowGap = gap;
  props.labelHeight = labelHeight;
  props.labelGap = labelGap;
  props.labelFollowsCover = false;
  props.labelInset = fui::Insets{0, 2, 0, 2};
  props.titleText = titleText;
  props.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  props.selectedCoverFrameGap = 3;
  props.selectedCoverFrameWidth = 5;
  props.selectedCoverFrameRadius =
      manifest.coverRadius > 0 ? static_cast<uint8_t>(std::min<int>(255, manifest.coverRadius + 3)) : 0;
  props.scrollIndicator = items.size() > pageItems;
  props.itemProviderUserData = this;
  props.itemProvider = [](uint16_t index, void* user) {
    auto* self = static_cast<PluginCatalogActivity*>(user);
    return fui::coverGridItem(self->items[index].title.c_str(), static_cast<int16_t>(index));
  };
  props.coverPainterUserData = this;
  props.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::CoverGridItem&, uint16_t index, void* user) {
    return static_cast<PluginCatalogActivity*>(user)->paintPreview(rect, index);
  };
  fui::coverGrid(screen.frame(), gridRect, props);
}

void PluginCatalogActivity::buildThumbnailListScreen(UiScreen& screen) {
  buildCatalogFooter(screen);
  const auto& theme = screen.theme();
  const fui::Rect body = screen.body();
  constexpr int targetVisibleItems = 5;
  const bool touch = screen.frame().device().hasTouch;
  const int16_t normalHeight = touch ? std::max(theme.rowHeight, theme.listTouchMinRowHeight) : theme.rowHeight;
  const int16_t rowGap = touch ? std::max(theme.listRowGap, theme.listTouchRowGap) : theme.listRowGap;
  const int16_t twoRowHeight = static_cast<int16_t>(normalHeight * 2 + rowGap);
  const int target = std::max(1, std::min<int>(targetVisibleItems, items.size()));
  const int16_t fitHeight =
      std::max<int16_t>(normalHeight, static_cast<int16_t>((body.height - rowGap * (target - 1)) / target));
  const int16_t cardHeight = std::min<int16_t>(twoRowHeight, fitHeight);
  const uint16_t visible = std::max<uint16_t>(1, fui::listVisibleRows(body, cardHeight, rowGap));

  const int selected = activeNav().selected - contentSelectionOffset();
  const int selectedItem = selected >= 0 && selected < static_cast<int>(items.size()) ? selected : -1;
  const uint16_t visibleItems = std::max<uint16_t>(1, std::min<uint16_t>(visible, static_cast<uint16_t>(items.size())));
  const uint16_t maxTop = items.size() > visibleItems ? static_cast<uint16_t>(items.size() - visibleItems) : 0;
  const int requestedTop = browseRequestedTop.exchange(-1);
  uint16_t top = static_cast<uint16_t>(std::max(0, requestedTop >= 0 ? requestedTop : activeNav().top));
  const bool followSelection = activeNav().followOnBuild.exchange(false);
  if (selectedItem >= 0 && followSelection)
    top = fui::listTopIndexFor(selectedItem, top, visible, static_cast<uint16_t>(items.size()));
  top = std::min(top, maxTop);
  activeNav().top = top;
  browseViewportTop.store(top);
  browseViewportItems.store(visibleItems);

  int16_t y = body.y;
  for (uint16_t index = top; index < items.size() && index < top + visible; ++index) {
    const fui::Rect rect{body.x, y, body.width, cardHeight};
    fui::BookCardProps card;
    card.title = items[index].title.c_str();
    card.value = static_cast<int16_t>(index);
    card.action = ACTION_ROW;
    card.state = selectedItem == static_cast<int>(index) ? fui::StateSelected : fui::StateNormal;
    card.styles = selectionStyles(theme.listRow);
    card.titleText = theme.bodyText;
    card.titleText.maxLines = 2;
    card.authorText = theme.smallText;
    card.metaText = theme.smallText;
    card.progressMax = 0;
    card.centerTextOnCover = true;
    card.author = items[index].author.empty() ? nullptr : items[index].author.c_str();
    std::string dest;
    const bool saved = downloadDestination(items[index], dest) && Storage.exists(dest.c_str());
    card.meta = items[index].status.empty() ? nullptr : items[index].status.c_str();
    constexpr char savedBadge[] = "SAVED";
    constexpr int savedPadX = 6;
    constexpr int savedPadY = 4;
    constexpr int savedInset = 8;
    const int savedTextWidth = saved ? renderer.getTextWidth(SMALL_FONT_ID, savedBadge, EpdFontFamily::BOLD) : 0;
    const int savedTextHeight = saved ? renderer.getTextHeight(SMALL_FONT_ID) : 0;
    const int savedBadgeWidth = saved ? savedTextWidth + savedPadX * 2 : 0;
    const int savedBadgeHeight = saved ? savedTextHeight + savedPadY * 2 : 0;
    if (saved) card.padding.right = static_cast<int16_t>(card.padding.right + savedBadgeWidth + savedInset);
    const int16_t coverHeight = std::max<int16_t>(1, std::min<int16_t>(LIST_PREVIEW_HEIGHT, cardHeight - 12));
    card.coverSize =
        fui::Size{std::max<int16_t>(1, std::min<int16_t>(LIST_PREVIEW_WIDTH, coverHeight * 2 / 3)), coverHeight};
    card.coverPainterUserData = this;
    card.coverPainter = [](fui::DrawTarget&, fui::Rect coverRect, const fui::BookCardProps& props, void* user) {
      return static_cast<PluginCatalogActivity*>(user)->paintPreview(coverRect, props.value);
    };
    fui::bookCard(screen.frame(), rect, card);
    if (saved) {
      const int badgeX = rect.x + rect.width - card.padding.right + savedInset;
      const int badgeY = rect.y + (rect.height - savedBadgeHeight) / 2;
      renderer.fillRect(badgeX, badgeY, savedBadgeWidth, savedBadgeHeight, false);
      renderer.drawRect(badgeX, badgeY, savedBadgeWidth, savedBadgeHeight);
      renderer.drawText(SMALL_FONT_ID, badgeX + savedPadX, badgeY + savedPadY, savedBadge, true, EpdFontFamily::BOLD);
    }
    y = static_cast<int16_t>(y + cardHeight + rowGap);
  }
}

// Derives rowItems from the current state's row source: the browse lists
// (LIST_PICKER) or the pager rows bracketing the items (BROWSING). Labels
// point into `manifest`/`items` strings, which outlive the buffer.
void PluginCatalogActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(rowCount());
  const auto addRow = [&](const char* label, const char* subtitle = nullptr, const char* value = nullptr) {
    fui::ListItem row;
    row.label = label;
    row.subtitle = subtitle && subtitle[0] ? subtitle : nullptr;
    row.value = value && value[0] ? value : nullptr;
    row.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(row);
  };
  const auto addCacheActions = [&] {
    // Only check the small seed marker: no per-file enumeration or metadata
    // reads while opening the Cache screen.
    if (manifest.hasCacheSeed()) {
      const bool seeded = Storage.exists(cacheSeedMarkerPath().c_str());
      addRow(tr(STR_PLUGIN_CACHE_SEED), nullptr, seeded ? tr(STR_INSTALLED) : tr(STR_PLUGIN_CACHE_SEED_NOT_INSTALLED));
      addRow(seeded ? tr(STR_PLUGIN_CACHE_SEED_UPDATE) : tr(STR_PLUGIN_CACHE_SEED_DOWNLOAD), nullptr, ">");
    }
    addRow(tr(STR_PLUGIN_CACHE_REFRESH_NOW), nullptr, ">");
    addRow(tr(STR_PLUGIN_CACHE_CLEAR), nullptr, ">");
  };
  if (state == State::PLUGIN_PICKER) {
    if (showOpds) addRow(tr(STR_OPDS_BROWSER), tr(STR_OPDS_SERVERS));
    if (showPluginHubInstallRow) addRow(tr(STR_PLUGIN_HUB), tr(STR_PLUGIN_HUB_DESCRIPTION), ">");
    const auto addPluginRow = [&](const PluginRef& plugin) {
      // Keep Plugin Hub pinned above the Plugins divider alongside OPDS.
      const char* subtitle = plugin.description.empty() ? nullptr : plugin.description.c_str();
      const char* version = plugin.version.empty() ? nullptr : plugin.version.c_str();
      switch (plugin.deviceKind) {
        case PluginLocations::DeviceKind::None:
          addRow(plugin.title.c_str(), subtitle ? subtitle : tr(STR_PLUGIN_WEB_ONLY),
                 version ? version : (plugin.readmePath.empty() ? nullptr : ">"));
          break;
        case PluginLocations::DeviceKind::Catalog:
          addRow(plugin.title.c_str(), subtitle, version ? version : ">");
          break;
        case PluginLocations::DeviceKind::Background:
          addRow(plugin.title.c_str(), subtitle, version ? version : (plugin.readmePath.empty() ? nullptr : ">"));
          break;
      }
    };
    if (pinnedPluginCount() > 0) addPluginRow(installedPlugins.front());
    if (installedPlugins.size() > static_cast<size_t>(pinnedPluginCount())) {
      fui::ListItem header;
      header.label = "Plugins";
      header.enabled = false;
      header.actionValue = static_cast<int16_t>(rowItems.size());
      rowItems.push_back(header);
    }
    for (size_t i = pinnedPluginCount(); i < installedPlugins.size(); ++i) addPluginRow(installedPlugins[i]);
    for (const auto& entry : installedExtensions) {
      if (entry.kind == InstalledExtension::Kind::Header) {
        fui::ListItem header;
        header.label = entry.title.c_str();
        // Non-interactive shaded divider; the picker uses the disabled-row
        // background for a full-width gray band without any SDK changes.
        header.enabled = false;
        header.actionValue = static_cast<int16_t>(rowItems.size());
        rowItems.push_back(header);
      } else {
        // The version value is in the same right-aligned column as plugin versions.
        addRow(entry.title.c_str(), entry.description.empty() ? "No description available" : entry.description.c_str(),
               entry.version.empty() ? ">" : entry.version.c_str());
      }
    }
  } else if (state == State::LIST_PICKER) {
    for (const auto& list : manifest.browseLists) addRow(list.title.c_str());
  } else if (cacheTabActive()) {
    addCacheActions();
  } else if (settingsTabActive()) {
    const auto addSettingRow = [&](const Manifest::Setting& setting) {
      fui::ListItem row;
      row.label = setting.title.c_str();
      row.actionValue = static_cast<int16_t>(rowItems.size());
      const std::string current = settingValue(setting);
      if (setting.type == Manifest::Setting::Type::Toggle) {
        row.toggle = true;
        row.toggleChecked = strcasecmp(current.c_str(), "true") == 0 || current == "1";
      } else {
        for (const auto& option : setting.options) {
          if (option.value == current) {
            row.value = option.title.c_str();
            break;
          }
        }
      }
      rowItems.push_back(row);
    };

    if (cacheSettingsExpanded) {
      for (const auto& setting : manifest.settings)
        if (settingVisible(setting) && isCacheSetting(setting)) addSettingRow(setting);
      addCacheActions();
    } else {
      for (const auto& setting : manifest.settings)
        if (settingVisible(setting) && !isCacheSetting(setting)) addSettingRow(setting);
      if (cacheUsable()) {
        addRow("Cache", nullptr, ">");
      }
    }
  } else {
    if (!nativeFooterActive() && prevRowVisible()) addRow(tr(STR_PREV_PAGE), nullptr, ">");
    for (const auto& entry : items) {
      std::string dest;
      const bool saved = !entry.isDir && downloadDestination(entry, dest) && Storage.exists(dest.c_str());
      addRow(entry.title.c_str(), entry.author.c_str(), entry.isDir ? ">" : (saved ? "SAVED" : entry.status.c_str()));
    }
    if (!nativeFooterActive() && nextRowVisible()) addRow(tr(STR_NEXT_PAGE), nullptr, ">");
  }
}

void PluginCatalogActivity::releaseRows() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // rows; stop routing touches against it until the next render.
  closeRouting();
  rowsDirty = true;
}
