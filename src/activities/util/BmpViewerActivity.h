#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

class BmpViewerActivity final : public Activity, private UiAppHost {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    bool actionMode = false, std::string backLabel = {}, std::string confirmLabel = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  static constexpr freeink::ui::ActionId ACTION_CLOSE = 1;
  static constexpr freeink::ui::ActionId ACTION_CONFIRM = 2;

  static void actionScreen(UiScreen& screen, void* user);
  static void onCloseAction(const freeink::ui::ActionEvent& event, void* user);
  static void onConfirmAction(const freeink::ui::ActionEvent& event, void* user);
  void buildActionScreen(UiScreen& screen);
  void closeAction();
  void confirmAction();
  void drawReadyFooter(const MappedInputManager::Labels& labels, bool allowConfirm = true);
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderImage();
  MappedInputManager::Labels imageLabels(bool hasPrevious, bool hasNext, bool allowConfirm = true) const;
  bool saveJpegSleepCover();

  std::string filePath;
  bool actionMode = false;
  bool actionReady = false;
  bool actionCanConfirm = false;
  std::string actionBackLabel;
  std::string actionConfirmLabel;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};
