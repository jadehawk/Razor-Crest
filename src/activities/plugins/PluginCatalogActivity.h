#pragma once
#include <Bitmap.h>
#include <HalStorage.h>
#include <I18n.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "activities/CatalogActivity.h"
#include "components/OptionPopup.h"
#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"

class HalFile;
namespace freeink {
class SecureHttpClient;
}

// Picker entry; an empty manifestPath marks an inert browser-only plugin.
struct PluginRef {
  std::string name;          // folder name
  std::string dir;           // exact discovered <root>/<name> install directory
  std::string title;         // from device.json or manifest.json (falls back to name)
  std::string version;       // installed manifest version, if supplied
  std::string description;   // one-line summary, if provided
  std::string manifestPath;  // device.json path, "" for a web-only plugin
  std::string readmePath;    // README.md path when present
  PluginLocations::DeviceKind deviceKind = PluginLocations::DeviceKind::None;
};

// Rescan plugin roots on demand; metadata is owned by the picker.
std::vector<PluginRef> discoverPlugins();

// Manifest-driven picker, catalog, download, and sign-in screens.
// See docs/sd-plugins.md for the device.json schema.
class PluginCatalogActivity : public CatalogActivity {
 public:
  // showOpds adds the OPDS row; rootMode returns Home instead of popping to Settings.
  // Out-of-line construction/destruction needs the complete SecureHttpClient type.
  explicit PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool showOpds = false,
                                 bool rootMode = false);
  ~PluginCatalogActivity() override;

  void onEnter() override;
  void onExit() override;

 protected:
  // Generic hosts can claim supported local plugin manifests while leaving all
  // existing catalog, events, README, and install behavior unchanged.
  std::string pickerManifestForRow(int index) const {
    if (state != State::PLUGIN_PICKER) return {};
    const int pluginIndex = pickerPluginForRow(index);
    if (pluginIndex < 0) return {};
    return installedPlugins[pluginIndex].manifestPath;
  }
  void activateStandardPluginRow(int index) { PluginCatalogActivity::activateIndex(index); }

 private:
  static constexpr freeink::ui::ActionId ACTION_PLUGIN_TAB = ACTION_USER + 2;
  static constexpr freeink::ui::ActionId ACTION_PAGE = ACTION_USER + 3;
  static constexpr freeink::ui::ActionId ACTION_REFRESH = ACTION_USER + 4;

  struct Manifest {
    // Shared on-SD token and flat {cfg.KEY} configuration.
    std::string tokenFile, tokenPath;
    bool tokenPersistent = true;
    std::string configFile;
    // JSON uses dotted field paths; XML uses element/attribute selectors.
    std::string browseFormat;
    // Browse request: templates may use {token}, {cfg.KEY}, {page}, {limit}.
    pluginhttp::RequestSpec browseReq;
    std::string itemsPath;  // JSON: dotted path to the item array; "" = response root
    // JSON field paths (dotted); XML field selectors ("elem", "elem@attr", "@attr").
    std::string titlePath, authorPath, idPath, urlPath, thumbnailPath;
    // Optional JSON detail fields are streamed only for the selected item so a
    // page of long descriptions/repository metadata does not stay in DRAM.
    std::string descriptionPath, detailAuthorPath, repositoryPath;
    std::string detailsUrlPath, detailsFormatPath;
    // Compare this item field against the installed manifest version for badges.
    std::string versionPath;
    // Optional catalog-entry compatibility fields. Empty paths mean no
    // restriction; entries that omit the mapped values remain visible.
    std::string compatFamilyPath, compatFirmwarePath, compatApiMinPath, compatRequiresPath;
    bool tracksInstalls() const { return !versionPath.empty(); }
    bool hasItemDetails() const {
      return tracksInstalls() || !descriptionPath.empty() || !repositoryPath.empty() || !detailsUrlPath.empty();
    }
    bool filtersCompatibility() const {
      return !compatFamilyPath.empty() || !compatFirmwarePath.empty() || !compatApiMinPath.empty() ||
             !compatRequiresPath.empty();
    }
    enum class ImageFormat : uint8_t { Bmp, Jpeg, Png, Auto };
    enum class SelectionStyle : uint8_t { Theme, Light, Dark, Outline };
    enum class CacheRefreshScope : uint8_t { Current, Catalog, Plugin };
    enum class CacheResourcePolicy : uint8_t { Disabled, Ttl, Immutable };
    pluginhttp::RequestSpec imageReq;
    ImageFormat imageFormat = ImageFormat::Jpeg;
    // Presentation values accept literals or existing {cfg.KEY} templates.
    std::string viewTpl = "list", gridColumnsTpl = "4", gridRowsTpl = "2", listThumbnailsTpl = "false";
    std::string gridPageSizeTpl = "6", listPageSizeTpl = "10";
    std::string selectionStyleTpl = "theme", coverRadiusTpl = "0";
    std::string cacheRefreshTpl = "7d", cacheLimitMbTpl = "32";
    std::string cacheNamespaceTpl, cacheGenerationTpl;
    std::string hidePaginationTpl = "false";
    bool gridView = false;
    uint8_t gridColumns = 4;
    uint8_t gridRows = 2;
    uint8_t coverRadius = 0;
    bool listThumbnails = false;
    bool refreshEnabled = false;
    bool paginationHidden = false;
    SelectionStyle selectionStyle = SelectionStyle::Theme;
    bool cacheEnabled = false;
    bool cacheTokenScoped = false;
    bool cacheStaleFallback = true;
    CacheRefreshScope cacheManualRefreshScope = CacheRefreshScope::Catalog;
    CacheResourcePolicy cacheDetailsPolicy = CacheResourcePolicy::Disabled;
    uint16_t cacheRefreshDays = 7;  // 0 = always refresh from the network
    uint16_t cacheLimitMb = 32;
    std::string cacheNamespace, cacheGeneration, cacheSeedUrl;
    bool hasImages() const { return !imageReq.url.empty(); }
    bool hasCacheSeed() const { return cacheEnabled && !cacheSeedUrl.empty(); }
    int pageSize = 8;

    struct UiTab {
      enum class Kind : uint8_t { Browse, Cache, Settings };
      std::string id, title;
      Kind kind = Kind::Browse;
    };
    std::vector<UiTab> tabs;
    std::string defaultTab;

    struct SettingOption {
      std::string title, value;
    };
    struct Setting {
      enum class Type : uint8_t { Toggle, Choice };
      std::string key, title, defaultValue;
      std::string visibleDeviceCapability;
      Type type = Type::Choice;
      std::vector<SettingOption> options;
    };
    std::vector<Setting> settings;
    // Named JSON views override the browse URL/body; Back returns to their picker.
    struct BrowseList {
      std::string title, url, body;
      std::string kind = "plugin";  // v2 index; legacy lists default to plugin
      std::string itemsPath;        // v2 item array key
      std::string noticeTitle, noticeMessage, noticeConfirm, noticeCancel;
      bool hasNotice() const { return !noticeMessage.empty(); }
    };
    std::vector<BrowseList> browseLists;
    // Optional remote index that supplies named JSON views dynamically. The
    // standard contract is {"lists":[...]}; generated catalog indexes may
    // expose the same title/url records under "catalogs".
    std::string browseListsUrl;
    std::string capabilityListsUrl;  // optional v2 index; legacy URL remains intact
    // JSON search overrides URL/body independently; empty fields reuse browse.
    std::string searchUrl, searchBody;
    bool hasSearch() const { return (!searchUrl.empty() || !searchBody.empty()) && browseFormat != "xml"; }
    // XML list options:
    std::string xmlItem;                     // local-name of the repeating element (required)
    std::string xmlContainer;                // local-name whose presence marks a navigable folder
    bool xmlSkipSelf = false;                // drop the entry whose url equals the request url
    bool xmlResolveUrls = false;             // resolve url field against the request origin
    std::vector<std::string> xmlExtensions;  // allowed file extensions ("" = all)

    bool isXmlList() const { return browseFormat == "xml"; }
    // Empty dlUrlPath means a direct URL; otherwise resolve it through an API hop.
    pluginhttp::RequestSpec downloadReq;
    std::string dlUrlPath;
    // Optional HTTP Basic credentials for the file GET (webdav); templates.
    std::string dlUser, dlPass;
    std::string destDir, filenameTpl;
    std::string previewOnSelectTpl = "false";
    bool previewOnSelect = false;
    // Bundle item fields: base URL and relative paths, installed under destDir/subdir.
    std::string bundleBasePath, bundleFilesPath, bundleSubdir;
    bool isBundle() const { return !bundleFilesPath.empty(); }
    // Optional sidecar written after a successful download; templates may use
    // {id}, {title}, {md5} (MD5 of the destination path).
    std::string sidecarPath, sidecarBody;
    // Both grants write the shared token file; device_code also shows a QR/code.
    std::string authType;  // "device_code" (default) or "password"
    pluginhttp::RequestSpec authReq, pollReq;
    std::string authCodePath, authVerifyPath, authDeviceCodePath;
    std::string authIntervalPath, authExpiresPath, authTokenPath, authErrorPath;
    // Optional presentation-only hints for device-code screens. These never change
    // the QR payload or authorization flow; they only replace the raw verification
    // URL with cleaner service-specific instructions when provided by the plugin.
    std::string authDisplayUrl, authQrHint, authManualHint;

    bool hasDeviceCode() const { return authType == "device_code" && !authReq.url.empty() && !pollReq.url.empty(); }
    bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
  };

  struct Item {
    std::string title, author, id, url, thumbnail;
    std::string version;  // catalog version (tracksInstalls only)
    std::string status;   // computed install/update badge shown in the row value
    bool isDir = false;   // a container/folder (navigable), not a downloadable file
    // Bundle download only: base URL + relative file paths for this item.
    std::string base;
    std::vector<std::string> files;
  };

  struct ItemDetails {
    std::string description, author, repository;
    std::string detailsUrl, detailsFormat;
  };

  std::string manifestPath;  // empty while the picker is showing
  std::string catalogTitle;
  Manifest manifest;
  bool manifestLoaded = false;
  bool browseListsRemoteAttempted = false;
  std::vector<PluginRef> installedPlugins;
  // Compact picker-only rows: headers and one entry per installed SD package.
  // Built after registry validation, never during rendering.
  struct InstalledExtension {
    enum class Kind : uint8_t { Header, Theme, Wallpaper };
    Kind kind = Kind::Header;
    std::string id, title, dir, version, author, description;
  };
  std::vector<InstalledExtension> installedExtensions;
  int pinnedPluginCount() const;
  int pluginPickerStart() const;
  int pickerRowForPlugin(int index) const;
  int pickerPluginForRow(int row) const;
  int extensionPickerStart() const;
  bool pickerHeaderRow(int index) const;
  int nextPickerRow(int current, int direction, int steps = 1) const;
  void discoverInstalledExtensions();
  void showInstalledExtensionDetails(int index);
  void confirmUninstallInstalledExtension(int index);
  bool pluginHubAvailable = false;
  bool showPluginHubInstallRow = false;
  bool ignoreHubInstallerBackRelease = false;
  OptionPopup pluginHubPopup;
  bool showOpds = false;
  bool rootMode = false;
  std::string pickerReturnName;  // plugin identity to reselect after leaving a catalog
  std::vector<Item> items;
  // Row buffer over the active content. Footer paging is a separate native
  // FreeInkUI control and never occupies an item row.
  std::vector<freeink::ui::ListItem> rowItems;
  bool rowsDirty = true;
  uint16_t previewReadyMask = 0;
  HalFile previewFile;
  Bitmap previewBitmap{previewFile};
  std::string token;
  pluginhttp::Headers config;  // {cfg.KEY} values
  int page = 1;
  bool hasMore = false;
  int currentList = -1;  // index into manifest.browseLists; -1 = none/default
  std::string searchQuery;
  bool searchActive = false;
  // Reuse browse TLS; release in the picker and before a single-file download.
  std::unique_ptr<freeink::SecureHttpClient> session;
  // XML-list folder navigation: current container URL and the trail back out.
  std::string browseCurrentUrl;
  std::vector<std::string> browseHistory;
  // Device-code sign-in state
  std::string authUserCode, authVerifyUrl, authDeviceCode;
  unsigned long authIntervalMs = 5000;
  unsigned long authNextPollMs = 0;
  unsigned long authDeadlineMs = 0;
  int activePluginTab = -1;
  std::vector<freeink::ui::ListNav> pluginTabNavs;
  bool navigationStartedOnTabs = false;
  // Touch browse swipes scroll the viewport independently of the focused item.
  // Builders publish the viewport they actually drew; input posts a requested
  // top index without mutating render-owned ListNav::top from the input task.
  std::atomic<int> browseViewportTop{0};
  std::atomic<int> browseViewportItems{1};
  std::atomic<int> browseRequestedTop{-1};
  bool browseRefreshNeeded = false;
  bool browseLoadedFromCache = false;
  bool forceCacheRefresh = false;
  int pendingNetworkPage = 0;
  int pendingItemActivation = -1;
  int pendingItemDownload = -1;
  bool pendingCacheSeedInstall = false;
  bool pendingPreviewFill = false;
  std::string browseSourcePath;
  std::string remoteCacheGeneration;

  struct CacheMetadata {
    int64_t capturedAt = 0;
    uint32_t epoch = 0;
    std::string generation;
  };

  struct CacheStats {
    uint32_t pages = 0;
    uint32_t images = 0;
    uint64_t bytes = 0;
    int64_t lastRefreshAt = 0;
  } cacheStats;
  bool cacheStatsDirty = true;
  bool cacheSettingsExpanded = false;
  std::string cacheTabLabel, cacheLastRefreshValue, cachePagesValue, cacheImagesValue, cacheStorageValue;
  std::string cacheSeedValue;
  // QR placement measured by buildScreen (AUTH state); drawn as a raw-renderer
  // overlay in drawFooter() after the app has painted.
  freeink::ui::Rect authQrRect{};

  void enterPluginPicker();
  void confirmUninstallPlugin(int pickerIndex);
  void enterCatalog();
  void exitCatalog();
  // Clear items, paging, list and search state, and the row buffer.
  void resetBrowse();
  // JSON catalog with named lists and none picked yet.
  bool wantsListPicker() const;
  void openList(int index);
  bool loadManifest();
  bool loadBrowseListIndex();
  bool loadToken();
  bool clearEphemeralToken();
  void startDeviceAuth();
  void loadConfig();
  bool saveToken(const std::string& value);
  // Show named lists when present, otherwise fetch the first page.
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override;
  void performSearch(const std::string& query) override;
  void downloadFinished(bool cancelled) override;
  // Header label for the browsing screen (list title / search / page suffix).
  std::string browsingHeaderLabel() const;
  // JSON pagination state; enhanced plugin UIs render paging in the native footer.
  bool prevRowVisible() const;
  bool nextRowVisible() const;
  // Zero outside list states disables the base list protocol.
  int rowCount() const;
  int listCount() const override { return rowCount(); }
  // Row dispatch: picker rows pick; item rows open/download.
  void activateIndex(int index) override;
  void render(RenderLock&& lock) override;
  void buildScreen(UiScreen& screen) override;
  freeink::ui::ListNav& activeNav() override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  void navigateButtons() override;
  // Move one visible browse batch. Touch keeps the focused item unchanged;
  // hardware batch navigation moves focus into the newly visible batch.
  bool stepBrowseViewport(int direction, bool moveSelection);
  bool handleButtons() override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;
  void rebuildRowItems();
  // Items (and the interaction table indexing them) are about to be replaced:
  // stop routing and mark the row buffer for rebuild.
  void releaseRows();
  void buildAuthScreen(UiScreen& screen);
  void buildBrowsingScreen(UiScreen& screen);
  void buildPluginTabBar(UiScreen& screen);
  void buildCatalogFooter(UiScreen& screen);
  void buildCacheScreen(UiScreen& screen);
  void buildPluginSettingsScreen(UiScreen& screen);
  void buildPlainListScreen(UiScreen& screen);
  void buildGridScreen(UiScreen& screen);
  void buildThumbnailListScreen(UiScreen& screen);
  bool pluginTabsActive() const;
  bool browseTabActive() const;
  bool cacheTabActive() const;
  bool settingsTabActive() const;
  bool tabsFocused() const;
  void switchPluginTab(int index, bool keepFocus = true);
  void stepPluginTab(int direction);
  static void onPluginTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onPageEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onRefreshEvent(const freeink::ui::ActionEvent& event, void* user);
  void resolvePresentation();
  std::string configValue(const std::string& key, const std::string& fallback = {}) const;
  std::string settingValue(const Manifest::Setting& setting) const;
  bool deviceCapabilityAvailable(const std::string& capability) const;
  bool settingVisible(const Manifest::Setting& setting) const;
  bool saveSettingValue(const Manifest::Setting& setting, const std::string& value);
  freeink::ui::StyleSet selectionStyles(const freeink::ui::StyleSet& base) const;
  bool wantsPreviewImages() const;
  bool nativeFooterActive() const;
  int contentSelectionOffset() const;
  int footerPreviousSelection() const;
  int footerRefreshSelection() const;
  int footerNextSelection() const;
  int navigationCount() const;
  void clearPreviews();
  bool cachedPreviewValid(const std::string& path) const;
  bool fetchPreviewToPath(const Item& item, const std::string& dest, int targetWidth, int targetHeight);
  bool preparePreviews();
  bool paintPreview(freeink::ui::Rect rect, int itemIndex);
  static std::string previewPath(int itemIndex);
  bool cacheUsable() const;
  std::string cacheDir() const;
  std::string cacheSubstituted(std::string tpl, const Item* item = nullptr, int requestedPage = -1) const;
  std::string cacheCatalogKey() const;
  std::string cacheRequestKey(const pluginhttp::RequestSpec& request, const Item* item,
                              const std::string& identity) const;
  std::string cachePagePath(int requestedPage) const;
  std::string cachePageMetaPath(int requestedPage) const;
  std::string cacheResourcePath(const pluginhttp::RequestSpec& request, const Item* item, const std::string& identity,
                                const char* prefix, const char* extension) const;
  std::string cacheResourceMetaPath(const std::string& resourcePath) const;
  std::string cacheImagePath(const Item& item) const;
  std::string cachePluginEpochPath() const;
  std::string cacheCatalogEpochPath() const;
  bool ensureCacheDir();
  bool readCacheMetadata(const std::string& path, CacheMetadata& metadata) const;
  bool writeCacheMetadata(const std::string& path, const CacheMetadata& metadata);
  bool readCacheEpoch(const std::string& path, uint32_t& epoch) const;
  bool writeCacheEpoch(const std::string& path, uint32_t epoch);
  uint32_t requiredCacheEpoch() const;
  bool bumpCacheEpoch(Manifest::CacheRefreshScope scope);
  bool cacheTimestampFresh(int64_t capturedAt) const;
  bool cacheEntryFresh(const CacheMetadata& metadata, bool applyTtl) const;
  bool tryLoadCachedPage(int requestedPage, bool allowStale);
  bool storeFetchedPage(int requestedPage);
  bool tryLoadCachedTextResource(const std::string& path, Manifest::CacheResourcePolicy policy, bool allowStale,
                                 std::string& out) const;
  bool storeCachedTextResource(const std::string& path, const std::string& body);
  void noteCacheRefresh();
  void refreshCacheStats();
  void enforceCacheLimit();
  void clearCatalogCache();
  std::string cacheAgeShort() const;
  void refreshCacheNow();
  void confirmClearCache();
  int cacheRowCount() const;
  bool isCacheSetting(const Manifest::Setting& setting) const;
  int cacheSettingCount() const;
  int settingsRowCount() const;
  int settingsManifestIndex(int displayIndex, bool cacheOnly) const;
  void activateSetting(int manifestIndex);
  void activateCacheRow(int index);
  std::string cacheSeedMarkerPath() const;
  void installCacheSeed();
  bool extractCacheSeed(const std::string& archivePath);
  // Browse url/body with the selected browse list's overrides applied.
  const std::string& activeBrowseUrl() const;
  const std::string& activeBrowseBody() const;
  // Copies `headers` with `substituted()` applied to each value.
  pluginhttp::Headers substitutedHeaders(const pluginhttp::Headers& headers, const Item* item) const;
  void fetchPage(int newPage);
  // Read installed versions once per page, outside the render path.
  void computeInstallStatus();
  bool parseXmlList();
  bool parseBrowseResponse();
  void activateItem(int itemIndex);  // XML list: navigate into a folder, else details/preview/download
  void previewItem(int itemIndex);
  bool loadItemDetails(int itemIndex, ItemDetails& details);
  void showItemDetails(int itemIndex);
  void downloadItem(int itemIndex);
  HttpDownloader::DownloadError downloadExtension(const Item& item);
  void confirmRemoveExtension(int itemIndex);
  bool downloadDestination(const Item& item, std::string& dest);
  HttpDownloader::DownloadError finalizeDownloadedFile(const Item& item, const std::string& dest);
  HttpDownloader::DownloadError savePreviewFile(const Item& item, const std::string& previewPath);
  HttpDownloader::DownloadError downloadBundle(const Item& item);
  HttpDownloader::DownloadError downloadBook(const Item& item);
  void beginAuth();
  void pollAuth();
  bool refreshCredentialToken();  // password grant: mint a token from config creds
  // Load credentials and stream a JSON/XML browse response to the SD temp file.
  // Retries a password grant once on 401/403; displays failures and returns false.
  bool fetchBrowseResponse();
  int apiRequest(const pluginhttp::RequestSpec& req, String& out);
  pluginhttp::RequestSpec substitutedRequest(const pluginhttp::RequestSpec& req, const Item* item = nullptr) const;
  std::string substituted(std::string tpl, const Item* item) const;
  // Where downloads land: a "dest_dir" in the plugin's config.json (its web
  // card can offer the setting), else the manifest's download.dest_dir.
  std::string downloadDir() const;
};
