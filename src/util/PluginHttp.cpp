#include "PluginHttp.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "activities/RenderLock.h"
#include "network/ProtectedPaths.h"
#include "network/WifiPowerSaveGuard.h"

extern GfxRenderer renderer;

namespace {
constexpr size_t MAX_TOKEN_FILE_SIZE = 2 * 1024;

bool networkReady() { return WiFi.getMode() != WIFI_MODE_NULL && WiFi.status() == WL_CONNECTED; }

void releaseFontCachesBeforeTls(const std::string& url) {
  if (url.compare(0, 8, "https://") != 0) return;
  const auto heap = HalMemory::getInternalHeap();
  const bool hasPsram = HalMemory::getPsramHeap().totalBytes > 0;
  // The no-PSRAM baseline needs more internal headroom for the whole TLS
  // handshake. On PSRAM boards, retain hot UI caches unless DRAM is tight.
  const size_t minFree = hasPsram ? 48 * 1024 : 104 * 1024;
  const size_t minBlock = hasPsram ? 24 * 1024 : 48 * 1024;
  if (heap.freeBytes >= minFree && heap.largestBlockBytes >= minBlock) return;
  // Cache pointers are shared with rendering; never free them without its lock.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  if (auto* caches = renderer.getFontCacheManager()) {
    caches->releaseSdFontCaches();
    const auto after = HalMemory::getInternalHeap();
    if (after.freeBytes > heap.freeBytes + 4096) {
      LOG_INF("PHTP", "TLS cache reclaim: free=%u->%u max=%u->%u", (unsigned)heap.freeBytes, (unsigned)after.freeBytes,
              (unsigned)heap.largestBlockBytes, (unsigned)after.largestBlockBytes);
    }
  }
}

void releaseIdleTlsSessionIfTight(freeink::SecureHttpClient* session) {
  if (!session) return;
  const auto heap = HalMemory::getInternalHeap();
  const bool hasPsram = HalMemory::getPsramHeap().totalBytes > 0;
  const size_t minFree = hasPsram ? 48 * 1024 : 104 * 1024;
  const size_t minBlock = hasPsram ? 24 * 1024 : 48 * 1024;
  if (heap.freeBytes >= minFree && heap.largestBlockBytes >= minBlock) return;
  // A keep-alive TLS connection can retain substantial heap while a catalog
  // renders. Close only under pressure; future requests reconnect normally.
  session->end();
  const auto after = HalMemory::getInternalHeap();
  if (after.freeBytes > heap.freeBytes + 4096) {
    LOG_INF("PHTP", "TLS idle session released: free=%u->%u max=%u->%u", (unsigned)heap.freeBytes,
            (unsigned)after.freeBytes, (unsigned)heap.largestBlockBytes, (unsigned)after.largestBlockBytes);
  }
}

// Resolves the caller's reused TLS session (or `tmp`) into a configured,
// connected client, or nullptr if begin() or the connection failed.
freeink::SecureHttpClient* openClient(freeink::SecureHttpClient* session, freeink::SecureHttpClient& tmp,
                                      const std::string& url, const pluginhttp::Headers& headers,
                                      const std::function<bool()>& shouldAbort = nullptr) {
  freeink::SecureHttpClient& http = session ? *session : tmp;
  http.setUserAgent("CrossPoint");
  // Same trust posture as every other SecureNet consumer: no CA bundle ships
  // with the transport, so verification is skipped; traffic stays encrypted.
  http.setInsecure();
  http.setTimeout(30000);
  if (!http.begin(url)) return nullptr;
  // Handshake first: the client's copy of a large header (a session cookie can
  // be several KB) would otherwise take heap the handshake needs.
  if (!http.connect(shouldAbort)) return nullptr;
  for (const auto& h : headers) http.addHeader(h.first, h.second);
  return &http;
}
}  // namespace

namespace pluginhttp {

// True when a dotted-path segment addresses an array index.
static bool segIsIndex(const std::string& seg) { return !seg.empty() && isdigit(static_cast<unsigned char>(seg[0])); }

void splitPath(const std::string& dotted, std::vector<std::string>& out) {
  out.clear();
  out.reserve(static_cast<size_t>(std::count(dotted.begin(), dotted.end(), '.')) + 1);
  size_t start = 0;
  while (start <= dotted.size()) {
    const size_t dot = dotted.find('.', start);
    if (dot == std::string::npos) {
      if (start < dotted.size()) out.push_back(dotted.substr(start));
      break;
    }
    if (dot > start) out.push_back(dotted.substr(start, dot - start));
    start = dot + 1;
  }
}

JsonVariantConst resolvePath(JsonVariantConst node, const std::string& dotted) {
  if (dotted.empty()) return node;
  std::vector<std::string> segs;
  splitPath(dotted, segs);
  for (const auto& seg : segs) {
    if (node.isNull()) break;
    if (segIsIndex(seg)) {
      node = node[atoi(seg.c_str())];
    } else {
      node = node[seg.c_str()];
    }
  }
  return node;
}

std::string variantToString(JsonVariantConst v) {
  if (v.isNull()) return "";
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
  char buf[24];
  if (v.is<long long>() || v.is<int>()) {
    snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
    return buf;
  }
  return "";
}

void substituteAll(std::string& s, const char* key, const std::string& value) {
  const size_t keyLen = strlen(key);
  size_t pos = 0;
  while ((pos = s.find(key, pos)) != std::string::npos) {
    s.replace(pos, keyLen, value);
    pos += value.size();
  }
}

void readRequest(JsonVariantConst node, const char* defaultMethod, RequestSpec& out) {
  out.url = node["url"] | "";
  out.method = node["method"] | defaultMethod;
  out.body = node["body"] | "";
  readHeaders(node["headers"], out.headers);
}

void readHeaders(JsonVariantConst node, Headers& out) {
  out.clear();
  const JsonObjectConst headers = node.as<JsonObjectConst>();
  out.reserve(headers.size());
  for (JsonPairConst kv : headers) {
    out.emplace_back(kv.key().c_str(), kv.value().as<const char*>() ? kv.value().as<const char*>() : "");
  }
}

std::string urlEncodeQuery(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

std::string inPluginDir(const std::string& pluginDir, const char* path) {
  if (!path || !path[0] || path[0] == '/') return path ? path : "";
  return pluginDir + "/" + path;
}

// A manifest may not point the firmware at a credential store.
static bool allowedFile(const std::string& file) {
  if (protectedpaths::isPluginPath(file)) return true;
  LOG_ERR("PHTP", "Manifest file not allowed: %s", file.c_str());
  return false;
}

bool loadTokenFromFile(const std::string& file, const std::string& path, std::string& out) {
  out.clear();
  if (file.empty()) return true;  // token-less plugin
  if (!allowedFile(file)) return false;
  std::string raw;
  if (!Storage.readFileToString("PHTP", file, MAX_TOKEN_FILE_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  out = variantToString(resolvePath(doc.as<JsonVariantConst>(), path));
  return !out.empty();
}

bool saveTokenToFile(const std::string& file, const std::string& path, const std::string& value) {
  if (file.empty()) return false;
  if (!allowedFile(file)) return false;
  JsonDocument doc;
  // Build the nesting the read path expects (numeric segments unsupported).
  std::vector<std::string> segs;
  splitPath(path, segs);
  if (segs.empty()) return false;
  JsonVariant node = doc.to<JsonObject>();
  for (size_t i = 0; i + 1 < segs.size(); i++) node = node[segs[i]].to<JsonObject>();
  node[segs.back()] = value;
  std::string out;
  serializeJson(doc, out);
  HalFile f;
  if (!Storage.openFileForWrite("PHTP", file, f)) return false;
  const size_t written = f.write(out.data(), out.size());
  f.flush();
  if (written != out.size()) {
    // A short write (SD full, I/O fault) must not persist a truncated token:
    // the writer would report success, then every later load fails.
    LOG_ERR("PHTP", "short token write (%u of %u)", static_cast<unsigned>(written), static_cast<unsigned>(out.size()));
    if (f.isOpen()) f.close();
    Storage.remove(file.c_str());
    return false;
  }
  return true;
}

bool removeTokenFile(const std::string& file) {
  if (file.empty()) return true;
  if (!allowedFile(file)) return false;
  if (!Storage.exists(file.c_str())) return true;
  if (Storage.remove(file.c_str())) return true;
  LOG_ERR("PHTP", "Failed to remove token file: %s", file.c_str());
  return false;
}

void loadConfigFile(const std::string& file, Headers& out) {
  out.clear();
  if (file.empty()) return;
  if (!allowedFile(file)) return;
  // Plugin config is optional and created lazily on the first settings change.
  // A missing file means "use manifest defaults", not an SD read error.
  if (!Storage.exists(file.c_str())) return;
  std::string raw;
  if (!Storage.readFileToString("PHTP", file, MAX_TOKEN_FILE_SIZE, raw)) return;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  const JsonObjectConst fields = doc.as<JsonObjectConst>();
  out.reserve(fields.size());
  for (JsonPairConst kv : fields) {
    out.emplace_back(kv.key().c_str(), variantToString(kv.value()));
  }
}

bool saveConfigValue(const std::string& file, const std::string& key, const std::string& value,
                     const bool booleanValue) {
  if (file.empty() || key.empty() || !allowedFile(file)) return false;
  JsonDocument doc;
  if (Storage.exists(file.c_str())) {
    std::string raw;
    if (!Storage.readFileToString("PHTP", file, MAX_TOKEN_FILE_SIZE, raw) ||
        deserializeJson(doc, raw) != DeserializationError::Ok || !doc.is<JsonObject>()) {
      return false;
    }
  } else {
    doc.to<JsonObject>();
  }
  if (booleanValue)
    doc[key] = value == "true" || value == "1";
  else
    doc[key] = value;
  std::string out;
  serializeJson(doc, out);
  if (out.size() > MAX_TOKEN_FILE_SIZE) return false;
  HalFile f;
  if (!Storage.openFileForWrite("PHTP", file, f)) return false;
  const size_t written = f.write(out.data(), out.size());
  f.flush();
  if (f.isOpen()) f.close();
  if (written != out.size()) {
    LOG_ERR("PHTP", "short config write (%u of %u)", static_cast<unsigned>(written), static_cast<unsigned>(out.size()));
    Storage.remove(file.c_str());
    return false;
  }
  return true;
}

int request(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
            const std::string& body, const Headers& headers, String& out, const size_t maxResponse,
            Headers* responseHeaders, const std::function<bool()>& shouldAbort) {
  if (!networkReady()) {
    LOG_ERR("PHTP", "Network request blocked: WiFi is not connected");
    return -1;
  }
  releaseFontCachesBeforeTls(url);
  WifiPowerSaveGuard psGuard;
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient* httpPtr = openClient(session, tmp, url, headers, shouldAbort);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

  out.remove(0);
  size_t reserved = 0;
  bool overflow = false;
  const int status = http.sendRequest(
      method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, size_t len) {
        if (len > maxResponse - out.length()) {
          overflow = true;
          return false;
        }
        const size_t needed = out.length() + len;
        // Grow geometrically instead of reallocating for every network chunk.
        if (needed > reserved) reserved = std::min(maxResponse, std::max(needed, reserved * 2));
        if (!out.reserve(reserved) || !out.concat(reinterpret_cast<const char*>(data), len)) {
          overflow = true;
          return false;
        }
        return true;
      },
      shouldAbort);
  if (responseHeaders) *responseHeaders = http.getHeaders();
  releaseIdleTlsSessionIfTight(session);
  // Error statuses still return their body: OAuth device-code polling carries
  // its state ("authorization_pending") in 4xx response bodies.
  if (overflow || status < 0 || !http.responseComplete()) {
    LOG_ERR("PHTP", "API request failed: status=%d overflow=%d complete=%d free=%u max=%u %s", status, overflow,
            http.responseComplete(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), url.c_str());
    return -1;
  }
  return status;
}

int requestToFile(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                  const std::string& body, const Headers& headers, const char* destPath, const size_t maxResponse) {
  if (!networkReady()) {
    LOG_ERR("PHTP", "Network request blocked: WiFi is not connected");
    return -1;
  }
  releaseFontCachesBeforeTls(url);
  WifiPowerSaveGuard psGuard;
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient* httpPtr = openClient(session, tmp, url, headers);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

  int status = -1;
  bool writeOk = true;
  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PHTP", destPath, file)) return -1;
    status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                              [&](const uint8_t* data, size_t len) {
                                if (written + len > maxResponse) {
                                  writeOk = false;
                                  return false;
                                }
                                if (file.write(data, len) != len) {
                                  writeOk = false;
                                  return false;
                                }
                                written += len;
                                return true;
                              });
    file.flush();
    // file closed at scope exit, before any Storage.remove of destPath
  }
  releaseIdleTlsSessionIfTight(session);
  if (!writeOk || status < 0 || !http.responseComplete()) {
    LOG_ERR("PHTP", "API request (to file) failed: status=%d writeOk=%d complete=%d free=%u max=%u %s", status, writeOk,
            http.responseComplete(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), url.c_str());
    Storage.remove(destPath);
    return -1;
  }
  return status;
}

bool mintPasswordToken(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                       const std::string& body, const Headers& headers, const std::string& tokenPath,
                       std::string& outToken) {
  String response;
  constexpr size_t MAX_AUTH_RESPONSE = 8 * 1024;
  const int status = request(session, url, method, body, headers, response, MAX_AUTH_RESPONSE);
  if (status < 200 || status >= 300) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) return false;
  outToken = variantToString(resolvePath(doc.as<JsonVariantConst>(), tokenPath));
  return !outToken.empty();
}

}  // namespace pluginhttp
