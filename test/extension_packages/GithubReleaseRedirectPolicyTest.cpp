#include <string>

#include "../../src/network/GithubReleaseRedirectPolicy.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
  using namespace githubreleasedirect;
  const std::string source = "https://github.com/jadehawk/halo.crosspoint-theme/releases/download/"
                             "v0.1.4/halo-0.1.4.crosspoint-theme.zip";
  std::string path;
  CHECK(githubReleasePath(source, path));
  CHECK(path == "/jadehawk/halo.crosspoint-theme/releases/download/v0.1.4/halo-0.1.4.crosspoint-theme.zip");
  CHECK(githubReleasePath("https://github.com/jadehawk/wallpapers/releases/download/v0.1.4/"
                          "starwars-x4plus-0.1.4.crosspoint-wallpaper.zip", path));
  CHECK(!githubReleasePath("http://github.com/a/b/releases/download/v0.1.4/foo.crosspoint-theme.zip", path));
  CHECK(!githubReleasePath("https://attacker.com/releases/download/v0.1.4/foo.crosspoint-theme.zip", path));
  CHECK(!githubReleasePath(source + "\r\nInjected: value", path));

  CHECK(redirectStatus("HTTP/1.1 302 Found"));
  CHECK(redirectStatus("HTTP/1.1 307 Temporary Redirect"));
  CHECK(!redirectStatus("HTTP/1.1 200 OK"));
  CHECK(!redirectStatus("HTTP/1.1 403 Forbidden"));
  CHECK(!redirectStatus("HTTP/1.1 3020 Found"));

  const std::string asset = "https://release-assets.githubusercontent.com/github-production-release-asset/"
                            "12345/blob?sv=2018-11-09&sig=token%2Fvalue";
  CHECK(signedAssetUrl(asset));
  std::string result;
  CHECK(locationHeader("Location: " + asset, result));
  CHECK(result == asset);
  CHECK(locationHeader("location:\t" + asset, result));
  CHECK(!locationHeader("Location: https://evil.example/assets?sig=x", result));
  CHECK(!locationHeader("Location: http://release-assets.githubusercontent.com/assets?sig=x", result));
  CHECK(!locationHeader("Location: " + asset + "\nInjected: header", result));
  CHECK(!locationHeader("Content-Length: 0", result));
  return 0;
}
