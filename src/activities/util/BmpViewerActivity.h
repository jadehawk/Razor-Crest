#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    bool actionMode = false, std::string backLabel = {}, std::string confirmLabel = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderImage();
  MappedInputManager::Labels imageLabels(bool hasPrevious, bool hasNext, bool allowConfirm = true) const;
  bool saveJpegSleepCover();

  std::string filePath;
  bool actionMode = false;
  bool actionReady = false;
  std::string actionBackLabel;
  std::string actionConfirmLabel;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};
