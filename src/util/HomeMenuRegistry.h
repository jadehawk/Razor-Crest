#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

// Firmware-owned Home shortcuts. On-device plugins can request only an
// already-supported native dashboard; they never supply code or actions.
class HomeMenuRegistry {
 public:
  enum class Target : uint8_t { Browse, Library, Plugins, Opds, Transfer, Dashboard, Settings };
  struct Entry {
    std::string id;
    std::string label;
    std::string deviceManifest;
    Target target = Target::Browse;
    UIIcon icon = Folder;
    bool enabled = false;
  };

  void load(int capacity, bool pluginsEnabled, bool opdsAvailable);
  const std::vector<Entry>& all() const { return entries_; }
  std::vector<Entry> visible() const;
  bool setEnabled(const std::string& id, bool enabled);
  bool move(const std::string& id, int direction);
  bool reset();
  int capacity() const { return capacity_; }

  // Clamp by actual screen layout; the native Settings control is never
  // removed, so even crowded themes retain a path back to configuration.
  static int capacityFor(const GfxRenderer& renderer, int menuTop, int rowHeight, int rowSpacing, int hintsHeight,
                         int footerHeight);

 private:
  bool save() const;
  int capacity_ = 5;
  std::vector<Entry> entries_;
};
