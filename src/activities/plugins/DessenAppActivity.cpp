#if CROSSPOINT_DESSEN_APPS
#include "DessenAppActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>
#include <sdk/dessen_abi.h>  // DSN_HOME_*

#include <cstdio>
#include <utility>
#ifndef BOARD_HAS_PSRAM
#include <esp_attr.h>
#include <esp_system.h>
#include <wolfssl/wolfcrypt/aes.h>

#include <algorithm>
#include <cstring>
#endif

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
void cleanupDessenTemporaryFiles() {
  Storage.remove("/.crosspoint/dsn-write.tmp");
  std::string path, normalized;
  if (Storage.readFileToString("DSN", "/.crosspoint/dsn-download.tmp", 512, path) &&
      DessenPermissions::canonical(path, normalized) && normalized.ends_with(".part"))
    Storage.remove(path.c_str());
  Storage.remove("/.crosspoint/dsn-download.tmp");
}
}  // namespace

extern void silentRestartToDessen(const std::string& appPath);
extern void silentRestart();

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

bool hasEvent(const fui::InputSnapshot& s) {
  return s.confirm || s.back || s.prev || s.next || s.focusPrev || s.focusNext || (s.touchReleased && s.touchX >= 0);
}
}  // namespace

DessenAppActivity::DessenAppActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appPath,
                                     bool homeLauncher)
    : Activity("DessenApp", renderer, mappedInput),
      appPath(std::move(appPath)),
      homeLauncher(homeLauncher),
      uiTarget(makeUiTarget(renderer)) {
  // Fixed-size app fonts are independent of theme font scaling.
#ifndef OMIT_FONTS
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_12, NOTOSANS_12_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_14, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_16, NOTOSANS_16_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_18, NOTOSANS_18_FONT_ID);
#else
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_12, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_14, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_16, NOTOSANS_14_FONT_ID);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_NOTOSANS_18, NOTOSANS_14_FONT_ID);
#endif
}

#ifndef BOARD_HAS_PSRAM

namespace {
constexpr uint32_t SNAPSHOT_BLOCK = 256;
constexpr uint32_t SNAPSHOT_TAG = 16;
constexpr uint32_t RESUME_MAGIC = 0xD5C03A91;
constexpr uint32_t NETWORK_MAGIC = 0xD5C03A92;
constexpr const char* REQUEST_SNAPSHOT = "/.crosspoint/dsn-request.snapshot";
constexpr const char* VM_SNAPSHOT = "/.crosspoint/dsn-vm.snapshot";
constexpr const char* RESPONSE_SNAPSHOT = "/.crosspoint/dsn-response.snapshot";

struct ResumeTicket {
  uint32_t magic;
  char appPath[192];
  uint8_t key[32], manifestDigest[32];
  dessen::FreeInkHost::Continuation continuation;
  uint32_t responseOffset, responseSize, requestSize;
  int result;
  bool homeLauncher;
};
alignas(ResumeTicket) RTC_NOINIT_ATTR uint8_t rebootTicketStorage[sizeof(ResumeTicket)];
ResumeTicket& rebootTicket = *reinterpret_cast<ResumeTicket*>(rebootTicketStorage);
ResumeTicket activeTicket{};
bool haveResume = false;
bool haveNetwork = false;

void clearTicket(ResumeTicket& ticket) {
  volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&ticket);
  for (size_t i = 0; i < sizeof(ticket); ++i) bytes[i] = 0;
}

bool transferSnapshot(HalFile& file, uint8_t* memory, uint32_t size, const uint8_t* key, bool save,
                      uint8_t domain = 0) {
  auto aes = makeUniqueNoThrow<Aes>();
  auto scratch = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_BLOCK + SNAPSHOT_TAG);
  if (!aes || !scratch || wc_AesInit(aes.get(), nullptr, INVALID_DEVID) != 0) {
    LOG_ERR("DSN", "Could not allocate snapshot encryption state");
    return false;
  }
  bool ok = wc_AesGcmSetKey(aes.get(), key, 32) == 0;
  uint8_t* encrypted = scratch.get();
  uint8_t* tag = encrypted + SNAPSHOT_BLOCK;
  for (uint32_t offset = 0; ok && offset < size; offset += SNAPSHOT_BLOCK) {
    const uint32_t count = std::min(SNAPSHOT_BLOCK, size - offset);
    uint8_t nonce[12]{};
    nonce[8] = domain;
    for (unsigned i = 0; i < 4; ++i) nonce[i] = static_cast<uint8_t>(offset >> (8 * i));
    if (save) {
      ok = wc_AesGcmEncrypt(aes.get(), encrypted, memory + offset, count, nonce, sizeof(nonce), tag, SNAPSHOT_TAG,
                            nullptr, 0) == 0 &&
           file.write(encrypted, count) == count && file.write(tag, SNAPSHOT_TAG) == SNAPSHOT_TAG;
    } else {
      ok = file.read(encrypted, count) == static_cast<int>(count) &&
           file.read(tag, SNAPSHOT_TAG) == static_cast<int>(SNAPSHOT_TAG) &&
           wc_AesGcmDecrypt(aes.get(), memory + offset, encrypted, count, nonce, sizeof(nonce), tag, SNAPSHOT_TAG,
                            nullptr, 0) == 0;
    }
  }
  wc_AesFree(aes.get());
  return ok;
}

bool readSnapshot(const char* path, uint8_t* memory, uint32_t size, const uint8_t* key, uint8_t domain) {
  HalFile file;
  const size_t expected = size + ((size + SNAPSHOT_BLOCK - 1) / SNAPSHOT_BLOCK) * SNAPSHOT_TAG;
  return Storage.openFileForRead("DSN", path, file) && file.fileSize() == expected &&
         transferSnapshot(file, memory, size, key, false, domain);
}
}  // namespace

bool DessenAppActivity::takeResumePath(std::string& path, bool& launchHome) {
  haveResume = false;
  haveNetwork = false;
  cleanupDessenTemporaryFiles();
  if (esp_reset_reason() != ESP_RST_SW || (rebootTicket.magic != RESUME_MAGIC && rebootTicket.magic != NETWORK_MAGIC) ||
      !memchr(rebootTicket.appPath, 0, sizeof(rebootTicket.appPath))) {
    clearTicket(rebootTicket);
    Storage.remove(VM_SNAPSHOT);
    Storage.remove(RESPONSE_SNAPSHOT);
    Storage.remove(REQUEST_SNAPSHOT);
    return false;
  }
  activeTicket = rebootTicket;
  clearTicket(rebootTicket);  // consume before opening any guest or SD state
  std::string normalized;
  if (!DessenPermissions::canonical(activeTicket.appPath, normalized) ||
      normalized.size() >= sizeof(activeTicket.appPath) || !normalized.ends_with("/app.bin") ||
      activeTicket.continuation.memorySize > MAX_APP_MEMORY) {
    clearTicket(activeTicket);
    Storage.remove(VM_SNAPSHOT);
    Storage.remove(RESPONSE_SNAPSHOT);
    Storage.remove(REQUEST_SNAPSHOT);
    return false;
  }
  path = activeTicket.appPath;
  launchHome = activeTicket.homeLauncher;
  haveNetwork = activeTicket.magic == NETWORK_MAGIC;
  haveResume = !haveNetwork;
  return true;
}

void DessenAppActivity::abandonResume() {
  haveResume = false;
  haveNetwork = false;
  clearTicket(activeTicket);
  clearTicket(rebootTicket);
  Storage.remove(VM_SNAPSHOT);
  Storage.remove(RESPONSE_SNAPSHOT);
  Storage.remove(REQUEST_SNAPSHOT);
}

void DessenAppActivity::discardSnapshot() {
  if (!snapshotPath.empty()) {
    Storage.remove(VM_SNAPSHOT);
    Storage.remove(RESPONSE_SNAPSHOT);
    Storage.remove(REQUEST_SNAPSHOT);
    snapshotPath.clear();
  }
  volatile uint8_t* key = snapshotKey;
  for (size_t i = 0; i < sizeof(snapshotKey); ++i) key[i] = 0;
}

bool DessenAppActivity::suspendNetwork() {
  if (suspended || !memory || !services->permissions().allowsHandoff() ||
      appPath.size() >= sizeof(activeTicket.appPath) || !host.saveContinuation(activeTicket.continuation))
    return false;
  const uint32_t size = host.vm().mem_size;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) return false;
  snapshotPath = VM_SNAPSHOT;
  esp_fill_random(snapshotKey, sizeof(snapshotKey));
  HalFile file;
  bool saved = Storage.openFileForWrite("DSN", snapshotPath, file) &&
               transferSnapshot(file, memory.get(), size, snapshotKey, true);
  if (file) saved = file.close() && saved;
  if (!saved || !services->detachNetworkBuffers(memory.get(), size)) {
    discardSnapshot();
    return false;
  }
  std::unique_ptr<uint8_t[]> request;
  uint32_t requestSize = 0;
  HalFile requestFile;
  bool requestSaved = services->exportNetworkRequest(request, requestSize) &&
                      Storage.openFileForWrite("DSN", REQUEST_SNAPSHOT, requestFile) &&
                      transferSnapshot(requestFile, request.get(), requestSize, snapshotKey, true, 2);
  if (requestFile) requestSaved = requestFile.close() && requestSaved;
  request.reset();
  if (!requestSaved || !host.detachMemory()) {
    services->restoreNetworkBuffers(memory.get());
    discardSnapshot();
    return false;
  }
  memory.reset();
  suspended = true;
  activeTicket.magic = 0;
  memcpy(activeTicket.appPath, appPath.c_str(), appPath.size() + 1);
  memcpy(activeTicket.key, snapshotKey, sizeof(snapshotKey));
  memcpy(activeTicket.manifestDigest, services->permissionDigest(), 32);
  activeTicket.requestSize = requestSize;
  activeTicket.homeLauncher = homeLauncher;
  activeTicket.responseSize = 0;
  activeTicket.responseOffset = 0;
  activeTicket.result = DSN_ERR_IO;
  host.setServices(nullptr);
  services.reset();
  LOG_INF("DSN", "Network handoff: clean restart before transport; guest=%u free=%u max_block=%u",
          static_cast<unsigned>(size), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  rebootTicket = activeTicket;
  rebootTicket.magic = NETWORK_MAGIC;
  silentRestartToDessen(appPath);
  // Sleep may supersede the restart. Never continue with a detached guest.
  abandonResume();
  discardSnapshot();
  return true;
}

bool DessenAppActivity::resumeNetwork() {
  if (!suspended) return false;
  services->releaseNetworkRadio();
  activeTicket.magic = 0;
  activeTicket.responseOffset = services->networkResponseOffset();
  activeTicket.responseSize = services->networkResponseSize();
  activeTicket.result = services->networkResult();
  activeTicket.homeLauncher = homeLauncher;
  HalFile response;
  if (activeTicket.responseSize) {
    const bool saved =
        Storage.openFileForWrite("DSN", RESPONSE_SNAPSHOT, response) &&
        transferSnapshot(response, services->networkResponse(), activeTicket.responseSize, activeTicket.key, true, 1);
    if (!saved || !response.close()) {
      clearTicket(activeTicket);
      discardSnapshot();
      return false;
    }
  } else {
    Storage.remove(RESPONSE_SNAPSHOT);
    Storage.remove(REQUEST_SNAPSHOT);
  }
  LOG_INF("DSN", "Network result ready: %d", activeTicket.result);
  rebootTicket = activeTicket;
  rebootTicket.magic = RESUME_MAGIC;  // arm only after all output is durable
  silentRestartToDessen(appPath);
  clearTicket(rebootTicket);  // sleep may supersede a requested reboot
  clearTicket(activeTicket);
  discardSnapshot();
  return false;
}

bool DessenAppActivity::restoreContinuation() {
  if (!haveResume || appPath != activeTicket.appPath ||
      memcmp(services->permissionDigest(), activeTicket.manifestDigest, 32) != 0 ||
      host.vm().mem_size != activeTicket.continuation.memorySize)
    return false;
  haveResume = false;
  const uint32_t size = host.vm().mem_size;
  const uint32_t offset = activeTicket.responseOffset, count = activeTicket.responseSize;
  bool ok =
      offset <= size && count <= size - offset && readSnapshot(VM_SNAPSHOT, memory.get(), size, activeTicket.key, 0);
  if (ok && count) ok = readSnapshot(RESPONSE_SNAPSHOT, memory.get() + offset, count, activeTicket.key, 1);
  if (ok) ok = host.resumeContinuation(activeTicket.continuation, activeTicket.result);
  clearTicket(activeTicket);
  Storage.remove(VM_SNAPSHOT);
  Storage.remove(RESPONSE_SNAPSHOT);
  Storage.remove(REQUEST_SNAPSHOT);
  return ok;
}
#else
void DessenAppActivity::abandonResume() {}
bool DessenAppActivity::takeResumePath(std::string&, bool&) {
  cleanupDessenTemporaryFiles();
  return false;
}
#endif

DessenAppActivity::~DessenAppActivity() {
  stopNetworkWork();
#ifndef BOARD_HAS_PSRAM
  if (services) services->cancelSuspendedNetwork();
  discardSnapshot();
  clearTicket(activeTicket);
  haveResume = false;
  haveNetwork = false;
  Storage.remove(VM_SNAPSHOT);
  Storage.remove(RESPONSE_SNAPSHOT);
  Storage.remove(REQUEST_SNAPSHOT);
#endif
}

void DessenAppActivity::onExit() {
  stopNetworkWork();
  if (services) services->cancelReadingStats();
#ifndef BOARD_HAS_PSRAM
  if (suspended) {
    discardSnapshot();
    clearTicket(activeTicket);
    clearTicket(rebootTicket);
    silentRestart();
  }
#endif
  Activity::onExit();
}

void DessenAppActivity::stopNetworkWork() {
  if (!networkRunning.load(std::memory_order_acquire)) return;
  services->cancelNetwork();
  while (!networkDone.load(std::memory_order_acquire)) delay(10);
  networkRunning.store(false, std::memory_order_release);
  networkTask = nullptr;
}

void DessenAppActivity::runNetworkWork(void* context) {
  auto* activity = static_cast<DessenAppActivity*>(context);
  while (!activity->services->serviceAsyncWork()) vTaskDelay(pdMS_TO_TICKS(10));
  activity->networkDone.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool DessenAppActivity::startNetworkWork() {
  networkDone.store(false, std::memory_order_relaxed);
  networkRunning.store(true, std::memory_order_release);
  // Networking owns this bounded stack only while the guest is parked.
  if (xTaskCreate(runNetworkWork, "DessenNetwork", 8192, this, 1, &networkTask) != pdPASS) {
    LOG_ERR("DSN", "Could not allocate 8192-byte network worker");
    networkRunning.store(false, std::memory_order_release);
    networkTask = nullptr;
    services->failNetwork();
    return false;
  }
  return true;
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
  if (!services || !services->permissions().ready()) return false;
  host.setServices(services.get());
  return true;
}

void DessenAppActivity::onEnter() {
  Activity::onEnter();
  const size_t slash = appPath.find_last_of('/');
  if (slash == appPath.npos) {
    leaveApp();
    return;
  }
  const std::string directory = appPath.substr(0, slash);
  const size_t name = directory.find_last_of('/');
  services = makeUniqueNoThrow<CrosspointServices>(directory.substr(name + 1), directory);
  if (!services || !services->loadPermissions()) {
    leaveApp();
    return;
  }
  services->setHintContext(&renderer, &mappedInput);
#ifndef BOARD_HAS_PSRAM
  if (haveResume || haveNetwork) {
    if (appPath != activeTicket.appPath || memcmp(services->permissionDigest(), activeTicket.manifestDigest, 32) != 0) {
      abandonResume();
      leaveApp();
      return;
    }
    services->permissions().authorize();
    if (haveNetwork) {
      haveNetwork = false;
      suspended = true;
      snapshotPath = VM_SNAPSHOT;
      if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
      const uint32_t size = activeTicket.requestSize;
      auto request =
          size && size <= CrosspointServices::MAX_NETWORK_REQUEST ? makeUniqueNoThrow<uint8_t[]>(size) : nullptr;
      const bool ready = request && readSnapshot(REQUEST_SNAPSHOT, request.get(), size, activeTicket.key, 2) &&
                         services->importNetworkRequest(request.get(), size, activeTicket.continuation.memorySize);
      request.reset();
      Storage.remove(REQUEST_SNAPSHOT);
      if (!ready) services->failNetwork();
      LOG_INF("DSN", "Clean network boot: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      if (!ready || !startNetworkWork()) {
        if (!resumeNetwork()) leaveApp();
      }
      return;
    }
    startAuthorizedApp();
    return;
  }
#endif
  services->permissions().authorize();
  startAuthorizedApp();
}

void DessenAppActivity::startAuthorizedApp() {
  {
    RenderLock lock;
#ifndef BOARD_HAS_PSRAM
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
#endif
    if (loadApp()) {
#ifndef BOARD_HAS_PSRAM
      if (haveResume && !restoreContinuation()) {
        lock.unlock();
        leaveApp();
        return;
      }
#endif
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

bool DessenAppActivity::storeOverlayFrame() {
#ifndef BOARD_HAS_PSRAM
  // Keep working memory available for native screens and radio services.
  constexpr size_t reserve = 40000;
  if (ESP.getFreeHeap() < renderer.getBufferSize() + reserve || ESP.getMaxAllocHeap() < 8000) {
    return false;
  }
#endif
  return renderer.storeBwBuffer();
}

void DessenAppActivity::openKeyboard() {
  {
    RenderLock lock;
    // Button-only boards do not need a permanent backup for touch overlays.
    // A keyboard is an explicit native screen, so take a best-effort snapshot here.
    if (!frameStored) frameStored = storeOverlayFrame();
  }
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
  if (!services || !services->permissions().ready()) return;
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, FORCE_QUIT_HOLD_MS)) {
    LOG_INF("DSN", "Force quit: %s", appPath.c_str());
    if (networkRunning.load(std::memory_order_acquire)) {
      leaveAfterNetwork = true;
      services->cancelNetwork();
      return;
    }
    leaveApp();
    return;
  }

  if (networkRunning.load(std::memory_order_acquire)) {
    if (!networkDone.load(std::memory_order_acquire)) return;
    networkRunning.store(false, std::memory_order_release);
    networkTask = nullptr;
    if (leaveAfterNetwork) {
      leaveApp();
      return;
    }
    RenderLock lock;
#ifndef BOARD_HAS_PSRAM
    if (suspended && !resumeNetwork()) {
      lock.unlock();
      leaveApp();
      return;
    }
#endif
    requestUpdate();
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
  snap.prev = mappedInput.wasReleased(Button::Left);
  snap.next = mappedInput.wasReleased(Button::Right);
  snap.focusPrev = mappedInput.wasReleased(Button::PageBack);
  snap.focusNext = mappedInput.wasReleased(Button::PageForward);
  // Touch swipes are their own events (distinct from the buttons), so an app can
  // use the page keys to move a selection AND a swipe to scroll. Clear the
  // release so the swipe is never also read as a tap on what it passed over.
  int swipeStartX = -1, swipeStartY = -1;
  const SwipeDir swipe = mappedInput.wasSwipe(&swipeStartX, &swipeStartY);
  dessen::FreeInkHost::Swipe appSwipe = dessen::FreeInkHost::SwipeNone;
  if (swipe != SwipeDir::None) {
    snap.touchReleased = false;  // never also read the swipe as a tap on what it passed over
    snap.touchX = swipeStartX;
    snap.touchY = swipeStartY;
    snap.back = false;
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
      if (frameStored && services->needsNetworkHeap()) {
        renderer.discardStoredBwBuffer();
        frameStored = false;
      }
      if (services->needsNetworkHeap()) {
#ifndef BOARD_HAS_PSRAM
        if (!suspendNetwork()) {
          services->failNetwork();
          requestUpdate();
        } else {
          lock.unlock();
          leaveApp();  // only reached if the reboot was superseded
        }
#else
        if (!startNetworkWork()) requestUpdate();
#endif
      } else if (services->readingStatsBusy()) {
        if (services->serviceAsyncWork()) {
          historyProgressAt = 0;
          requestUpdate();
        } else {
          // Leave fast queries alone; display progress after roughly a second
          // and then at most every five seconds to avoid excessive e-ink refreshes.
          const auto now = millis();
          if (!historyProgressAt) historyProgressAt = now - 4000;
          if (now - historyProgressAt >= 5000) {
            historyProgressAt = now;
            requestUpdate();
          }
          lock.unlock();
          // skipLoopDelay keeps the CPU awake. A real RTOS yield permits idle
          // and other tasks to run, even during a long continuous scan.
          vTaskDelay(1);
        }
      } else if (services->serviceAsyncWork()) {
        requestUpdate();
      }
      return;
    }
    if (state == dessen::State::Waiting && !hasEvent(snap) && appSwipe == dessen::FreeInkHost::SwipeNone &&
        host.timerInterval() && millis() - lastWakeMs >= host.timerInterval()) {
      lastWakeMs = millis();
      appSwipe = dessen::FreeInkHost::WakeTick;
      suppressTimerFrameRestore = true;
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
        if (!services->permissions().allowsBook(host.launchPath()) || !activityManager.goToReader(host.launchPath()))
          leaveApp();
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
    } else {
      LOG_INF("DSN", "App exited: code=%d sp=%u path=%s", host.exitCode(), static_cast<unsigned>(host.vm().x[2]),
              appPath.c_str());
    }
    leaveApp();
    return;
  }
  if (deliver || state == dessen::State::Running) requestUpdate();
}

void DessenAppActivity::render(RenderLock&&) {
  if (networkRunning.load(std::memory_order_acquire)) return;
  if (!services || !memory || !services->permissions().ready() || host.state() == dessen::State::Launch) return;
  if (host.state() == dessen::State::Busy && services->readingStatsBusy()) {
    // Native progress belongs to the optional host service, not to a particular
    // plugin. The guest stays parked and will redraw after the query completes.
    if (frameStored) {
      renderer.discardStoredBwBuffer();
      frameStored = false;
    }
    const auto percent = services->readingStatsProgress();
    const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
    const int barW = w * 3 / 4, barX = (w - barW) / 2, barY = h / 2;
    char progress[16];
    snprintf(progress, sizeof(progress), "%u%%", percent);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, barY - 44, tr(STR_LOADING), true, EpdFontFamily::BOLD);
    renderer.drawRect(barX, barY, barW, 14, true);
    const int filled = (barW - 4) * static_cast<int>(percent) / 100;
    if (filled > 0) renderer.fillRect(barX + 2, barY + 2, filled, 10, true);
    renderer.drawCenteredText(UI_10_FONT_ID, barY + 24, progress);
    renderer.drawCenteredText(SMALL_FONT_ID, barY + 56, "Hold Back to cancel");
    renderer.displayBuffer();
    return;
  }
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
      // Only touch devices can open the global control-center overlay.
      // discard first: the slot holds the previous frame and storeBwBuffer()
      // errors on a double store.
      renderer.discardStoredBwBuffer();
      frameStored = mappedInput.hasTouch() && storeOverlayFrame();
      drew = true;
    }
    if (state != dessen::State::Running) break;  // still running: loop() asks for another pass
  }
  // We were re-rendered with no new frame of our own while the app is parked —
  // i.e. a system overlay (the frontlight panel) popped and ActivityManager asked
  // us to repaint. The parked app can't redraw itself, so restore its last frame
  // to wipe the overlay. Gated on a parked state so long in-VM work (e.g. puzzle
  // generation, which also produces no frame) does not repaint every pass.
  const bool skipFrameRestore = suppressTimerFrameRestore;
  suppressTimerFrameRestore = false;
  if (!drew && !skipFrameRestore && frameStored && (state == dessen::State::Waiting || state == dessen::State::Busy)) {
    renderer.restoreBwBuffer();  // repaints the saved frame; frees the slot
    renderer.displayBuffer();
    frameStored = mappedInput.hasTouch() && storeOverlayFrame();  // only touch overlays need a standing copy
  }
}

#endif  // CROSSPOINT_DESSEN_APPS
