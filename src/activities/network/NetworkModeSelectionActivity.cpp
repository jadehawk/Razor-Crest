#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return MENU_ITEM_COUNT; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Clear transient tap flash; keep the USB row selected behind the popup.
  // Selection focus is retained by nav.selected until confirmation.
  app.clearTapFlash();
  nav.selected = index;

#if FREEINK_CAP_USB_MSC && FREEINK_DEVICE_X4PRO
  if (static_cast<NetworkMode>(index) == NetworkMode::USB_DRIVE) {
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
  onModeSelected(static_cast<NetworkMode>(index));
}

#if FREEINK_CAP_USB_MSC && FREEINK_DEVICE_X4PRO
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
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
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
