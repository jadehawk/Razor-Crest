#include "LocalSendPluginActivity.h"

#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/media/cover-grid.h"
#include "util/BookCacheUtils.h"
#include "util/DeviceHardwareProfile.h"
#include "util/PluginLocations.h"

namespace {
namespace fui = freeink::ui;
constexpr const char* PLUGIN_NAME = "localsend";
constexpr const char* NATIVE_TYPE = "localsend.v1";
constexpr const char* CAPABILITY = "transfer.localsend.v1";
constexpr size_t MAX_DEVICE_MANIFEST_BYTES = 4096;
constexpr size_t MAX_STATE_BYTES = 4096;
constexpr const char* STATE_PATH = "/.crosspoint/localsend/localsend.json";
constexpr const char* STATE_TMP_PATH = "/.crosspoint/localsend/localsend.json.tmp";
constexpr const char* STATE_BACKUP_PATH = "/.crosspoint/localsend/localsend.json.bak";

const char* defaultDeviceId() {
  const char* model = devicehardware::model();
  if (!std::strcmp(model, "x3")) return "X3";
  if (!std::strcmp(model, "x4")) return "X4";
  if (!std::strcmp(model, "x4pro")) return "X4Pro";
  if (!std::strcmp(model, "x4classic")) return "X4Classic";
  if (!std::strcmp(model, "sticky")) return "Sticky";
  if (!std::strcmp(model, "papermono")) return "PaperMono";
  return LocalSendReceiver::validDeviceId(model) ? model : "Reader";
}

bool readManifest(const std::string& dir, JsonDocument& doc) {
  std::string raw;
  if (!Storage.readFileToString("LSEND", dir + "/device.json", MAX_DEVICE_MANIFEST_BYTES, raw) ||
      deserializeJson(doc, raw, DeserializationOption::NestingLimit(6)))
    return false;
  return doc["native"]["type"] == NATIVE_TYPE && doc["native"]["capability"] == CAPABILITY;
}

bool validInstallation(const std::string& dir) {
  if (dir.empty() || !Storage.exists((dir + "/manifest.json").c_str())) return false;
  std::string raw;
  if (!Storage.readFileToString("LSEND", dir + "/manifest.json", 1024, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::NestingLimit(4))) return false;
  return doc["name"] == PLUGIN_NAME;
}

void copyName(char (&dest)[129], const char* name) { std::snprintf(dest, sizeof(dest), "%s", name); }

// All file operations share the receive-path validation. A history entry
// without its original directory must not be guessed or opened/deleted.
std::string savedPath(const char* filename, const char* destination) {
  if (!localsend::safeReceiveName(filename) || !localsend::safeDestinationFolder(destination)) return {};
  std::string path = destination;
  if (path != "/") path += '/';
  path += filename;
  return path;
}
}  // namespace

bool LocalSendPluginActivity::isAvailable() {
  const auto dir = PluginLocations::findPluginDir(PLUGIN_NAME);
  if (!validInstallation(dir)) return false;
  JsonDocument doc;
  if (!readManifest(dir, doc)) return false;
  const JsonArrayConst folders = doc["native"]["folders"].as<JsonArrayConst>();
  if (folders.isNull() || folders.size() == 0 || folders.size() > MAX_FOLDERS) return false;
  const char* paths[MAX_FOLDERS]{};
  int count = 0;
  for (JsonVariantConst entry : folders) {
    const char* path = entry["path"].as<const char*>();
    const char* label = entry["title"].as<const char*>();
    if (!localsend::safeDestinationFolder(path) || !label || !*label || std::strlen(label) > 48) return false;
    for (int i = 0; i < count; ++i)
      if (!std::strcmp(paths[i], path)) return false;
    paths[count++] = path;
  }
  return true;
}

LocalSendPluginActivity::LocalSendPluginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiTabListActivity("LocalSendPlugin", renderer, mappedInput), coverCache(renderer) {}

bool LocalSendPluginActivity::loadDefinition() {
  pluginDir = PluginLocations::findPluginDir(PLUGIN_NAME);
  if (!validInstallation(pluginDir)) return false;
  JsonDocument doc;
  if (!readManifest(pluginDir, doc)) return false;
  const char* label = doc["title"].as<const char*>();
  if (label && *label && std::strlen(label) < sizeof(title)) std::snprintf(title, sizeof(title), "%s", label);
  const JsonObjectConst ui = doc["native"]["ui"].as<JsonObjectConst>();
  auto readLabel = [&ui](const char* key, char (&target)[65]) {
    const JsonString value = ui[key].as<JsonString>();
    if (!value || value.size() == 0 || value.size() >= sizeof(target) || value.size() != std::strlen(value.c_str()))
      return;
    for (size_t i = 0; i < value.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
      if (c < 32 || c == 127) return;
    }
    std::memcpy(target, value.c_str(), value.size() + 1);
  };
  readLabel("folder", labels.folder);
  readLabel("listening", labels.listening);
  readLabel("unavailable", labels.unavailable);
  readLabel("recent", labels.recent);
  readLabel("empty", labels.empty);
  readLabel("receive_tab", labels.receiveTab);
  readLabel("history_tab", labels.historyTab);
  readLabel("details", labels.details);
  readLabel("saved_in", labels.savedIn);
  readLabel("layout", labels.layout);
  readLabel("device_id", labels.deviceId);
  readLabel("grid_columns", labels.gridColumns);
  readLabel("grid_rows", labels.gridRows);
  readLabel("list_thumbnails", labels.listThumbnails);
  readLabel("clear_history", labels.clearHistory);
  const JsonArrayConst choices = doc["native"]["folders"].as<JsonArrayConst>();
  if (choices.isNull() || choices.size() == 0 || choices.size() > MAX_FOLDERS) return false;
  for (JsonVariantConst choice : choices) {
    const char* path = choice["path"].as<const char*>();
    const char* label = choice["title"].as<const char*>();
    if (!localsend::safeDestinationFolder(path) || !label || !*label || std::strlen(label) >= sizeof(folders[0].label))
      return false;
    for (int i = 0; i < folderCount; ++i)
      if (!std::strcmp(folders[i].path, path)) return false;
    std::snprintf(folders[folderCount].path, sizeof(folders[0].path), "%s", path);
    std::snprintf(folders[folderCount].label, sizeof(folders[0].label), "%s", label);
    ++folderCount;
  }
  // Use the default destination from the current plugin's choices.
  for (int i = 0; i < folderCount; ++i)
    if (!std::strcmp(folders[i].path, "/Books/LocalSend")) selectedFolder = i;
  return folderCount > 0;
}

void LocalSendPluginActivity::loadPreferences() {
  JsonDocument doc;
  std::string raw;
  const auto readState = [&](const char* path) {
    raw.clear();
    doc.clear();
    return Storage.exists(path) && Storage.readFileToString("LSEND", path, MAX_STATE_BYTES, raw) &&
           !deserializeJson(doc, raw, DeserializationOption::NestingLimit(4)) && doc.is<JsonObject>();
  };
  // A backup is used only to recover an interrupted write of this same file.
  // Plugin-local settings.json/recent.json are deliberately not migrated.
  if (!readState(STATE_PATH)) {
    if (!readState(STATE_BACKUP_PATH)) return;
    // The backup was verified before replacing a missing or malformed primary.
    if ((!Storage.exists(STATE_PATH) || Storage.remove(STATE_PATH)) && !Storage.rename(STATE_BACKUP_PATH, STATE_PATH))
      LOG_ERR("LSEND", "Could not restore Local Send state from backup");
  }

  gridView = doc["layout"] == "grid";
  listThumbnails = doc["list_thumbnails"] | false;
  gridColumns = static_cast<uint8_t>(std::clamp(doc["grid_columns"] | 2, 1, 4));
  gridRows = static_cast<uint8_t>(std::clamp(doc["grid_rows"] | 3, 1, 3));
  const char* id = doc["device_id"].as<const char*>();
  if (LocalSendReceiver::validDeviceId(id)) std::snprintf(deviceId, sizeof(deviceId), "%s", id);
  const char* storedFingerprint = doc["fingerprint"].as<const char*>();
  if (LocalSendReceiver::validFingerprint(storedFingerprint))
    std::memcpy(fingerprint, storedFingerprint, sizeof(fingerprint));
  const JsonString destination = doc["destination"].as<JsonString>();
  if (destination && destination.size() == std::strlen(destination.c_str())) {
    for (int i = 0; i < folderCount; ++i)
      if (!std::strcmp(destination.c_str(), folders[i].path)) selectedFolder = i;
  }
}

void LocalSendPluginActivity::refreshCovers() {
  // Only inspect thumbnails for the visible SD-backed history window.
  // Existing EPUB cache entries are reused; large received books are not decoded.
  for (int i = 0; i < MAX_RECENT_WINDOW; ++i) {
    recentCoverPaths[i].clear();
    if (i >= recentWindowCount) continue;
    const auto& entry = recentEntries[i];
    const std::string source = savedPath(entry.name, entry.folder);
    if (source.empty() || !FsHelpers::hasEpubExtension(source)) continue;
    auto epub = makeUniqueNoThrow<Epub>(source, "/.crosspoint");
    if (!epub) continue;
    const std::string cachePath = epub->getCachePath();
    auto dir = Storage.open(cachePath.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    for (auto item = dir.openNextFile(); item; item = dir.openNextFile()) {
      char name[64]{};
      item.getName(name, sizeof(name));
      const size_t n = std::strlen(name);
      if (!item.isDirectory() && n > 18 && !std::strncmp(name, "thumb_bw_fill_", 14) &&
          !std::strcmp(name + n - 4, ".bmp")) {
        recentCoverPaths[i] = cachePath + "/" + name;
        item.close();
        break;
      }
      item.close();
    }
    dir.close();
  }
  coverCache.invalidate();
}

void LocalSendPluginActivity::loadRecentWindow(int first) {
  if (first < 0 || first >= recentCount) return;
  if (recentWindowFirst == first && recentWindowCount > 0) return;
  recentWindowCount = history.readWindow(first, recentEntries, MAX_RECENT_WINDOW);
  recentWindowFirst = first;
  refreshCovers();
}

const localsend::History::Entry* LocalSendPluginActivity::windowEntry(int index) {
  if (index < 0 || index >= recentCount) return nullptr;
  if (index < recentWindowFirst || index >= recentWindowFirst + recentWindowCount) loadRecentWindow(index);
  const int offset = index - recentWindowFirst;
  return offset >= 0 && offset < recentWindowCount ? &recentEntries[offset] : nullptr;
}

bool LocalSendPluginActivity::saveState() {
  if (!LocalSendReceiver::validFingerprint(fingerprint)) {
    LOG_ERR("LSEND", "Cannot save Local Send state without a valid fingerprint");
    return false;
  }
  JsonDocument doc;
  doc["fingerprint"] = fingerprint;
  doc["destination"] = folders[selectedFolder].path;
  if (deviceId[0]) doc["device_id"] = deviceId;
  doc["layout"] = gridView ? "grid" : "list";
  doc["list_thumbnails"] = listThumbnails;
  doc["grid_columns"] = gridColumns;
  doc["grid_rows"] = gridRows;
  std::string raw;
  serializeJson(doc, raw);
  if (doc.overflowed() || raw.size() > MAX_STATE_BYTES || !Storage.ensureDirectoryExists("/.crosspoint") ||
      !Storage.ensureDirectoryExists("/.crosspoint/localsend")) {
    LOG_ERR("LSEND", "Cannot prepare Local Send state for save");
    return false;
  }
  HalFile file;
  if (!Storage.openFileForWrite("LSEND", STATE_TMP_PATH, file)) {
    LOG_ERR("LSEND", "Cannot open temporary Local Send state");
    return false;
  }
  const bool written = file.write(raw.data(), raw.size()) == raw.size();
  file.flush();
  const bool closed = file.close();
  if (!written || !closed) {
    Storage.remove(STATE_TMP_PATH);
    LOG_ERR("LSEND", "Cannot write complete Local Send state");
    return false;
  }
  const bool hadOld = Storage.exists(STATE_PATH);
  if (hadOld) {
    if ((Storage.exists(STATE_BACKUP_PATH) && !Storage.remove(STATE_BACKUP_PATH)) ||
        !Storage.rename(STATE_PATH, STATE_BACKUP_PATH)) {
      Storage.remove(STATE_TMP_PATH);
      LOG_ERR("LSEND", "Cannot back up previous Local Send state");
      return false;
    }
  }
  if (!Storage.rename(STATE_TMP_PATH, STATE_PATH)) {
    if (hadOld) Storage.rename(STATE_BACKUP_PATH, STATE_PATH);
    Storage.remove(STATE_TMP_PATH);
    LOG_ERR("LSEND", "Cannot publish Local Send state");
    return false;
  }
  if (Storage.exists(STATE_BACKUP_PATH)) Storage.remove(STATE_BACKUP_PATH);
  return true;
}

bool LocalSendPluginActivity::cancelled(void* context) {
  auto& self = *static_cast<LocalSendPluginActivity*>(context);
  self.mappedInput.update(true);
  if (gpio.rawInputActive()) {
    delay(6);
    self.mappedInput.update(true);
  }
  self.leaveRequested = self.leaveRequested || self.mappedInput.isPressed(MappedInputManager::Button::Back) ||
                        self.mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        self.mappedInput.wasHomeGesture();
  return self.leaveRequested;
}

bool LocalSendPluginActivity::startReceiver() {
  if (!receiver) return false;
  receiver->stop();
  listening = receiver->setAdvertisedId(deviceId[0] ? deviceId : defaultDeviceId()) &&
              receiver->setDestination(folders[selectedFolder].path) && receiver->begin(&cancelled, this);
  seenSequence = 0;
  return listening;
}

void LocalSendPluginActivity::onEnter() {
  UiTabListActivity::onEnter();
  if (!loadDefinition()) {
    LOG_ERR("LSEND", "Plugin definition unavailable or invalid");
    onBackButton();
    return;
  }
  loadPreferences();
  recentCount = history.count();
  if (recentCount) loadRecentWindow(0);
  std::snprintf(ipAddress, sizeof(ipAddress), "%s:53317", WiFi.localIP().toString().c_str());
  receiver = makeUniqueNoThrow<LocalSendReceiver>();
  if (!receiver) {
    LOG_ERR("LSEND", "OOM: receiver for plugin screen");
  } else {
    const bool newIdentity = !fingerprint[0];
    if (newIdentity) std::snprintf(fingerprint, sizeof(fingerprint), "%s", receiver->deviceFingerprint());
    if (!receiver->setFingerprint(fingerprint) || ((newIdentity || !Storage.exists(STATE_PATH)) && !saveState())) {
      LOG_ERR("LSEND", "Cannot persist stable Local Send identity; receiver disabled");
      receiver.reset();
    }
  }
  // Display the Receive tab before advertising or accepting any incoming
  // transfer. ActivityManager enters this screen without a render lock.
  requestUpdateAndWait();
  if (receiver) startReceiver();
  requestUpdate();
}

void LocalSendPluginActivity::onExit() {
  popup.dismiss();
  if (receiver) {
    receiver->stop();
    receiver.reset();
  }
  listening = false;
  Activity::onExit();
}

void LocalSendPluginActivity::recordCompleted() {
  if (!receiver || receiver->completedSequence() == seenSequence) return;
  seenSequence = receiver->completedSequence();
  const char* name = receiver->completedName();
  if (!localsend::safeReceiveName(name)) return;

  const char* destination = receiver->destinationFolder();
  {
    RenderLock lock(*this);
    if (sessionCount == MAX_SESSION_NAMES) {
      for (int i = 1; i < sessionCount; ++i) {
        copyName(sessionNames[i - 1], sessionNames[i]);
        std::memcpy(sessionPaths[i - 1], sessionPaths[i], sizeof(sessionPaths[0]));
      }
      --sessionCount;
    }
    copyName(sessionNames[sessionCount], name);
    std::snprintf(sessionPaths[sessionCount], sizeof(sessionPaths[0]), "%s", destination);
    ++sessionCount;
    if (!history.append(destination, name)) {
      LOG_ERR("LSEND", "History could not record %s", name);
    } else {
      recentCount = history.count();
      recentWindowFirst = -1;
      recentWindowCount = 0;
      activeNav().followOnBuild = true;
    }
  }
  const std::string path = savedPath(name, destination);
  if (!path.empty() && library::isLibraryBookFile(path)) library::markLibraryIndexDirty();
  requestUpdate();
}

void LocalSendPluginActivity::showConsent() {
  if (!receiver || !receiver->pendingOffer()) return;
  const auto* offer = receiver->pendingOffer();
  char message[320], display[160];
  const auto peer = IPAddress(offer->peer).toString();
  uint64_t totalBytes = 0;
  for (uint8_t i = 0; i < offer->fileCount; ++i) totalBytes += offer->files[i].expected;
  if (offer->fileCount == 1)
    std::snprintf(display, sizeof(display), "%s", offer->files[0].fileName);
  else
    std::snprintf(display, sizeof(display), "%u files (first: %.100s)", unsigned(offer->fileCount),
                  offer->files[0].fileName);
  std::snprintf(message, sizeof(message), tr(STR_LOCALSEND_RECEIVE_PROMPT), display,
                unsigned((totalBytes + 1023u) / 1024u), peer.c_str());
  const char* actions[] = {tr(STR_LOCALSEND_DECLINE), tr(STR_LOCALSEND_ACCEPT)};
  confirmHeld = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  consentPopup = true;
  popup.showMessage(tr(STR_LOCALSEND_RECEIVE_TITLE), message, actions, 2, 0, [this](int index) {
    if (index == 1) {
      // Paint a non-interactive status while the potentially slow SD FAT scan runs.
      checkingStorage.store(true);
      requestUpdateAndWait();
      if (receiver) receiver->decide(true);
      checkingStorage.store(false);
    } else if (receiver) {
      receiver->decide(false);
    }
  });
  requestUpdate();
}

void LocalSendPluginActivity::showDeviceIdKeyboard() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, "Device ID", std::string(deviceId),
                                                           32, InputType::Text);
  if (!keyboard) {
    showFileError("Unable to open keyboard");
    return;
  }
  // The keyboard takes over the main loop, so pause discovery/reception
  // rather than leaving a listener whose connections cannot be serviced.
  if (receiver) receiver->stop();
  listening = false;
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& id = std::get<KeyboardResult>(result.data).text;
      if (id.empty() || LocalSendReceiver::validDeviceId(id.c_str())) {
        std::snprintf(deviceId, sizeof(deviceId), "%s", id.c_str());
        saveState();
      } else {
        showFileError("Use 1-32 letters, digits, spaces, - or _");
      }
    }
    if (receiver) startReceiver();
    requestUpdate();
  });
}

void LocalSendPluginActivity::showFolderChoice() {
  const char* actions[MAX_FOLDERS]{};
  for (int i = 0; i < folderCount; ++i) actions[i] = folders[i].label;
  consentPopup = false;
  popup.show(labels.folder[0] ? labels.folder : tr(STR_LOCALSEND_FOLDER), actions, folderCount, selectedFolder,
             [this](int choice) { requestedFolder = choice; });
  requestUpdate();
}

void LocalSendPluginActivity::showFileError(const char* message) {
  const char* choices[] = {tr(STR_LOCALSEND_CLOSE)};
  consentPopup = false;
  popup.showMessage(tr(STR_LOCALSEND_FILE_DETAILS), message, choices, 1, 0, [](int) {});
  requestUpdate();
}

void LocalSendPluginActivity::removeHistoryEntry(int index) {
  if (index < 0 || index >= recentCount) return;
  RenderLock lock(*this);
  if (!history.eraseAt(index)) {
    LOG_ERR("LSEND", "Could not remove history entry %d", index);
    return;
  }
  recentCount = history.count();
  recentWindowFirst = -1;
  recentWindowCount = 0;
  activeNav().requestSelection(std::min(index + 1, recentCount));
  requestUpdate();
}

void LocalSendPluginActivity::forgetDeletedFile(const std::string& path) {
  RenderLock lock(*this);
  for (int i = 0; i < sessionCount;) {
    if (savedPath(sessionNames[i], sessionPaths[i]) != path) {
      ++i;
      continue;
    }
    for (int j = i + 1; j < sessionCount; ++j) {
      std::memcpy(sessionNames[j - 1], sessionNames[j], sizeof(sessionNames[0]));
      std::memcpy(sessionPaths[j - 1], sessionPaths[j], sizeof(sessionPaths[0]));
    }
    --sessionCount;
    sessionNames[sessionCount][0] = 0;
    sessionPaths[sessionCount][0] = 0;
  }
  const int removed = history.erasePath(path.c_str());
  if (removed < 0) LOG_ERR("LSEND", "Could not update history after deletion");
  if (removed > 0) {
    recentCount = history.count();
    recentWindowFirst = -1;
    recentWindowCount = 0;
    activeNav().requestSelection(std::min(activeNav().selected.load(), recentCount));
  }
  requestUpdate();
}

void LocalSendPluginActivity::promptDeleteFile(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) {
    showFileError(tr(STR_LOCALSEND_FILE_MISSING));
    return;
  }
  const char* choices[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  consentPopup = false;
  popup.showMessage(tr(STR_DELETE_ITEM_CONFIRM), path.c_str(), choices, 2, 0, [this, path](int choice) {
    if (choice != 1) return;
    const auto slash = path.find_last_of('/');
    const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const std::string destination = slash == 0 ? "/" : path.substr(0, slash);
    if (savedPath(name, destination.c_str()) != path) {
      showFileError(tr(STR_LOCALSEND_FILE_MISSING));
      return;
    }
    auto file = Storage.open(path.c_str());
    if (!file || file.isDirectory()) {
      if (file) file.close();
      showFileError(tr(STR_LOCALSEND_FILE_MISSING));
      return;
    }
    file.close();
    // The library's shared helper also clears EPUB caches and clipping references.
    const bool book = FsHelpers::hasEpubExtension(path);
    const bool removed = book ? removeBookFile(path) : Storage.remove(path.c_str());
    if (Storage.exists(path.c_str())) {
      showFileError(tr(STR_LOCALSEND_DELETE_FAILED));
      return;
    }
    if (!removed) LOG_ERR("LSEND", "File deleted, but cleanup failed: %s", path.c_str());
    if (book) RECENT_BOOKS.removeByPath(path);
    if (library::isLibraryBookFile(path)) library::markLibraryIndexDirty();
    forgetDeletedFile(path);
  });
  popup.setMessageMaxLines(10);
  requestUpdate();
}

void LocalSendPluginActivity::showFileDetails(const char* filename, const char* destination, bool history, int index) {
  const std::string path = savedPath(filename, destination);
  // Keep the destination on its own line so the filename cannot consume
  // its available width/lines in the compact 4-action History dialog.
  char message[192];
  std::snprintf(message, sizeof(message), "%s:\n%s", labels.savedIn[0] ? labels.savedIn : tr(STR_LOCALSEND_SAVED_IN),
                destination && *destination ? destination : "/");
  const char* receiveActions[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_LOCALSEND_CLOSE)};
  const char* historyActions[] = {tr(STR_OPEN), tr(STR_LOCALSEND_REMOVE_HISTORY), tr(STR_DELETE),
                                  tr(STR_LOCALSEND_CLOSE)};
  consentPopup = false;
  popup.showMessage(filename, message, history ? historyActions : receiveActions, history ? 4 : 3, 0,
                    [this, history, index, path](int choice) {
                      if ((history && choice == 3) || (!history && choice == 2)) return;
                      if (history && choice == 1) {
                        removeHistoryEntry(index);
                        return;
                      }
                      if (path.empty()) {
                        showFileError(tr(STR_LOCALSEND_FILE_MISSING));
                        return;
                      }
                      if (choice == 0) {
                        auto file = Storage.open(path.c_str());
                        if (!file || file.isDirectory()) {
                          if (file) file.close();
                          showFileError(tr(STR_LOCALSEND_FILE_MISSING));
                          return;
                        }
                        file.close();
                        if (FsHelpers::hasImageExtension(path)) {
                          // Keep the LocalSend screen on the activity stack.
                          // The image viewer normally returns to File Browser.
                          auto viewer = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, path, false,
                                                                             std::string{}, std::string{}, true);
                          if (!viewer) {
                            showFileError("Unable to open image viewer");
                            return;
                          }
                          if (receiver) receiver->stop();
                          listening = false;
                          startActivityForResult(std::move(viewer), [this](const ActivityResult&) {
                            // Resume after image decoding has released its buffers.
                            if (receiver && WiFi.status() == WL_CONNECTED) startReceiver();
                            requestUpdate();
                          });
                          return;
                        }
                        // EPUBs replace this screen and reclaim Wi-Fi for reading.
                        // Keep the existing full reader transition for EPUB files.
                        if (receiver) receiver->stop();
                        listening = false;
                        // Release the radio before allocating the reader
                        // and keep the File Transfer parent from
                        // performing its normal exit reboot during navigation.
                        WiFi.disconnect(false);
                        WiFi.mode(WIFI_OFF);
                        LOG_INF("LSEND", "Opening received file: %s", path.c_str());
                        onSelectBook(path);
                      } else {
                        promptDeleteFile(path);
                      }
                    });
  popup.setMessageMaxLines(10);
  requestUpdate();
}

const char* LocalSendPluginActivity::tabLabel(int index) const {
  if (index == static_cast<int>(Tab::Settings)) return tr(STR_SETTINGS_TITLE);
  if (index == static_cast<int>(Tab::History))
    return labels.historyTab[0] ? labels.historyTab : tr(STR_LOCALSEND_HISTORY_TAB);
  return labels.receiveTab[0] ? labels.receiveTab : tr(STR_LOCALSEND_RECEIVE_TAB);
}

void LocalSendPluginActivity::onTabAction(int index) {
  if (popup.isActive() || index < 0 || index >= tabCount()) return;
  if (tab_ != static_cast<Tab>(index)) {
    tab_ = static_cast<Tab>(index);
    auto& n = activeNav();
    n.selected = 0;
    n.followOnBuild = true;
    requestUpdate();
  }
  app.clearTapFlash();
}

void LocalSendPluginActivity::switchTab(int direction) {
  if (popup.isActive()) return;
  const bool onTabBar = ringPos() == 0;
  tab_ = static_cast<Tab>((activeTab() + tabCount() + direction % tabCount()) % tabCount());
  auto& n = activeNav();
  if (onTabBar) n.selected = 0;
  n.followOnBuild = true;
  requestUpdate();
}

bool LocalSendPluginActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0)
      switchTab(1);
    else
      activateIndex(ringPos() - 1);
    return true;
  }
  return false;
}

bool LocalSendPluginActivity::handleCustomInput() {
  if (!popup.isActive()) {
    if (tab_ == Tab::History && recentCount > 0 && (gridView || listThumbnails)) {
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        // Grid/cards are not a FreeInk list: follow a page of items via the
        // existing per-tab ListNav instead of losing requestScroll().
        const int pageItems = gridView ? gridColumns * gridRows : 3;
        const int current = std::max(0, ringPos() - 1);
        const int next = std::clamp(current + (swipe == MappedInputManager::SwipeDir::Up ? pageItems : -pageItems), 0,
                                    recentCount - 1);
        routeTouch(mappedInput, true);  // Release card hit before changing the viewport.
        activeNav().requestSelection(next + 1);
        requestUpdate();
        return true;
      }
    }
    return false;
  }
  if (consentPopup && receiver) {
    receiver->pollConsent();
    if (!receiver->pendingOffer()) {
      popup.dismiss();
      consentPopup = false;
      requestUpdate();
      return true;
    }
  }
  const bool blocked = confirmHeld;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) confirmHeld = false;
  popup.handleInput(mappedInput, [this] { requestUpdate(); }, blocked);
  if (!popup.isActive() && consentPopup) {
    if (receiver && receiver->pendingOffer()) receiver->decide(false);
    consentPopup = false;
  }
  return true;
}

void LocalSendPluginActivity::onBackButton() {
  leaveRequested = true;
  finish();
}

void LocalSendPluginActivity::activateIndex(int index) {
  if (popup.isActive()) return;
  app.clearTapFlash();
  if (tab_ == Tab::Receive && index >= 0 && index < sessionCount) {
    const int entry = sessionCount - index - 1;
    showFileDetails(sessionNames[entry], sessionPaths[entry], false, entry);
  } else if (tab_ == Tab::History && index >= 0 && index < recentCount) {
    localsend::History::Entry entry{};
    if (history.read(index, entry)) showFileDetails(entry.name, entry.folder, true, index);
  } else if (tab_ == Tab::Settings && index >= 0 && index < 7) {
    consentPopup = false;
    if (index == 0) {
      if (folderCount > 0) showFolderChoice();
    } else if (index == 1) {
      showDeviceIdKeyboard();
    } else if (index == 2) {
      const char* choices[] = {"List", "Grid"};
      popup.show(labels.layout[0] ? labels.layout : "History Layout", choices, 2, gridView ? 1 : 0, [this](int choice) {
        gridView = choice == 1;
        saveState();
        requestUpdate();
      });
    } else if (index == 3) {
      listThumbnails = !listThumbnails;
      saveState();
    } else if (index == 4) {
      const char* choices[] = {"1", "2", "3", "4"};
      popup.show("Grid columns", choices, 4, gridColumns - 1, [this](int choice) {
        gridColumns = choice + 1;
        saveState();
        requestUpdate();
      });
    } else if (index == 5) {
      const char* choices[] = {"1", "2", "3"};
      popup.show("Grid rows", choices, 3, gridRows - 1, [this](int choice) {
        gridRows = choice + 1;
        saveState();
        requestUpdate();
      });
    } else if (recentCount > 0) {
      char message[112];
      std::snprintf(message, sizeof(message), "Clear all %d History entries?\nReceived files stay on the SD card.",
                    recentCount);
      const char* actions[] = {"Cancel", "Clear History"};
      popup.showMessage(labels.clearHistory[0] ? labels.clearHistory : "Clear History", message, actions, 2, 0,
                        [this](int choice) {
                          if (choice != 1) return;
                          if (!history.clear()) {
                            showFileError("Failed to clear History");
                            return;
                          }
                          recentCount = 0;
                          recentWindowFirst = -1;
                          recentWindowCount = 0;
                          for (auto& path : recentCoverPaths) path.clear();
                          coverCache.invalidate();
                          auto& historyNav = tabNavs[static_cast<size_t>(Tab::History)];
                          historyNav.selected = 0;
                          historyNav.top = 0;
                          historyNav.followOnBuild = true;
                          requestUpdate();
                        });
      popup.setMessageMaxLines(5);
    }
    // On non-touch devices the popup must be drawn before the next key release.
    requestUpdate();
  }
}

void LocalSendPluginActivity::loop() {
  if (leaveRequested) {
    finish();
    return;
  }
  UiListActivity::loop();
  if (leaveRequested) return;
  if (requestedFolder >= 0) {
    const int next = requestedFolder;
    requestedFolder = -1;
    if (next < folderCount && next != selectedFolder && !popup.isActive() && (!receiver || !receiver->pendingOffer())) {
      const int previous = selectedFolder;
      selectedFolder = next;
      if (!startReceiver()) {
        selectedFolder = previous;
        startReceiver();
      } else {
        saveState();
      }
      requestUpdate();
    }
  }
  if (popup.isActive() || !listening || !receiver) return;
  if (WiFi.status() != WL_CONNECTED) {
    receiver->stop();
    listening = false;
    requestUpdate();
    return;
  }
  receiver->poll();
  if (leaveRequested) {
    finish();
    return;
  }
  recordCompleted();
  if (receiver->pendingOffer()) showConsent();
}

int LocalSendPluginActivity::listCount() const {
  if (tab_ == Tab::Settings) return 7;
  return tab_ == Tab::Receive ? std::max(1, sessionCount) : std::max(1, recentCount);
}

void LocalSendPluginActivity::buildHistoryGrid(UiScreen& screen) {
  const auto& theme = screen.theme();
  const fui::Rect area = screen.body();
  const int16_t gap = std::max<int16_t>(4, theme.spaceSm / 2);
  const int16_t rowHeight = std::max<int16_t>(48, (area.height - gap * (gridRows - 1)) / gridRows);
  fui::TextStyle titleText = theme.smallText;
  titleText.maxLines = 3;
  const int16_t labelHeight = screen.target().lineHeight(titleText.font) * 3;
  const int16_t cellWidth = (area.width - gap * (gridColumns - 1)) / gridColumns;
  const int16_t coverHeight = std::max<int16_t>(24, std::min<int16_t>(120, rowHeight - labelHeight - gap - 5));
  const int16_t coverWidth = std::max<int16_t>(18, std::min<int16_t>(cellWidth - 8, coverHeight * 2 / 3));
  const uint16_t visible = std::max<uint16_t>(1, fui::coverGridVisibleCells(area, gridColumns, rowHeight, gap));
  const int selected = ringPos() - 1;
  auto& nav = activeNav();
  uint16_t top =
      static_cast<uint16_t>(std::min<int>(std::max(0, nav.top), recentCount > visible ? recentCount - visible : 0));
  if (nav.followOnBuild.exchange(false) && selected >= 0 && selected < recentCount)
    top = fui::coverGridTopIndexFor(selected, recentCount, gridColumns, visible);
  top = std::min<uint16_t>(top, recentCount > visible ? recentCount - visible : 0);
  nav.top = top;
  loadRecentWindow(top);
  coverCache.prepare();
  fui::CoverGridProps grid;
  grid.count = recentCount;
  grid.topIndex = top;
  grid.selectedIndex = selected;
  grid.action = ACTION_ROW;
  grid.inputMask = fui::InputTouch;
  grid.columns = gridColumns;
  grid.coverSize = fui::Size{coverWidth, coverHeight};
  grid.rowHeight = rowHeight;
  grid.gap = gap;
  grid.rowGap = gap;
  grid.labelHeight = labelHeight;
  grid.titleText = titleText;
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](uint16_t index, void* user) {
    auto* self = static_cast<LocalSendPluginActivity*>(user);
    const auto* entry = self->windowEntry(index);
    return fui::coverGridItem(entry ? entry->name : "", index);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::CoverGridItem&, uint16_t index, void* user) {
    auto* self = static_cast<LocalSendPluginActivity*>(user);
    return self->coverCache.paint(rect, index % HomeCoverCache::MAX_COVERS,
                                  self->recentCoverPaths[index - self->recentWindowFirst]);
  };
  fui::coverGrid(screen.frame(), area, grid);
}

void LocalSendPluginActivity::buildHistoryCards(UiScreen& screen) {
  const auto& theme = screen.theme();
  const fui::Rect area = screen.body();
  const int16_t gap = std::max<int16_t>(3, theme.listRowGap);
  const int16_t height = std::max<int16_t>(theme.rowHeight, 3 * screen.target().lineHeight(theme.bodyText.font) +
                                                                screen.target().lineHeight(theme.smallText.font) + 44);
  const uint16_t visible = std::max<uint16_t>(1, fui::listVisibleRows(area, height, gap));
  const int selected = ringPos() - 1;
  auto& nav = activeNav();
  uint16_t top =
      static_cast<uint16_t>(std::min<int>(std::max(0, nav.top), recentCount > visible ? recentCount - visible : 0));
  if (nav.followOnBuild.exchange(false) && selected >= 0 && selected < recentCount)
    top = fui::listTopIndexFor(selected, top, visible, recentCount);
  nav.top = top;
  loadRecentWindow(top);
  coverCache.prepare();
  int16_t y = area.y;
  for (uint16_t i = top; i < recentCount && i < top + visible; ++i) {
    fui::BookCardProps card;
    const auto* entry = windowEntry(i);
    if (!entry) break;
    card.title = entry->name;
    card.meta = entry->folder;
    card.value = i;
    card.action = ACTION_ROW;
    card.state = selected == static_cast<int>(i) ? fui::StateSelected : fui::StateNormal;
    card.styles = theme.listRow;
    card.titleText = theme.bodyText;
    card.titleText.maxLines = 3;
    card.metaText = theme.smallText;
    card.progressMax = 0;
    card.coverSize = fui::Size{48, 72};
    card.coverPainterUserData = this;
    card.coverPainter = [](fui::DrawTarget&, fui::Rect rect, const fui::BookCardProps& item, void* user) {
      auto* self = static_cast<LocalSendPluginActivity*>(user);
      const int index = item.value - self->recentWindowFirst;
      return self->coverCache.paint(rect, item.value % HomeCoverCache::MAX_COVERS, self->recentCoverPaths[index]);
    };
    fui::bookCard(screen.frame(), fui::Rect{area.x, y, area.width, height}, card);
    y += height + gap;
  }
}

void LocalSendPluginActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  buildTabBar(screen);
  if (tab_ == Tab::History && recentCount > 0) {
    if (gridView) {
      buildHistoryGrid(screen);
      return;
    }
    if (listThumbnails) {
      buildHistoryCards(screen);
      return;
    }
    // FreeInkUI's pull-based row source resolves only visible SD records.
    fui::ListProps props;
    props.count = static_cast<uint16_t>(recentCount);
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.labelText = screen.theme().bodyText;
    props.labelText.maxLines = 3;
    props.rowProviderCtx = this;
    props.rowProvider = [](void* context, uint16_t index, fui::ListItem& item) {
      auto* self = static_cast<LocalSendPluginActivity*>(context);
      item = {};
      const auto* entry = self->windowEntry(index);
      if (entry) {
        item.label = entry->name;
        item.actionValue = index;
        if (index == 0) item.sectionHeading = self->labels.recent[0] ? self->labels.recent : tr(STR_LOCALSEND_RECENT);
      } else {
        item.enabled = false;
        item.label = tr(STR_LOCALSEND_FILE_MISSING);
      }
    };
    syncTabListViewport(screen, props);
    loadRecentWindow(props.topIndex);
    screen.list(props);
    return;
  }

  int index = 0;
  if (tab_ == Tab::Receive) {
    const auto primary = screen.theme().bodyText;
    const auto secondary = screen.theme().smallText;
    const int16_t titleHeight = screen.target().lineHeight(primary.font);
    const int16_t ipHeight = screen.target().lineHeight(secondary.font);
    // Compact status followed directly by the session list. No empty text rows.
    const auto band = screen.takeTop(static_cast<int16_t>(titleHeight + (listening ? ipHeight : 0) + 1), 0);
    const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
    const int16_t textX = static_cast<int16_t>(band.x + side + 13);
    const int16_t textWidth = static_cast<int16_t>(std::max(0, int(band.width) - side * 2 - 13));
    const int16_t statusY = band.y;
    screen.target().fill(
        fui::Rect{static_cast<int16_t>(band.x + side), static_cast<int16_t>(statusY + titleHeight / 2 - 3), 6, 6},
        fui::Paint::solid(listening ? fui::Color::Black : fui::Color::LightGray));
    auto statusStyle = primary;
    statusStyle.bold = true;
    screen.target().text(fui::Rect{textX, statusY, textWidth, titleHeight},
                         listening ? (labels.listening[0] ? labels.listening : tr(STR_LOCALSEND_LISTENING))
                                   : (labels.unavailable[0] ? labels.unavailable : tr(STR_LOCALSEND_NOT_LISTENING)),
                         statusStyle);
    if (listening)
      screen.target().text(fui::Rect{textX, static_cast<int16_t>(statusY + titleHeight), textWidth, ipHeight},
                           ipAddress, secondary);
    screen.target().fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - 1), band.width, 1},
                         fui::Paint::solid(fui::Color::Black));

    if (sessionCount == 0) {
      rows[index] = {};
      rows[index].label = labels.empty[0] ? labels.empty : tr(STR_LOCALSEND_NONE);
      rows[index++].enabled = false;
    } else {
      for (int i = sessionCount - 1; i >= 0; --i) {
        rows[index] = {};
        rows[index].label = sessionNames[i];
        rows[index].actionValue = static_cast<int16_t>(index);
        ++index;
      }
    }
  } else if (tab_ == Tab::History) {
    rows[index] = {};
    rows[index].label = labels.empty[0] ? labels.empty : tr(STR_LOCALSEND_NONE);
    rows[index].sectionHeading = labels.recent[0] ? labels.recent : tr(STR_LOCALSEND_RECENT);
    rows[index++].enabled = false;
  } else {
    const char* captions[] = {labels.folder[0] ? labels.folder : tr(STR_LOCALSEND_FOLDER),
                              labels.deviceId[0] ? labels.deviceId : "Device ID",
                              labels.layout[0] ? labels.layout : "History Layout",
                              labels.listThumbnails[0] ? labels.listThumbnails : "List layout thumbnails",
                              labels.gridColumns[0] ? labels.gridColumns : "Grid columns",
                              labels.gridRows[0] ? labels.gridRows : "Grid rows",
                              labels.clearHistory[0] ? labels.clearHistory : "Clear History"};
    const char* values[] = {folders[selectedFolder].path,
                            deviceId[0] ? deviceId : defaultDeviceId(),
                            gridView ? "Grid" : "List",
                            listThumbnails ? "On" : "Off",
                            "",
                            "",
                            ""};
    char columns[4], rowCount[4];
    std::snprintf(columns, sizeof(columns), "%u", unsigned(gridColumns));
    std::snprintf(rowCount, sizeof(rowCount), "%u", unsigned(gridRows));
    values[4] = columns;
    values[5] = rowCount;
    char historyCount[24];
    std::snprintf(historyCount, sizeof(historyCount), "%d entries", recentCount);
    values[6] = historyCount;
    for (int i = 0; i < 7; ++i) {
      rows[index] = {};
      rows[index].label = captions[i];
      rows[index].subtitle = values[i];
      rows[index].enabled = i != 6 || recentCount > 0;
      rows[index++].actionValue = i;
    }
  }

  fui::ListProps props;
  props.items = rows;
  props.count = static_cast<uint16_t>(index);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 3;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void LocalSendPluginActivity::renderOverlay() {
  if (checkingStorage.load()) {
    // A standalone status overlay has no buttons and needs only one e-ink flush.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    const fui::DeviceContext device = target.deviceContext();
    fui::InteractionBuffer<1> interactions;
    const fui::InputSnapshot noInput{};
    fui::Frame<1> frame(target, device, noInput, interactions);
    fui::PopupProps props;
    props.message = "Checking SD card space...\nTransfer starts after verification.\nThis may take a while.";
    props.text = theme.bodyText;
    props.text.align = fui::TextAlign::Center;
    props.text.maxLines = 5;
    props.styles = theme.popup;
    const fui::Rect bounds = device.screen();
    const int16_t width =
        static_cast<int16_t>(std::max<int>(1, std::min<int>(bounds.width * 3 / 4, bounds.width - 16)));
    const int16_t textWidth = static_cast<int16_t>(std::max<int>(1, width - props.padding.left - props.padding.right));
    const fui::Size textSize = fui::measureWrappedText(target, props.message, props.text, textWidth);
    const int16_t height = static_cast<int16_t>(std::max<int>(
        1, std::min<int>(bounds.height - 16, textSize.height + props.padding.top + props.padding.bottom)));
    fui::popup(frame, fui::centeredRect(bounds, fui::Size{width, height}), props);
  } else if (popup.isActive()) {
    popup.render(renderer);
  }
}

#endif
