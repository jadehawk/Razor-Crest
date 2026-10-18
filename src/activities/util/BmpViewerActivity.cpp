#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char CUSTOM_SLEEP_TMP_BMP[] = "/sleep.bmp.tmp";
constexpr char JPEG_SLEEP_TMP_PXC[] = "/sleep.bmp.pxc.tmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;
constexpr uint8_t GRAYSCALE_PALETTE[] = {0, 0, 0, 0, 85, 85, 85, 0, 170, 170, 170, 0, 255, 255, 255, 0};

RenderConfig fitImage(const ImageDimensions& dimensions, const GfxRenderer& renderer) {
  const float scale = std::min({static_cast<float>(renderer.getScreenWidth()) / dimensions.width,
                                static_cast<float>(renderer.getScreenHeight()) / dimensions.height, 1.0f});
  const int width = std::max(1, std::min(renderer.getScreenWidth(), static_cast<int>(dimensions.width * scale)));
  const int height = std::max(1, std::min(renderer.getScreenHeight(), static_cast<int>(dimensions.height * scale)));
  RenderConfig config{(renderer.getScreenWidth() - width) / 2, (renderer.getScreenHeight() - height) / 2, width,
                      height};
  config.useExactDimensions = true;
  return config;
}

void drawProcessingFooter(GfxRenderer& renderer) {
  const auto originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int footerHeight = metrics.buttonHintsHeight;
  const int footerY = renderer.getScreenHeight() - footerHeight;
  const bool grayscalePlane = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();

  // In legacy grayscale planes, a set bit preserves the monochrome base. The
  // visible status strip itself is drawn only into the B/W (or absolute)
  // framebuffer so it stays solid white over any wallpaper artwork.
  renderer.fillRect(0, footerY, renderer.getScreenWidth(), footerHeight, grayscalePlane);
  if (!grayscalePlane) {
    const int textY = footerY + std::max(0, (footerHeight - renderer.getTextHeight(UI_10_FONT_ID)) / 2);
    renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_PROCESSING), true);
  }

  renderer.setOrientation(originalOrientation);
}

void clearActionFooter(GfxRenderer& renderer) {
  const auto originalOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRect(0, renderer.getScreenHeight() - metrics.buttonHintsHeight, renderer.getScreenWidth(),
                    metrics.buttonHintsHeight, false);
  renderer.setOrientation(originalOrientation);
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     const bool actionMode, std::string backLabel, std::string confirmLabel)
    : Activity("BmpViewer", renderer, mappedInput),
      UiAppHost(renderer),
      filePath(std::move(path)),
      actionMode(actionMode),
      actionBackLabel(std::move(backLabel)),
      actionConfirmLabel(std::move(confirmLabel)) {}

void BmpViewerActivity::actionScreen(UiScreen& screen, void* user) {
  static_cast<BmpViewerActivity*>(user)->buildActionScreen(screen);
}

void BmpViewerActivity::onCloseAction(const freeink::ui::ActionEvent&, void* user) {
  static_cast<BmpViewerActivity*>(user)->closeAction();
}

void BmpViewerActivity::onConfirmAction(const freeink::ui::ActionEvent&, void* user) {
  static_cast<BmpViewerActivity*>(user)->confirmAction();
}

void BmpViewerActivity::buildActionScreen(UiScreen& screen) {
  if (!actionMode || !actionReady) return;
  freeink::ui::FooterAction actions[2];
  actions[0].label = actionBackLabel.c_str();
  actions[0].action = ACTION_CLOSE;
  actions[1].label = actionConfirmLabel.c_str();
  actions[1].action = ACTION_CONFIRM;
  actions[1].enabled = actionCanConfirm;
  freeink::ui::FooterProps props;
  props.actions = actions;
  props.count = actionCanConfirm ? 2 : 1;
  screen.footer(props);
}

void BmpViewerActivity::closeAction() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BmpViewerActivity::confirmAction() {
  if (!actionCanConfirm) return;
  setResult(ActivityResult{});
  finish();
}

void BmpViewerActivity::drawReadyFooter(const MappedInputManager::Labels& labels, const bool allowConfirm) {
  if (actionMode) {
    actionReady = true;
    actionCanConfirm = allowConfirm;
    if (mappedInput.hasTouch()) {
      renderUi();
      return;
    }
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  const auto name = makeUniqueNoThrow<char[]>(500);
  if (!name) {
    LOG_ERR("BMP", "OOM: sibling filename buffer");
    return;
  }
  size_t imageCount = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name.get(), 500);
    if (name[0] != '.' && FsHelpers::hasImageExtension(std::string_view{name.get()})) ++imageCount;
  }
  dir.rewindDirectory();
  siblingImages.reserve(imageCount);

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name.get(), 500);
      if (name[0] != '.') {
        std::string fname(name.get());
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) || FsHelpers::hasJpgExtension(filePath) ||
         (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM &&
          FsHelpers::hasPngExtension(filePath));
}

MappedInputManager::Labels BmpViewerActivity::imageLabels(const bool hasPrevious, const bool hasNext,
                                                          const bool allowConfirm) const {
  if (actionMode) {
    return mappedInput.mapLabels(actionBackLabel.c_str(), allowConfirm ? actionConfirmLabel.c_str() : "", "", "");
  }
  return mappedInput.mapLabels(tr(STR_BACK), allowConfirm && canSetSleepCover() ? tr(STR_SET_SLEEP_COVER) : "",
                               hasPrevious ? "<" : "", hasNext ? ">" : "");
}

bool BmpViewerActivity::renderImage() {
  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  JpegToFramebufferConverter jpegDecoder;
  PngToFramebufferConverter pngDecoder;
  ImageToFramebufferDecoder& decoder =
      jpeg ? static_cast<ImageToFramebufferDecoder&>(jpegDecoder) : static_cast<ImageToFramebufferDecoder&>(pngDecoder);
  ImageDimensions dimensions;
  if (!decoder.getDimensions(filePath, dimensions)) return false;
  if (dimensions.width <= 0 || dimensions.height <= 0) return false;

  const auto config = fitImage(dimensions, renderer);
  const bool hasPrevious = siblingImages.size() > 1 && currentImageIndex > 0;
  const bool hasNext = siblingImages.size() > 1 && currentImageIndex != -1 &&
                       currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const auto readyLabels = imageLabels(hasPrevious, hasNext, true);
  const auto drawImage = [&](const bool processing) {
    if (!decoder.decodeToFramebuffer(filePath, renderer, config)) return false;
    if (processing) {
      drawProcessingFooter(renderer);
    } else {
      drawReadyFooter(readyLabels);
    }
    return true;
  };

  if (!jpeg) {
    if (!drawImage(false)) return false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  // Keep all actions off the visible B/W base while the grayscale planes are
  // generated. Once grayscale is complete, reveal the full Close/Save footer
  // together with a fast differential refresh.
  if (!drawImage(actionMode)) return false;
  const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return false;
  if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  bool ready = true;
  // ponytail: decode each plane; add a pixel cache if large-image latency needs it.
  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(mode);
    if (!drawImage(actionMode)) {
      ready = false;
      break;
    }
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  if (ready) renderer.displayGrayBuffer();

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!decoder.decodeToFramebuffer(filePath, renderer, config)) ready = false;
  if (actionMode) {
    // Match the footer that is still physically visible before rebasing the
    // controller's differential baseline. Clearing it before cleanup would
    // falsely tell the controller that Processing was already erased.
    drawProcessingFooter(renderer);
  } else {
    drawReadyFooter(readyLabels);
  }
  renderer.cleanupGrayscaleWithFrameBuffer();
  if (ready && actionMode) {
    // Transition from the synced Processing baseline to a clean white footer
    // plus the final actions, forcing the panel to erase every Processing pixel.
    clearActionFooter(renderer);
    drawReadyFooter(readyLabels);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  return ready;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  actionReady = !actionMode;
  actionCanConfirm = !actionMode;
  if (actionMode && mappedInput.hasTouch()) {
    resetUi();
    app.on(ACTION_CLOSE, &BmpViewerActivity::onCloseAction, this);
    app.on(ACTION_CONFIRM, &BmpViewerActivity::onConfirmAction, this);
    app.setScreen(&BmpViewerActivity::actionScreen, this);
  }

  if (!actionMode && siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  if (FsHelpers::hasPngExtension(filePath) || FsHelpers::hasJpgExtension(filePath)) {
    renderer.clearScreen();
    if (!renderImage()) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      const auto labels = imageLabels(false, false, false);
      drawReadyFooter(labels, false);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    } else {
      actionReady = true;
    }
    return;
  }

  HalFile file;
  // 1. Open the BMP file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const bool grayscale = bitmap.hasGreyscale();
      const bool processing = actionMode && grayscale;
      const auto readyLabels = imageLabels(hasPrevious, hasNext, true);
      const auto drawFooter = [&]() {
        if (processing) {
          drawProcessingFooter(renderer);
        } else {
          drawReadyFooter(readyLabels);
        }
      };
      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        renderer.clearScreen();
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
        const auto labels = imageLabels(false, false, false);
        drawReadyFooter(labels, false);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        return;
      }

      // Draw UI hints on the base layer
      drawFooter();
      if (grayscale) {
        const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
        if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
        if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
        bool planesReady = true;
        for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
          if (bitmap.rewindToData() != BmpReaderError::Ok) {
            LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
            planesReady = false;
            break;
          }
          renderer.clearScreen(absolute ? 0xFF : 0x00);
          renderer.setRenderMode(mode);
          if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
            planesReady = false;
            break;
          }
          drawFooter();
          if (mode == GfxRenderer::GRAYSCALE_LSB) {
            renderer.copyGrayscaleLsbBuffers();
          } else {
            renderer.copyGrayscaleMsbBuffers();
          }
        }
        if (planesReady) renderer.displayGrayBuffer();

        // Rebuild the BW framebuffer for popups and subsequent differential updates.
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.clearScreen();
        if (bitmap.rewindToData() != BmpReaderError::Ok ||
            !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
          LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
          planesReady = false;
        }
        if (actionMode) {
          // Preserve the actually visible Processing footer as the controller
          // baseline before transitioning it to the final action footer.
          drawProcessingFooter(renderer);
        } else {
          drawFooter();
        }
        renderer.cleanupGrayscaleWithFrameBuffer();
        if (planesReady) {
          if (actionMode) {
            clearActionFooter(renderer);
            drawReadyFooter(readyLabels);
            renderer.displayBuffer(HalDisplay::FAST_REFRESH);
          }
          actionReady = true;
        } else {
          if (actionMode) {
            clearActionFooter(renderer);
            drawReadyFooter(readyLabels, false);
          }
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        actionReady = true;
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
      const auto labels = imageLabels(false, false, false);
      drawReadyFooter(labels, false);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = imageLabels(false, false, false);
    drawReadyFooter(labels, false);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  if (actionMode && mappedInput.hasTouch()) closeRouting();
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::saveJpegSleepCover() {
  if (Storage.exists(JPEG_SLEEP_TMP_PXC) && !Storage.remove(JPEG_SLEEP_TMP_PXC)) {
    LOG_ERR("BMP", "Failed to remove previous sleep image pixels");
    return false;
  }
  bool keepBmpTemp = false;
  const ScopedCleanup cleanup{[&keepBmpTemp]() {
    Storage.remove(JPEG_SLEEP_TMP_PXC);
    if (!keepBmpTemp) Storage.remove(CUSTOM_SLEEP_TMP_BMP);
  }};

  JpegToFramebufferConverter decoder;
  ImageDimensions dimensions;
  if (!decoder.getDimensions(filePath, dimensions)) return false;
  auto config = fitImage(dimensions, renderer);
  if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP &&
      (dimensions.width > renderer.getScreenWidth() || dimensions.height > renderer.getScreenHeight())) {
    const float ratio = static_cast<float>(dimensions.width) / dimensions.height;
    const float screenRatio = static_cast<float>(renderer.getScreenWidth()) / renderer.getScreenHeight();
    config.x = config.y = 0;
    config.maxWidth = renderer.getScreenWidth();
    config.maxHeight = renderer.getScreenHeight();
    if (ratio > screenRatio) {
      config.sourceCropX = 1.0f - screenRatio / ratio;
    } else {
      config.sourceCropY = 1.0f - ratio / screenRatio;
    }
  }
  config.cachePath = JPEG_SLEEP_TMP_PXC;
  renderer.clearScreen();
  bool cacheWritten;
  if (!decoder.decodeToFramebuffer(filePath, renderer, config, cacheWritten) || !cacheWritten) {
    LOG_ERR("BMP", "Failed to decode sleep image pixels");
    return false;
  }

  HalFile input, output;
  if (!Storage.openFileForRead("BMP", JPEG_SLEEP_TMP_PXC, input)) return false;
  uint16_t width, height;
  if (input.read(&width, sizeof(width)) != sizeof(width) || input.read(&height, sizeof(height)) != sizeof(height) ||
      width != config.maxWidth || height != config.maxHeight) {
    LOG_ERR("BMP", "Invalid sleep image pixels");
    return false;
  }
  const size_t pixelRowBytes = (width + 3) / 4;
  const size_t bmpRowBytes = (width + 15) / 16 * 4;
  if (input.size() != 4 + pixelRowBytes * height) {
    LOG_ERR("BMP", "Incomplete sleep image pixels");
    return false;
  }
  auto row = makeUniqueNoThrow<uint8_t[]>(bmpRowBytes);
  if (!row) {
    LOG_ERR("BMP", "OOM: sleep image row");
    return false;
  }
  if (!Storage.openFileForWrite("BMP", CUSTOM_SLEEP_TMP_BMP, output)) return false;
  BmpHeader header;
  createBmpHeader(&header, width, height, BmpRowOrder::TopDown);
  header.infoHeader.biBitCount = 2;
  header.infoHeader.biClrUsed = header.infoHeader.biClrImportant = 4;
  header.infoHeader.biSizeImage = bmpRowBytes * height;
  header.fileHeader.bfOffBits = sizeof(header) - sizeof(header.colors) + sizeof(GRAYSCALE_PALETTE);
  header.fileHeader.bfSize = header.fileHeader.bfOffBits + header.infoHeader.biSizeImage;
  if (output.write(&header, sizeof(header) - sizeof(header.colors)) != sizeof(header) - sizeof(header.colors) ||
      output.write(GRAYSCALE_PALETTE, sizeof(GRAYSCALE_PALETTE)) != sizeof(GRAYSCALE_PALETTE)) {
    LOG_ERR("BMP", "Failed to write sleep image header");
    return false;
  }
  for (int y = 0; y < height; ++y) {
    if (input.read(row.get(), pixelRowBytes) != pixelRowBytes || output.write(row.get(), bmpRowBytes) != bmpRowBytes) {
      LOG_ERR("BMP", "Failed to write sleep image row %d", y);
      return false;
    }
  }
  if (!output.close()) {
    LOG_ERR("BMP", "Failed to close sleep image");
    return false;
  }
  if (!Storage.replaceFile(CUSTOM_SLEEP_TMP_BMP, CUSTOM_SLEEP_ROOT_BMP)) {
    // ponytail: FAT replacement is not atomic; keep the complete temp for recovery.
    keepBmpTemp = true;
    LOG_ERR("BMP", "Failed to publish sleep image");
    return false;
  }
  return true;
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const bool jpeg = FsHelpers::hasJpgExtension(filePath);
  const bool transparentMode =
      !jpeg && SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
  if (!canSetSleepCover()) return;

  const char* destination =
      transparentMode ? (FsHelpers::hasPngExtension(filePath) ? TRANSPARENT_SLEEP_ROOT_PNG : TRANSPARENT_SLEEP_ROOT_BMP)
                      : CUSTOM_SLEEP_ROOT_BMP;
  bool success = filePath == destination;

  if (jpeg) {
    success = saveJpegSleepCover();
  } else if (!success) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
    if (!buffer) {
      LOG_ERR("BMP", "OOM: sleep cover copy buffer");
    } else {
      // Copy beside the target and swap in only a complete image, so a failed
      // copy keeps the previous sleep cover instead of a truncated one.
      const std::string tmp = std::string(destination) + ".tmp";
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", tmp, outFile)) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
          if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
            success = false;
            break;
          }
        }
        if (bytesRead < 0) success = false;
        outFile.close();
        success = success && Storage.replaceFile(tmp.c_str(), destination);
        if (!success) Storage.remove(tmp.c_str());
      }
    }
  }

  if (success) {
    if (!transparentMode) SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (actionMode) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeAction();
      return;
    }
    if (actionReady && mappedInput.hasTouch()) {
      const auto route = routeTouch(mappedInput);
      if (route.routed) return;
    }
    if (actionReady && actionCanConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmAction();
      return;
    }
    return;
  }

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    onEnter();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!canSetSleepCover()) return;

    auto confirmation =
        makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_SET_SLEEP_COVER) + std::string("?"),
                                                filePath.substr(filePath.find_last_of('/') + 1));
    if (!confirmation) {
      LOG_ERR("BMP", "OOM: sleep cover confirmation (%zu bytes)", sizeof(ConfirmationActivity));
      return;
    }
    startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        onEnter();
        return;
      }
      doSetSleepCover();
    });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
