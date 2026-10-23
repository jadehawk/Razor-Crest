#include "GithubReleaseRedirect.h"

#include <Arduino.h>
#include <Logging.h>
#include <SecureClient.h>

#include <cstdint>
#include <string>
#include <utility>

#include "GithubReleaseRedirectPolicy.h"
#include "WifiPowerSaveGuard.h"

namespace githubreleasedirect {
namespace {
constexpr unsigned long HEADER_TIMEOUT_MS = 8000;
constexpr size_t MAX_HEADER_BYTES = 8192;
constexpr size_t MAX_LINE_BYTES = 4120;
constexpr char HOST[] = "github.com";

bool cancelledNow(const bool* cancelled) { return cancelled && *cancelled; }
}  // namespace

bool resolveAssetUrl(const std::string& githubReleaseUrl, std::string& assetUrl, const bool* cancelled) {
  assetUrl.clear();
  std::string path;
  if (!githubReleasePath(githubReleaseUrl, path) || cancelledNow(cancelled)) return false;

  // A GitHub Release URL redirects to a signed HTTPS CDN URL. The general
  // client drains the intermediate 302 response body, which has stalled for
  // ~30 seconds on device. Read ONLY the response headers, then close this
  // connection before downloading from the trusted CDN normally.
  WifiPowerSaveGuard powerGuard;
  const unsigned long started = millis();
  freeink::SecureClient client;
  client.setInsecure();  // Same transport policy as HttpDownloader.
  client.setAbortCallback([cancelled] { return cancelledNow(cancelled); });
  if (!client.connect(HOST, 443)) {
    LOG_INF("EXT", "GitHub redirect preflight: TLS/connect unavailable (%lu ms)", millis() - started);
    return false;
  }
  const std::string request =
      "GET " + path + " HTTP/1.1\r\nHost: github.com\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION
      "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
  if (client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != request.size()) {
    client.stop();
    return false;
  }

  std::string line;
  line.reserve(192);
  bool firstLine = true;
  bool redirected = false;
  size_t seen = 0;
  const unsigned long deadline = millis() + HEADER_TIMEOUT_MS;
  while (static_cast<int32_t>(millis() - deadline) < 0 && seen < MAX_HEADER_BYTES && !cancelledNow(cancelled)) {
    if (client.available() <= 0) {
      delay(1);
      continue;
    }
    const int byte = client.read();
    if (byte < 0) continue;
    ++seen;
    if (byte != '\n') {
      if (line.size() >= MAX_LINE_BYTES) break;
      if (byte != '\r') line.push_back(static_cast<char>(byte));
      continue;
    }
    if (firstLine) {
      redirected = redirectStatus(line);
      firstLine = false;
      if (!redirected) break;
    } else if (line.empty()) {
      break;  // Header block ended without a supported Location.
    } else if (locationHeader(line, assetUrl)) {
      // A signed URL never gets printed (contains ephemeral tokens).
      client.stop();  // Deliberately do not wait for a redirect body/trailer.
      LOG_INF("EXT", "GitHub redirect resolved from headers in %lu ms", millis() - started);
      return true;
    }
    line.clear();
  }
  client.stop();
  assetUrl.clear();
  LOG_INF("EXT", "GitHub redirect preflight unavailable after %lu ms (falling back)", millis() - started);
  return false;
}
}  // namespace githubreleasedirect
