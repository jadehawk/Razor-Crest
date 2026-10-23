#pragma once

#include <string>

// Resolve the first, bodyless GitHub Releases 302 to the temporary HTTPS
// release-assets URL. Intentionally used only for theme/wallpaper ZIPs.
// Leaves normal plugin/book HTTP paths and final download verification intact.
namespace githubreleasedirect {
bool resolveAssetUrl(const std::string& githubReleaseUrl, std::string& assetUrl,
                     const bool* cancelled = nullptr);
}
