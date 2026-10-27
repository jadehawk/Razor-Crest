#pragma once
#include <I18n.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingsSection.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  CustomiseStatusBar,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  Bluetooth,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  TextSettings,
  Plugins,

  KeyboardLayouts,
  HomeButton,
  HomeMenuButtons,
  About,
  ExpandLibrary,
  ExpandConnectivity,
  WallpaperCollection,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::span<const StrId> staticEnumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  bool inTextSettings = false;           // Surfaced in the Text Settings screen; hidden from the flat Reader list

  bool webHidden = false;  // Persisted normally, but omitted from the generic Web Settings API

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore)
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
  std::function<std::string()> stringGetter;
  std::function<void(const std::string&)> stringSetter;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  SettingInfo& withWebHidden() {
    webHidden = true;
    return *this;
  }

  std::span<const StrId> enumLabels() const {
    return staticEnumValues.empty() ? std::span<const StrId>(enumValues) : staticEnumValues;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo StaticEnum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::span<const StrId> values,
                                const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.staticEnumValues = values;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, const char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = std::move(getter);
    s.valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = std::move(getter);
    s.stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

// The rows of one SettingsSection; activating a row toggles/cycles it, opens
// its option popup, or launches its sub-screen.
class SettingsActivity final : public UiTabListActivity {
  SettingsSection section;
  const bool tabbed;
  uint8_t expandedSystemGroups = 0;  // Library and Connectivity accordion bits.
  std::vector<SettingInfo> settings;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue), rebuilt only when the setting list
  // changes; rowValues_ holds the live value text, refreshed every
  // buildScreen() by assigning into the existing strings.
  std::vector<std::string> rowValues_;
  std::vector<std::string> rowLabels_;  // Own indented subgroup labels.
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildSettings();
  int selectedSettingIndex() const { return tabbed ? ringPos() - 1 : nav.selected.load(); }
  void switchSection(SettingsSection next);
  static SettingsSection tabSection(int index);
  int tabCount() const override { return 4; }
  int activeTab() const override;
  const char* tabLabel(int index) const override {
    return I18N.get(SETTINGS_SECTION_TITLES[static_cast<size_t>(tabSection(index))]);
  }
  void onTabAction(int index) override { switchSection(tabSection(index)); }
  void stepTab(int direction) override;
  bool handleButtons() override;
  freeink::ui::ListNav& activeNav() override { return tabbed ? UiTabListActivity::activeNav() : nav; }
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  void navigateButtons() override;
  void rebuildRowItems();
  void toggleSelectedSetting();
  void openSleepTimeoutPicker();
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  static std::string settingValueText(const SettingInfo& setting);

  int listCount() const override { return static_cast<int>(settings.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

 public:
  SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SettingsSection section);
  void onEnter() override;
  void render(RenderLock&& lock) override;
};
