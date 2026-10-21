#pragma once

#include <Logging.h>
#include <Memory.h>

#include "PluginCatalogActivity.h"
#include "StatsDashboardActivity.h"

// Preserve the standard CrossPoint picker. Only a validated local dashboard
// manifest is intercepted; ordinary catalogs and README plugins are unchanged.
class StatsDashboardPluginCatalogActivity final : public PluginCatalogActivity {
 public:
  using PluginCatalogActivity::PluginCatalogActivity;

 protected:
  void activateIndex(int index) override {
    const std::string path = pickerManifestForRow(index);
    if (StatsDashboardActivity::supportsManifest(path)) {
      auto view = makeUniqueNoThrow<StatsDashboardActivity>(renderer, mappedInput, path);
      if (!view) {
        LOG_ERR("STATS", "Out of memory opening dashboard");
        return;
      }
      app.clearTapFlash();
      startActivityForResult(std::move(view), [](const ActivityResult&) {});
      return;
    }
    activateStandardPluginRow(index);
  }
};
