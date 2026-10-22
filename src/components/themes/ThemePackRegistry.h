#pragma once

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

// Discovery is cold-path SD I/O; only the selected theme's metrics are retained.
struct SdThemeChoice {
  std::string id;
  std::string label;
  std::string path;
};

class ThemePackRegistry {
 public:
  static ThemePackRegistry& getInstance();

  void scan();
  const std::vector<SdThemeChoice>& choices() const { return installed; }
  bool loadMetrics(const char* id, ThemeMetrics& out, std::string* definitionPath = nullptr) const;

 private:
  std::vector<SdThemeChoice> installed;
};
