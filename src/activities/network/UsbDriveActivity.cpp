#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "components/icons/transfer.h"

namespace fui = freeink::ui;

namespace {
// Render all grayscale planes while the SD card still belongs to firmware.
// No theme image, font, or open file survives the USB raw-storage handoff.
bool drawUsbGrayscale(const std::string& path, GfxRenderer& renderer) {
  if (path.empty()) return false;
  const auto mode = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
                        ? HalDisplay::GrayscaleMode::Direct
                        : HalDisplay::GrayscaleMode::Absolute;
  if (!renderer.grayscaleCapabilities(mode).supported()) return false;
  HalFile file;
  if (!Storage.openFileForRead("USB", path, file)) return false;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || !bitmap.hasGreyscale() ||
      bitmap.getWidth() != renderer.getScreenWidth() || bitmap.getHeight() != renderer.getScreenHeight()) {
    LOG_ERR("USB", "Unsupported USB grayscale image dimensions or format");
    return false;
  }
  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight())) return false;
  if (!renderer.displayGrayscaleBase(mode)) return false;
  bool complete = true;
  for (auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      complete = false;
      break;
    }
    renderer.clearScreen(mode == HalDisplay::GrayscaleMode::Absolute ? 0xFF : 0x00);
    renderer.setRenderMode(plane);
    if (!renderer.drawBitmap(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight())) {
      complete = false;
      break;
    }
    if (plane == GfxRenderer::GRAYSCALE_LSB)
      renderer.copyGrayscaleLsbBuffers();
    else
      renderer.copyGrayscaleMsbBuffers();
  }
  // Keep the grayscale render mode until displayGrayBuffer commits both planes.
  if (!complete) {
    renderer.setRenderMode(GfxRenderer::BW);  // Abort incomplete grayscale safely.
    LOG_ERR("USB", "USB grayscale planes incomplete");
    return false;
  }
  // The renderer cancels an unfinished absolute pass on a BW mode change.
  // Commit LSB/MSB first, exactly as SleepActivity does.
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  LOG_INF("USB", "Displayed themed grayscale USB scene");
  return true;
}
}  // namespace

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&UsbDriveActivity::driveScreen, this);

  // Show the safety instructions before giving the raw SD card to the USB host.
  requestUpdateAndWait();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  if (!restartRequested) Storage.endUsbDrive();
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (!startFailed) {
    const State nextState = Storage.usbDriveState();
    if (nextState != state) {
      state = nextState;
      requestUpdate();
    }
  }

  // A battery-powered USB device may stay mounted after cable removal. End
  // sustained suspend sessions too; host sleep intentionally has the same policy.
  if (state == State::Connected && Storage.usbDriveHostSuspended()) {
    if (!hostSuspendPending) {
      hostSuspendPending = true;
      hostSuspendStartedAt = millis();
    } else if (millis() - hostSuspendStartedAt >= HOST_SUSPEND_TIMEOUT_MS) {
      LOG_INF("USB", "USB Drive host suspend timed out; ending session");
      // endUsbDrive() soft-disconnects before teardown and reboot.
      restartToHome();
      return;
    }
  } else {
    hostSuspendPending = false;
  }

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (!startFailed && state == State::IoError) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  const bool canExitWithInput = state == State::WaitingForHost || startFailed;
  if (canExitWithInput && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture())) {
    restartToHome();
    return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  // Once USB owns the SD card, preserve the already-painted e-ink frame.
  // Redrawing themed glyphs or art would read an unmounted filesystem.
  if (usbFramePainted && !startFailed) return;
  if (preparing && !startFailed) {
    if (drawUsbGrayscale(UITheme::getInstance().usbGrayscalePath(), renderer)) {
      usbFramePainted = true;
      return;
    }
    renderer.clearScreen();
    if (!UITheme::getInstance().renderExternalScene(
            ThemeScene::Region::Usb, renderer, Rect{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()})) {
      renderer.clearScreen();
      const int centerX = renderer.getScreenWidth() / 2;
      const int centerY = renderer.getScreenHeight() / 2;
      renderer.drawRect(centerX - 54, centerY - 54, 108, 108);
      renderer.drawIcon(TransferIcon, centerX - 16, centerY - 16, 32);
    }
    renderer.displayBuffer();
    usbFramePainted = true;
    return;
  }
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_USB_DRIVE));

  renderUi();

  if (state == State::WaitingForHost || startFailed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void UsbDriveActivity::driveScreen(UiScreen& screen, void* user) {
  static_cast<UsbDriveActivity*>(user)->buildDriveScreen(screen);
}

void UsbDriveActivity::buildDriveScreen(UiScreen& screen) const {
  const char* message = nullptr;
  const char* detail = nullptr;
  const char* secondaryDetail = nullptr;
  if (preparing) {
    message = tr(STR_USB_DRIVE_PREPARING);
    detail = tr(STR_USB_DRIVE_EJECT_HINT);
  } else {
    switch (state) {
      case State::WaitingForHost:
        message = tr(STR_USB_DRIVE_WAITING);
        break;
      case State::Connected:
        message = tr(STR_USB_DRIVE_CONNECTED);
        detail = tr(STR_USB_DRIVE_CONNECT_DELAY);
        secondaryDetail = tr(STR_USB_DRIVE_EJECT_HINT);
        break;
      case State::IoError:
        message = startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR);
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        return;
    }
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});

  auto messageStyle = screen.theme().smallText;
  messageStyle.align = fui::TextAlign::Center;
  messageStyle.bold = true;
  messageStyle.maxLines = 2;
  auto detailStyle = screen.theme().smallText;
  detailStyle.align = fui::TextAlign::Center;
  detailStyle.maxLines = 3;

  const fui::Rect body = screen.body();
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), message, messageStyle, body.width).height;
  const int16_t detailHeight =
      detail ? fui::measureWrappedText(screen.target(), detail, detailStyle, body.width).height : 0;
  const int16_t secondaryDetailHeight =
      secondaryDetail ? fui::measureWrappedText(screen.target(), secondaryDetail, detailStyle, body.width).height : 0;
  const int16_t gap = detail ? screen.theme().spaceMd : 0;
  const int16_t secondaryGap = secondaryDetail ? screen.theme().spaceMd : 0;
  const int16_t totalHeight =
      static_cast<int16_t>(messageHeight + gap + detailHeight + secondaryGap + secondaryDetailHeight);
  if (body.height > totalHeight) screen.spacer(static_cast<int16_t>((body.height - totalHeight) / 2));

  screen.target().text(screen.takeTop(messageHeight, gap), message, messageStyle);
  if (detail) {
    screen.target().text(screen.takeTop(detailHeight, secondaryGap), detail, detailStyle);
  }
  if (secondaryDetail) {
    screen.target().text(screen.takeTop(secondaryDetailHeight), secondaryDetail, detailStyle);
  }
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  Storage.endUsbDrive();
  delay(20);
  restartToHomeAfterStorageHandoff();
}
