#pragma once

#include <string>
#include <string_view>

namespace githubreleasedirect {

// Firmware requests only trusted catalog GitHub release ZIPs. Reject control
// characters in both request and redirect to prevent header injection.
inline bool githubReleasePath(std::string_view url, std::string& path) {
  constexpr std::string_view origin = "https://github.com";
  if (url.size() > 512 || url.substr(0, origin.size() + 1) != "https://github.com/" ||
      url.find("/releases/download/v") == std::string_view::npos ||
      (url.find(".crosspoint-theme.zip") == std::string_view::npos &&
       url.find(".crosspoint-wallpaper.zip") == std::string_view::npos))
    return false;
  for (char c : url.substr(origin.size())) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
          c == '.' || c == '/')) return false;
  }
  path.assign(url.substr(origin.size()));
  return true;
}

inline bool redirectStatus(std::string_view line) {
  if (line.substr(0, 9) != "HTTP/1.1 " && line.substr(0, 9) != "HTTP/1.0 ") return false;
  if (line.size() < 12 || (line.size() > 12 && line[12] != ' ')) return false;
  const auto status = line.substr(9, 3);
  return status == "301" || status == "302" || status == "307" || status == "308";
}

inline bool signedAssetUrl(std::string_view url) {
  constexpr std::string_view domain1 = "https://release-assets.githubusercontent.com/";
  constexpr std::string_view domain2 = "https://objects.githubusercontent.com/";
  if (url.size() < 65 || url.size() > 4096 ||
      (url.substr(0, domain1.size()) != domain1 && url.substr(0, domain2.size()) != domain2))
    return false;
  if (url.find('?') == std::string_view::npos) return false;
  for (char c : url) {
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e ||
        c == '#' || c == '\\') return false;
  }
  return true;
}

// Parse a header line without buffering the body or relying on a successful
// chunked redirect-body terminator. Caller still checks the redirect status.
inline bool locationHeader(std::string_view line, std::string& location) {
  constexpr std::string_view key = "location:";
  if (line.size() <= key.size() || line.size() > 4120) return false;
  for (size_t i = 0; i < key.size(); ++i) {
    char c = line[i];
    if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    if (c != key[i]) return false;
  }
  auto value = line.substr(key.size());
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
  if (!signedAssetUrl(value)) return false;
  location.assign(value);
  return true;
}
}  // namespace githubreleasedirect
