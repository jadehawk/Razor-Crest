#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
#include "LocalSendPluginActivity.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,   StrId::STR_CALIBRE_WIRELESS, StrId::STR_CREATE_HOTSPOT,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    StrId::STR_LOCALSEND_MENU,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,           StrId::STR_CALIBRE_DESC, StrId::STR_HOTSPOT_DESC,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    StrId::STR_LOCALSEND_MENU_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot,
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    UIIcon::Wifi,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
  if (!LocalSendPluginActivity::isAvailable()) visibleItems_ = BASE_MENU_ITEM_COUNT;
#endif
  for (int i = 0; i < visibleItems_; i++) {
    // Display Local Send directly after Join Network, without changing the
    // underlying NetworkMode values or the existing menus when unavailable.
    int sourceIndex = i;
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    if (visibleItems_ == MENU_ITEM_COUNT && i > 0) sourceIndex = i == 1 ? BASE_MENU_ITEM_COUNT : i - 1;
#endif
    fui::ListItem item;
    item.label = I18N.get(menuItems[sourceIndex]);
    item.subtitle = I18N.get(menuDescs[sourceIndex]);
    item.icon = listIconFor(menuIcons[sourceIndex], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return visibleItems_; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= visibleItems_) return;
  // Clear transient tap flash; keep the USB row selected behind the popup.
  // Selection focus is retained by nav.selected until confirmation.
  app.clearTapFlash();
  nav.selected = index;
  int sourceIndex = index;
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
  if (visibleItems_ == MENU_ITEM_COUNT && index > 0) sourceIndex = index == 1 ? BASE_MENU_ITEM_COUNT : index - 1;
#endif
  const auto mode = sourceIndex == BASE_MENU_ITEM_COUNT && visibleItems_ > BASE_MENU_ITEM_COUNT
                        ? NetworkMode::LOCAL_SEND
                        : static_cast<NetworkMode>(sourceIndex);

#if FREEINK_CAP_USB_MSC && (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PICCO)
  if (mode == NetworkMode::USB_DRIVE && UITheme::getInstance().activeExternalScene()) {
    // Keep the picker on screen with the USB row selected. Draw the
    // confirmation in the same frame so there is no premature mode switch.
    usbConfirmed_ = false;
    const char* choices[] = {I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM)};
    usbConfirmation_.showMessage(I18N.get(StrId::STR_USB_DRIVE), I18N.get(StrId::STR_USB_DRIVE_CONNECT_DELAY), choices,
                                 2, 0, [this](int choice) { usbConfirmed_ = choice == 1; });
    requestUpdate();
    return;
  }
#endif
  onModeSelected(mode);
}

#if FREEINK_CAP_USB_MSC && (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PICCO)
bool NetworkModeSelectionActivity::handleCustomInput() {
  if (!usbConfirmation_.isActive()) return false;
  const bool handled = usbConfirmation_.handleInput(mappedInput, [this] { requestUpdate(); });
  // Complete the option popup's callback and repaint request before leaving.
  // Calling finish() from inside the callback would destroy a live popup.
  if (usbConfirmed_) {
    usbConfirmed_ = false;
    onModeSelected(NetworkMode::USB_DRIVE);
    return true;
  }
  return handled;
}

void NetworkModeSelectionActivity::renderOverlay() {
  if (usbConfirmation_.isActive()) usbConfirmation_.render(renderer);
}
#endif

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(visibleItems_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
