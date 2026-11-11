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
struct ModeRow {
  NetworkMode mode;
  StrId label;
  StrId desc;
  UIIcon icon;
};

// Present LocalSend directly after Join Network when its plugin is available.
// Explicit modes avoid coupling selection indices to enum values.
constexpr ModeRow MODE_ROWS[] = {
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, UIIcon::Wifi},
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    {NetworkMode::LOCAL_SEND, StrId::STR_LOCALSEND_MENU, StrId::STR_LOCALSEND_MENU_DESC, UIIcon::Wifi},
#endif
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, UIIcon::Library},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, UIIcon::Hotspot},
    {NetworkMode::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_DESC, UIIcon::Book},
#if FREEINK_CAP_USB_MSC
    {NetworkMode::USB_DRIVE, StrId::STR_USB_DRIVE, StrId::STR_USB_DRIVE_DESC, UIIcon::Usb},
#endif
};
static_assert(sizeof(MODE_ROWS) / sizeof(MODE_ROWS[0]) == NetworkModeSelectionActivity::MAX_ITEM_COUNT);
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  for (const ModeRow& row : MODE_ROWS) {
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
    if (row.mode == NetworkMode::LOCAL_SEND && !LocalSendPluginActivity::isAvailable()) continue;
#endif
    fui::ListItem item;
    item.label = I18N.get(row.label);
    item.subtitle = I18N.get(row.desc);
    item.icon = listIconFor(row.icon, 32);
    item.actionValue = static_cast<int16_t>(rowCount_);
    rowModes_[rowCount_] = row.mode;
    rowItems_[rowCount_++] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const { return rowCount_; }

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount_) return;
  // Keep the current row selected behind the USB confirmation popup.
  app.clearTapFlash();
  nav.selected = index;
  const NetworkMode mode = rowModes_[index];

#if FREEINK_CAP_USB_MSC && (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_PICCO)
  if (mode == NetworkMode::USB_DRIVE && UITheme::getInstance().activeExternalScene()) {
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
  props.count = static_cast<uint16_t>(rowCount_);
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
