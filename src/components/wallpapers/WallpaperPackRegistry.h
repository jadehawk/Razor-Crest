#pragma once

#include <string>
#include <vector>

// Independent of UI themes. BMP-only v1 collections use the existing Custom
// sleep-screen decoder and can also supply timed refresh without .sleep copies.
struct WallpaperPackChoice {
  std::string id;
  std::string label;
};

class WallpaperPackRegistry {
 public:
  static WallpaperPackRegistry& getInstance();
  void scan();
  const std::vector<WallpaperPackChoice>& choices() const { return installed; }
  bool selectImage(const char* id, std::string& path) const;

 private:
  std::vector<WallpaperPackChoice> installed;
};
