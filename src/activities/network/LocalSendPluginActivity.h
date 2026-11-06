#pragma once
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND

#include <cstdint>
#include <memory>
#include <string>

#include "activities/UiTabListActivity.h"
#include "components/HomeCoverCache.h"
#include "components/OptionPopup.h"
#include "network/LocalSendHistory.h"
#include "network/LocalSendReceiver.h"

class LocalSendPluginActivity final : public UiTabListActivity {
 public:
  static bool isAvailable();
  LocalSendPluginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return listening && !popup.isActive(); }
  bool preventAutoSleep() override { return listening; }

 private:
  enum class Tab : uint8_t { Receive, History, Settings };
  static constexpr int MAX_FOLDERS = 6;
  static constexpr int MAX_SESSION_NAMES = 16;
  static constexpr int MAX_RECENT_WINDOW = 16;  // Visible rows only; catalog remains on SD.
  static constexpr int MAX_ROWS = MAX_SESSION_NAMES;

  struct Folder {
    char label[49]{};
    char path[96]{};
  };
  Folder folders[MAX_FOLDERS]{};
  int folderCount = 0;
  int selectedFolder = 0;
  int requestedFolder = -1;
  char title[65] = "Local Send";
  struct ScreenLabels {
    char folder[65]{};
    char listening[65]{};
    char unavailable[65]{};
    char recent[65]{};
    char empty[65]{};
    char receiveTab[65]{};
    char historyTab[65]{};
    char details[65]{};
    char savedIn[65]{};
    char layout[65]{};
    char deviceId[65]{};
    char gridColumns[65]{};
    char gridRows[65]{};
    char listThumbnails[65]{};
    char clearHistory[65]{};
  } labels;
  char ipAddress[24]{};
  char deviceId[33]{};
  char sessionNames[MAX_SESSION_NAMES][129]{};
  char sessionPaths[MAX_SESSION_NAMES][96]{};
  localsend::History history;
  localsend::History::Entry recentEntries[MAX_RECENT_WINDOW]{};
  int recentWindowFirst = -1;
  int recentWindowCount = 0;
  int sessionCount = 0;
  int recentCount = 0;
  uint32_t seenSequence = 0;
  Tab tab_ = Tab::Receive;
  std::string pluginDir;
  std::unique_ptr<LocalSendReceiver> receiver;
  HomeCoverCache coverCache;
  std::string recentCoverPaths[MAX_RECENT_WINDOW];
  bool gridView = false;
  bool listThumbnails = false;
  uint8_t gridColumns = 2, gridRows = 3;
  OptionPopup popup;
  bool consentPopup = false;
  bool confirmHeld = false;
  bool listening = false;
  bool leaveRequested = false;
  freeink::ui::ListItem rows[MAX_ROWS]{};

  static bool cancelled(void* context);
  bool loadDefinition();
  void loadPreferences();
  void saveState();
  void refreshCovers();
  void loadRecentWindow(int first);
  const localsend::History::Entry* windowEntry(int index);
  void buildHistoryGrid(UiScreen& screen);
  void buildHistoryCards(UiScreen& screen);
  bool startReceiver();
  void recordCompleted();
  void showConsent();
  void showFolderChoice();
  void showDeviceIdKeyboard();
  void showFileDetails(const char* filename, const char* destination, bool history, int index);
  void showFileError(const char* message);
  void promptDeleteFile(const std::string& filePath);
  void removeHistoryEntry(int index);
  void forgetDeletedFile(const std::string& filePath);
  void switchTab(int direction);

  int listCount() const override;
  int tabCount() const override { return 3; }
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override { switchTab(direction); }
  bool handleButtons() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title; }
  void onBackButton() override;
  bool handleCustomInput() override;
  void renderOverlay() override;
};

#endif
