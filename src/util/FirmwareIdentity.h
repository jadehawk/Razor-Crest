#pragma once

#ifndef CROSSPOINT_FIRMWARE_FAMILY
#define CROSSPOINT_FIRMWARE_FAMILY "crosspoint"
#endif

#ifndef CROSSPOINT_FIRMWARE_ID
#define CROSSPOINT_FIRMWARE_ID "Razor-Crest"
#endif

#ifndef CROSSPOINT_PLUGIN_API_VERSION
#define CROSSPOINT_PLUGIN_API_VERSION 1
#endif

#ifndef CROSSPOINT_PLUGIN_CAPABILITIES
#define CROSSPOINT_PLUGIN_CAPABILITIES                                                                             \
  "catalog.lists,catalog.notices,catalog.bundle_install,catalog.version_tracking,catalog.compatibility,catalog."   \
  "images,"                                                                                                        \
  "catalog.cache,catalog.refresh,catalog.item_details,catalog.hardware_filter.v1,stats.native.v1,ui.dashboard.v1," \
  "theme.sd.v1,theme.metrics.v1,theme.images.v1,theme.composition.v1,theme.fonts.v1,"                              \
  "theme.font-family.v1,theme.settings-navigation.v1,theme.book-cover.v1,theme.menu-decoration.v1,"                \
  "theme.screen-decoration.v1,wallpaper.packs.v1"
#endif

namespace firmwareidentity {
inline constexpr const char* FAMILY = CROSSPOINT_FIRMWARE_FAMILY;
inline constexpr const char* ID = CROSSPOINT_FIRMWARE_ID;
inline constexpr int PLUGIN_API_VERSION = CROSSPOINT_PLUGIN_API_VERSION;
inline constexpr const char* PLUGIN_CAPABILITIES = CROSSPOINT_PLUGIN_CAPABILITIES;
}  // namespace firmwareidentity
