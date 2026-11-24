#if CROSSPOINT_DESSEN_APPS
#include "DessenAppActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>
#include <sdk/dessen_abi.h>  // DSN_HOME_*

#include <ctime>
#include <utility>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"

namespace fui = freeink::ui;

// RTC-backed silent maintenance reboots resume this exact standalone app.
extern void silentRestartToDessenOperation(const std::string& appPath, int action);
extern void silentRestartToDessen(const std::string& appPath);

// Shared with the sleep-time plugin drain. Called only after unloading the
// RV32 guest, so X3 can reclaim its internal heap before Wi-Fi initialization.
extern bool connectLastUsedWifi(uint32_t timeoutMs, bool showPopup);

namespace {
constexpr unsigned long FORCE_QUIT_HOLD_MS = 1000;
// Instruction budget per render pass, so a busy app still yields to the UI.
// A compute-heavy app advances STEP_BUDGET*MAX_STEPS_PER_RENDER instructions per
// render pass; passes are separated by the ~10ms UI-loop tick, so fewer, larger
// passes mean less wall-clock spent waiting between them. Force-quit (the Back
// long-press) is checked in loop() before the render lock, so raising this does
// not slow it. ponytail: tuned by eye; raise if a heavy app's first frame drags.
constexpr uint32_t STEP_BUDGET = 200000;
constexpr int MAX_STEPS_PER_RENDER = 16;

// The original sideloaded demo uses reading-stats-dessen; newer packages use
// reading-statistics-dessen-rust. Both are the same first-party app. Keep
// network operations restricted to an exact installed plugin root/name.
bool isStandaloneStatsApp(const std::string& path) {
  constexpr const char* names[] = {"reading-stats-dessen", "reading-statistics-dessen-rust"};
  constexpr const char* roots[] = {"/.crosspoint/plugins/", "/plugins/", "/.plugins/"};
  for (const char* root : roots)
    for (const char* name : names)
      if (path == std::string(root) + name + "/app.bin") return true;
  return false;
}

bool hasEvent(const fui::InputSnapshot& s) {
  return s.confirm || s.back || s.prev || s.next || s.focusPrev || s.focusNext || (s.touchReleased && s.touchX >= 0);
}
}  // namespace

DessenAppActivity::DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath,
                                     bool homeLauncher, int nativeOperation, bool resumeAfterNetwork)
    : Activity("DessenApp", renderer, mappedInput),
      appPath(std::move(appPath)),
      homeLauncher(homeLauncher),
      uiTarget(makeUiTarget(renderer)),
      nativeOperation(nativeOperation),
      resumeAfterNetwork(resumeAfterNetwork) {
  // Fixed fonts for sideloaded dashboard apps, independent of UI theme scale.
  // Keep the old Dessen large slot (3) as an alias of Noto Sans 18.
  // OMIT_FONTS targets only register Noto Sans 14 and use it as a fallback.
#ifndef OMIT_FONTS
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, NOTOSANS_18_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_12, NOTOSANS_12_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_14, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_18, NOTOSANS_18_FONT_ID);
#else
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_12, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_14, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_18, NOTOSANS_14_FONT_ID);
#endif
}

bool DessenAppActivity::loadApp() {
  HalFile file;
  if (!Storage.openFileForRead("DSN", appPath, file)) return false;
  const size_t size = file.fileSize();
  // Peek the header for the memory block the app asked for (2nd u32, little-endian).
  uint8_t hdr[DSN_HEADER_SIZE];
  if (size < DSN_HEADER_SIZE || file.read(hdr, DSN_HEADER_SIZE) != static_cast<int>(DSN_HEADER_SIZE)) return false;
  const uint32_t memSize = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | hdr[7] << 24;
  if (memSize < size || memSize > MAX_APP_MEMORY) {
    LOG_ERR("DSN", "App wants %u bytes (file %u, cap %u): out of range", static_cast<unsigned>(memSize),
            static_cast<unsigned>(size), static_cast<unsigned>(MAX_APP_MEMORY));
    return false;
  }
  memory = makeUniqueNoThrow<uint8_t[]>(memSize);  // > ~4 KB -> PSRAM on an S3
  if (!memory) {
    LOG_ERR("DSN", "OOM: %u bytes of app memory", static_cast<unsigned>(memSize));
    return false;
  }
  if (!file.seek(0) || file.read(memory.get(), size) != static_cast<int>(size)) {
    LOG_ERR("DSN", "Unreadable app: %s", appPath.c_str());
    return false;
  }
  if (!host.load(memory.get(), static_cast<uint32_t>(size), memSize)) {
    LOG_ERR("DSN", "Not a valid dessen app: %s", appPath.c_str());
    return false;
  }
  host.setRandom(esp_random);
  // appDir = the plugin folder; appName = its basename. These scope the app's
  // device id and key store.
  const size_t slash = appPath.find_last_of('/');
  const std::string appDir = slash == std::string::npos ? "/" : appPath.substr(0, slash);
  const size_t nameSlash = appDir.find_last_of('/');
  const std::string appName = nameSlash == std::string::npos ? appDir : appDir.substr(nameSlash + 1);
  services = makeUniqueNoThrow<CrosspointServices>(appName, appDir);
  if (!services) {
    LOG_ERR("DSN", "OOM: services");
    return false;
  }
  host.setServices(services.get());
  return true;
}

namespace {
constexpr char kStatsToken[] = "/.crosspoint/reading-statistics-token.json";
constexpr char kStatsOverviewUrl[] = "https://sync.techy-notes.com/api/device/overview";

constexpr char kStatsLoginUrl[] = "https://sync.techy-notes.com/api/device/login-code";

bool connectStatsWifi() {
  return WiFi.status() == WL_CONNECTED || connectLastUsedWifi(10000, false);
}

bool replaceStatsCache(const std::string& target, const std::string& tmp) {
  const std::string backup = target + ".bak";
  if (!Storage.exists(target.c_str()) && Storage.exists(backup.c_str()) &&
      !Storage.rename(backup.c_str(), target.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  const bool had = Storage.exists(target.c_str());
  if (had && !Storage.rename(target.c_str(), backup.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), target.c_str())) {
    Storage.remove(tmp.c_str());
    if (had) Storage.rename(backup.c_str(), target.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  return true;
}

bool writeStatsCacheJson(const std::string& path, const JsonDocument& doc, size_t limit) {
  std::string payload;
  serializeJson(doc, payload);
  if (payload.empty() || payload.size() > limit) return false;
  HalFile file;
  return Storage.openFileForWrite("DSN", path, file) &&
         file.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == payload.size();
}

bool refreshNativeWebLogin(const std::string& dir, const pluginhttp::Headers& headers) {
  constexpr size_t limit = 384;
  const std::string target = dir + "/web-login.json";
  const std::string tmp = target + ".part";
  const int status = pluginhttp::requestToFile(nullptr, kStatsLoginUrl, "POST", "", headers,
                                               tmp.c_str(), limit, nullptr, nullptr, false, 4000);
  std::string raw;
  JsonDocument doc;
  if (status != 200 || !Storage.readFileToString("DSN", tmp, limit, raw) ||
      deserializeJson(doc, raw) != DeserializationError::Ok || !doc["user_code"].is<const char*>() ||
      !doc["expires_in"].is<uint32_t>()) {
    LOG_ERR("DSN", "Native web login response unavailable or invalid: HTTP %d", status);
    Storage.remove(tmp.c_str());
    return false;
  }
  const std::string code = doc["user_code"].as<const char*>();
  const uint32_t expiresIn = doc["expires_in"].as<uint32_t>();
  const auto now = std::time(nullptr);
  bool valid = code.size() == 9 && code[4] == '-' && expiresIn > 0 && expiresIn <= 86400 &&
               now >= 946684800 && static_cast<uint64_t>(now) + expiresIn <= UINT32_MAX;
  for (size_t i = 0; valid && i < code.size(); ++i) {
    const char c = code[i];
    valid = i == 4 || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  }
  if (!valid) {
    Storage.remove(tmp.c_str());
    LOG_ERR("DSN", "Native web login has invalid code, lifetime, or reader clock");
    return false;
  }
  // Compare freshness using the same reader clock that will read the cache.
  // The server continues enforcing the original expiry and single use.
  doc.clear();
  doc["user_code"] = code;
  doc["expires_at"] = static_cast<uint32_t>(now) + expiresIn;
  if (!writeStatsCacheJson(tmp, doc, limit) || !replaceStatsCache(target, tmp)) {
    Storage.remove(tmp.c_str());
    LOG_ERR("DSN", "Native web login cache could not be saved");
    return false;
  }
  LOG_INF("DSN", "Native web login cache updated");
  return true;
}

bool disconnectNativeStats(const std::string& appPath) {
  std::string token;
  if (Storage.exists(kStatsToken)) {
    if (!pluginhttp::loadTokenFromFile(kStatsToken, "token", token)) return false;
    const pluginhttp::Headers headers = {{"Authorization", "Bearer " + token}};
    const std::string tmp = appPath.substr(0, appPath.find_last_of('/')) + "/disconnect.part";
    if (!connectStatsWifi()) return false;
    const int status = pluginhttp::requestToFile(nullptr, kStatsLoginUrl, "DELETE", "", headers,
                                                 tmp.c_str(), 128, nullptr, nullptr, false, 4000);
    Storage.remove(tmp.c_str());
    if (status != 200 && status != 401) return false;
    if (!pluginhttp::removeTokenFile(kStatsToken)) return false;
  }
  const auto dir = appPath.substr(0, appPath.find_last_of('/'));
  for (const char* cache : {"/server-overview.json", "/web-login.json"}) {
    const std::string path = dir + cache;
    Storage.remove(path.c_str());
    Storage.remove((path + ".part").c_str());
    Storage.remove((path + ".bak").c_str());
  }
  return true;
}

bool refreshNativeStatsSnapshot(const std::string& appPath) {
  const auto slash = appPath.find_last_of('/');
  if (slash == std::string::npos) return false;
  const std::string dir = appPath.substr(0, slash);
  if (!Storage.exists((dir + "/device.json").c_str())) {
    LOG_ERR("DSN", "Native overview: bundled pairing descriptor missing");
    return false;
  }
  std::string token;
  if (!pluginhttp::loadTokenFromFile(kStatsToken, "token", token) || token.empty() || token.size() > 128) {
    LOG_ERR("DSN", "Native overview: not paired (or invalid saved token)");
    return false;
  }
  // The guest is already gone before this call. One native saved-network join
  // (10 seconds) matches the established sleep-sync behavior on X3.
  if (!connectStatsWifi()) {
    LOG_ERR("DSN", "Native overview: saved Wi-Fi connection unavailable");
    return false;
  }
  const std::string target = dir + "/server-overview.json";
  const std::string tmp = target + ".part";
  const pluginhttp::Headers headers = {{"Authorization", "Bearer " + token}};
  const int status = pluginhttp::requestToFile(nullptr, kStatsOverviewUrl, "GET", "", headers, tmp.c_str(), 640, nullptr,
                                               nullptr, false, 4000);
  token.clear();
  const bool loginRefreshed = refreshNativeWebLogin(dir, headers);
  if (status != 200) {
    Storage.remove(tmp.c_str());
    LOG_ERR("DSN", "Native overview refresh failed: HTTP %d", status);
    return false;
  }
  // Reject malformed or incomplete successful responses before replacing the cache.
  {
    std::string raw;
    JsonDocument doc;
    bool valid = Storage.readFileToString("DSN", tmp, 640, raw) &&
                 deserializeJson(doc, raw) == DeserializationError::Ok &&
                 doc["source"] == "server" && doc["synced_at"].is<uint32_t>() &&
                 doc["synced_at"].as<uint32_t>() != 0;
    for (const char* field : {"total_seconds", "month_seconds", "month_active_days", "current_streak", "books",
                              "sessions", "page_turns", "best_day_seconds", "daily_average_seconds"}) {
      valid = valid && doc[field].is<uint32_t>();
    }
    const auto now = std::time(nullptr);
    if (valid && now >= 946684800 && static_cast<uint64_t>(now) <= UINT32_MAX) {
      doc["fetched_at"] = static_cast<uint32_t>(now);
      valid = writeStatsCacheJson(tmp, doc, 640);
    }
    if (!valid) {
      Storage.remove(tmp.c_str());
      LOG_ERR("DSN", "Native overview refresh returned invalid statistics");
      return false;
    }
  }
  if (!replaceStatsCache(target, tmp)) return false;
  LOG_INF("DSN", "Native overview snapshot updated: %s", target.c_str());
  return loginRefreshed;
}
}  // namespace

void DessenAppActivity::unloadGuestForNativeStats() {
  RenderLock lock(*this);
  host.setServices(nullptr);
  services.reset();
  memory.reset();
  pendingInput = {};
  pendingSwipe = dessen::FreeInkHost::SwipeNone;
  if (frameStored) {
    renderer.discardStoredBwBuffer();
    frameStored = false;
  }
  LOG_INF("DSN", "Native handoff: guest freed, free=%u max=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void DessenAppActivity::onEnter() {
  Activity::onEnter();
  if (nativeOperation == 0 && !resumeAfterNetwork && isStandaloneStatsApp(appPath)) {
    // The one-byte page marker is only meaningful for the immediate return
    // from a firmware-owned network operation. A normal launch (including
    // after a user exits during pairing) must always begin on Overall.
    const auto marker = appPath.substr(0, appPath.find_last_of('/')) + "/resume-page";
    if (Storage.exists(marker.c_str())) {
      Storage.remove(marker.c_str());
      LOG_INF("DSN", "Discarded stale resume page on normal app launch");
    }
  }
  if (nativeOperation != 0) {
    if (nativeOperation == 3) {
      const bool cleared = disconnectNativeStats(appPath);
      if (!cleared) {
        // The guest treats this one-shot marker as an error on Server Sync.
        const auto marker = appPath.substr(0, appPath.find_last_of('/')) + "/resume-page";
        HalFile file;
        const uint8_t errorPage = 8;
        if (Storage.openFileForWrite("DSN", marker, file)) file.write(&errorPage, sizeof(errorPage));
      }
      LOG_INF("DSN", "Reset Reading Statistics pairing: success=%d", cleared);
      silentRestartToDessen(appPath);
      return;
    }
    if (nativeOperation == 2) {
      const bool refreshed = refreshNativeStatsSnapshot(appPath);
      LOG_INF("DSN", "Standalone overview refresh completed: success=%d; resuming Dessen", refreshed);
      silentRestartToDessen(appPath);
      return;
    }
    const size_t slash = appPath.find_last_of('/');
    const std::string appDir = slash == std::string::npos ? std::string() : appPath.substr(0, slash);
    const size_t nameStart = appDir.find_last_of('/');
    const std::string appName = nameStart == std::string::npos ? appDir : appDir.substr(nameStart + 1);
    LOG_INF("DSN", "Standalone pairing from %s; guest unloaded, free=%u max=%u", appName.c_str(),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    auto native = makeUniqueNoThrow<PluginCatalogActivity>(renderer, mappedInput, false, appName, true);
    if (!native) {
      LOG_ERR("DSN", "Standalone pairing screen OOM");
      silentRestartToDessen(appPath);
      return;
    }
    startActivityForResult(std::move(native), [this](const ActivityResult&) {
      // The pairing-only catalog has finished (approved or cancelled).
      // Its TLS client is destroyed before this callback and the Dessen VM
      // remains unloaded until after the upcoming silent reboot.
      LOG_INF("DSN", "Standalone pairing screen returned; checking server snapshot");
      const bool refreshed = refreshNativeStatsSnapshot(appPath);
      LOG_INF("DSN", "Post-pairing overview refresh completed: success=%d; resuming Dessen", refreshed);
      silentRestartToDessen(appPath);
    });
    return;
  }
  {
    RenderLock lock;
#ifndef BOARD_HAS_PSRAM
    // X3/C3: reusable SD glyph caches can obstruct the guest's one large
    // internal allocation. Fonts are lazily reconstructed on demand.
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    LOG_INF("DSN", "Guest allocation headroom: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
    if (loadApp()) {
      lastPairingWakeMs = millis();
      requestUpdate();
      return;
    }
  }
  leaveApp();
}

void DessenAppActivity::leaveApp() {
  {
    RenderLock lock;  // render() also stores/restores this frame
    if (frameStored) {
      renderer.discardStoredBwBuffer();  // release the app's last frame before the next screen runs
      frameStored = false;
    }
  }
  if (homeLauncher) {
    activityManager.goHome(HomeMenuItem::NONE, /*cleanInitialRefresh=*/false, /*forceDefaultHome=*/true);
  } else {
    finish();
  }
}

bool DessenAppActivity::handleHomeGesture() {
  // A home-launcher app IS the home screen, so the Home button does nothing
  // (like pressing Home on the built-in Home) instead of dropping to the default
  // home. The escape hatches are a long Back press and a top-left header long-press (see loop()).
  // A normal plugin app still returns false so the Home gesture exits it.
  return homeLauncher;
}

void DessenAppActivity::openKeyboard() {
  auto kbd = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, services->keyboardPrompt());
  if (!kbd) {
    RenderLock lock;
    services->cancelText();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(kbd), [this](const ActivityResult& result) {
    RenderLock lock;
    if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
      services->cancelText();
    } else {
      services->deliverText(std::get<KeyboardResult>(result.data).text.c_str());
    }
    requestUpdate();
  });
}

void DessenAppActivity::loop() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, FORCE_QUIT_HOLD_MS)) {
    LOG_INF("DSN", "Force quit: %s", appPath.c_str());
    leaveApp();
    return;
  }

  fui::InputSnapshot snap = touchSnapshotFrom(mappedInput, /*withLongPress=*/true);
  // Escape hatch for a home launcher (where the Home button is a no-op): a
  // press-and-hold in the top-left header "back" area leaves to the default
  // home. A swipe would clash with the app's own paging, so use a long-press
  // on the spot a header back button lives. touchX/Y are panel coordinates;
  // the safe-area origin is the app's (0,0).
  if (homeLauncher && snap.longPress) {
    const fui::DeviceContext dev = uiTarget.deviceContext();
    const int ox = dev.safeArea.left, oy = dev.safeArea.top;
    if (snap.touchX >= ox && snap.touchX < ox + 88 && snap.touchY >= oy && snap.touchY < oy + 72) {
      leaveApp();
      return;
    }
  }
  using Button = MappedInputManager::Button;
  using SwipeDir = MappedInputManager::SwipeDir;
  snap.confirm = mappedInput.wasReleased(Button::Confirm);
  snap.back = mappedInput.wasReleased(Button::Back);
  // Keep the configured front keys distinct from the side page-turn keys.
  // Older Dessen apps still see the same PREV/NEXT event type; the source bit
  // lets new apps keep side keys for page navigation while using the front
  // keys for widget navigation.
  snap.prev = mappedInput.wasReleased(Button::Left);
  snap.next = mappedInput.wasReleased(Button::Right);
  snap.focusPrev = mappedInput.wasReleased(Button::PageBack);
  snap.focusNext = mappedInput.wasReleased(Button::PageForward);
  // Touch swipes are their own events (distinct from the buttons), so an app can
  // use the page keys to move a selection AND a swipe to scroll. Clear the
  // release so the swipe is never also read as a tap on what it passed over.
  int swipeStartX = -1;
  int swipeStartY = -1;
  const SwipeDir swipe = mappedInput.wasSwipe(&swipeStartX, &swipeStartY);
  dessen::FreeInkHost::Swipe appSwipe = dessen::FreeInkHost::SwipeNone;
  if (swipe != SwipeDir::None) {
    snap.touchReleased = false;  // never also read the swipe as a tap on what it passed over
    // Picco maps a left-edge swipe to Back as well as a horizontal swipe.
    // Reading Statistics owns horizontal page navigation, including edges.
    if (isStandaloneStatsApp(appPath) && (swipe == SwipeDir::Left || swipe == SwipeDir::Right)) {
      snap.back = false;
    }
    // Carry the start point in the existing touch fields. The Dessen adapter
    // converts panel coordinates to the app's safe-area coordinates.
    snap.touchX = swipeStartX;
    snap.touchY = swipeStartY;
    appSwipe = swipe == SwipeDir::Up     ? dessen::FreeInkHost::SwipeUp
               : swipe == SwipeDir::Down ? dessen::FreeInkHost::SwipeDown
               : swipe == SwipeDir::Left ? dessen::FreeInkHost::SwipeLeft
                                         : dessen::FreeInkHost::SwipeRight;
  }

  dessen::State state;
  bool deliver = false;
  {
    RenderLock lock;
    state = host.state();
    if (state == dessen::State::Busy) {
      if (services->wantsKeyboard()) {
        lock.unlock();
        openKeyboard();
        return;
      }
      // The saved e-ink frame is a large second RAM buffer on X3.
      // Release it before Wi-Fi/TLS work; the guest repaints the
      // successful/error result, and only then stores a new frame.
      if (frameStored && services->needsNetworkHeap()) {
        renderer.discardStoredBwBuffer();
        frameStored = false;
        LOG_INF("DSN", "Reclaimed app frame for network: free=%u max_block=%u",
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
      if (services->wantsWifiSelection()) {
        services->wifiSelectionOpened();
        lock.unlock();
        // Reuse the firmware's scanning, credential and saved-network UI.
        // The parked Dessen syscall resumes after the user connects/cancels.
        auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
        if (!picker) {
          services->completeWifiSelection(false);
          requestUpdate();
          return;
        }
        startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
          RenderLock lock(*this);
          services->completeWifiSelection(!result.isCancelled && WiFi.status() == WL_CONNECTED);
          requestUpdate();
        });
        return;
      }
      // Blocking work uses the loop stack. Keep the render task out of poll().
      if (services->serviceAsyncWork()) requestUpdate();
      return;
    }
    // Periodic wake for the Reading Statistics device-code approval flow.
    // A timer event does not impersonate a physical/touch input. Other Dessen
    // apps retain their existing blocking wait_event behavior.
    if (state == dessen::State::Waiting && appPath.find("/reading-statistics-dessen-rust/") != std::string::npos) {
      if (hasEvent(snap) || appSwipe != dessen::FreeInkHost::SwipeNone) {
        lastPairingWakeMs = millis();
      } else if (millis() - lastPairingWakeMs >= 5000) {
        lastPairingWakeMs = millis();
        appSwipe = dessen::FreeInkHost::WakeTick;
        // A timer wake is not a display invalidation. The VM can poll
        // silently and request a new frame only if its visible state changed.
        suppressTimerFrameRestore = true;
      }
    }
    deliver = state == dessen::State::Waiting && (hasEvent(snap) || appSwipe != dessen::FreeInkHost::SwipeNone);
    if (deliver) {
      pendingInput = snap;
      pendingSwipe = appSwipe;
    }
  }

  // A launcher/home app asked to open a firmware screen and end. goTo*/goHome
  // replace this activity, so there is nothing to finish afterwards.
  if (state == dessen::State::Launch) {
    switch (host.launch()) {
      case dessen::Launch::Reader:
        if (!activityManager.goToReader(host.launchPath())) leaveApp();
        break;
      case dessen::Launch::App: {
        // A home launcher (or any app) opening another dessen app as a shortcut.
        // Replace this activity like open_book does: when the opened app exits,
        // the stack empties and goHome() re-runs the launcher.
        std::string target = host.launchPath();
        auto app = Storage.exists(target.c_str())
                       ? makeUniqueNoThrow<DessenAppActivity>(renderer, mappedInput, std::move(target))
                       : nullptr;
        if (app)
          activityManager.replaceActivity(std::move(app));
        else
          leaveApp();  // missing binary / OOM: fall back home
        break;
      }
      case dessen::Launch::NativeStatsPair:
      case dessen::Launch::NativeStatsRefresh:
      case dessen::Launch::NativeStatsUnpair: {
        // Restricted to this first-party Reading Statistics guest. This must
        // be a full unload: pushing a native picker while the VM stays alive
        // leaves its 80KiB allocation unavailable to Wi-Fi/TLS.
        if (homeLauncher || !isStandaloneStatsApp(appPath)) {
          LOG_ERR("DSN", "Stats pairing/refresh rejected: unsupported app path %s", appPath.c_str());
          leaveApp();
          break;
        }
        if (!Storage.exists((appPath.substr(0, appPath.find_last_of('/')) + "/device.json").c_str())) {
          LOG_ERR("DSN", "Stats operation requires bundled device.json");
          leaveApp();
          break;
        }
        // Exit into an app-owned native networking phase. Firmware resets
        // before entering Wi-Fi and again before reloading the 80-KiB guest.
        // No legacy Reading Statistics SD plugin or device.json is consulted.
        const int action = host.launch() == dessen::Launch::NativeStatsUnpair ? 2
                           : host.launch() == dessen::Launch::NativeStatsPair ? 0
                                                                              : 1;
        LOG_INF("DSN", "Stats handoff accepted: app=%s action=%d", appPath.c_str(), action);
        unloadGuestForNativeStats();
        silentRestartToDessenOperation(appPath, action);
        break;
      }
      case dessen::Launch::Settings:
        activityManager.goToSettings();
        break;
      case dessen::Launch::Browser:
        activityManager.goToFileBrowser();
        break;
      case dessen::Launch::Home: {
        // open_home(dest): open the destination directly, the same way the home
        // menu does. goHome(item) only lands on the home menu with the item
        // highlighted, which reads as "back to the default home".
        switch (host.launchHomeDest()) {
          case DSN_HOME_SETTINGS:
            activityManager.goToSettings();
            break;
          case DSN_HOME_LIBRARY:
            if (!activityManager.goToLibrary()) leaveApp();
            break;
          case DSN_HOME_PLUGINS:
            activityManager.goToPlugins();
            break;
          case DSN_HOME_OPDS:
            activityManager.goToBrowser();
            break;
          case DSN_HOME_TRANSFER:
            activityManager.goToFileTransfer();
            break;
          case DSN_HOME_FILES:
          default:
            activityManager.goToFileBrowser();
            break;
        }
        break;
      }
      default:
        leaveApp();
        break;
    }
    return;
  }

  if (state == dessen::State::Exited || state == dessen::State::Faulted) {
    if (state == dessen::State::Faulted) {
      LOG_ERR("DSN", "App fault at 0x%08x (pc 0x%08x): %s", static_cast<unsigned>(host.vm().fault_addr),
              static_cast<unsigned>(host.vm().pc), appPath.c_str());
    }
    leaveApp();
    return;
  }
  if (deliver || state == dessen::State::Running) requestUpdate();
}

void DessenAppActivity::render(RenderLock&&) {
  if (!services || host.state() == dessen::State::Launch) return;
  // Carries the board's bezel insets for the current rotation; the app draws inside them.
  const fui::DeviceContext device = uiTarget.deviceContext();
  bool drew = false;
  dessen::State state = dessen::State::Running;
  for (int i = 0; i < MAX_STEPS_PER_RENDER; i++) {
    state = host.step(uiTarget, device, pendingInput, pendingSwipe, STEP_BUDGET);
    pendingInput = fui::InputSnapshot{};
    pendingSwipe = dessen::FreeInkHost::SwipeNone;
    if (host.takeRefreshRequest()) {
      renderer.displayBuffer();
      // Snapshot the latest clean frame so it can be restored after an overlay.
      // discard first: the slot holds the previous frame and storeBwBuffer()
      // errors on a double store.
      renderer.discardStoredBwBuffer();
      frameStored = renderer.storeBwBuffer();
      drew = true;
    }
    if (state != dessen::State::Running) break;  // still running: loop() asks for another pass
  }
  const bool skipFrameRestore = suppressTimerFrameRestore;
  suppressTimerFrameRestore = false;
  // We were re-rendered with no new frame of our own while the app is parked —
  // i.e. a system overlay (the frontlight panel) popped and ActivityManager asked
  // us to repaint. The parked app can't redraw itself, so restore its last frame
  // to wipe the overlay. Gated on a parked state so long in-VM work (e.g. puzzle
  // generation, which also produces no frame) does not repaint every pass.
  if (!drew && !skipFrameRestore && frameStored && (state == dessen::State::Waiting || state == dessen::State::Busy)) {
    renderer.restoreBwBuffer();  // repaints the saved frame; frees the slot
    renderer.displayBuffer();
    frameStored = renderer.storeBwBuffer();  // re-save it for the next overlay (slot is free)
  }
}

#endif  // CROSSPOINT_DESSEN_APPS
