#pragma once

#include <BoardConfig.h>

#include "activities/UiListActivity.h"
#if FREEINK_CAP_USB_MSC && (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PICCO)
#include "components/OptionPopup.h"
#endif

enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_DRIVE, LOCAL_SEND, OPDS_BROWSER };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Calibre Wireless" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "OPDS Browser" - Browse OPDS catalogs, or add a server when none is configured.
 *   Always accessible here; a customized Home screen may also offer a shortcut.
 * - "USB Drive" - Expose the SD card over USB (boards with USB mass storage)
 * - "Local Send" - Receive files when the optional LocalSend plugin is available.
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * The header stays on GUI.drawHeader for the battery indicator.
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

#if FREEINK_CAP_USB_MSC
  static constexpr int BASE_MENU_ITEM_COUNT = 4;
#else
  static constexpr int BASE_MENU_ITEM_COUNT = 3;
#endif
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
  static constexpr int MAX_ITEM_COUNT = BASE_MENU_ITEM_COUNT + 2;
#else
  static constexpr int MAX_ITEM_COUNT = BASE_MENU_ITEM_COUNT + 1;
#endif

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;
#if FREEINK_CAP_USB_MSC && (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PICCO)
  bool handleCustomInput() override;
  void renderOverlay() override;
  OptionPopup usbConfirmation_;
  bool usbConfirmed_ = false;
#endif

  // Built once in the constructor. Explicit row-to-mode mapping keeps
  // LocalSend and OPDS independent of their display positions.
  freeink::ui::ListItem rowItems_[MAX_ITEM_COUNT]{};
  NetworkMode rowModes_[MAX_ITEM_COUNT]{};
  int rowCount_ = 0;
};
