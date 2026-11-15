#include "LcpUnlock.h"

#include <HalStorage.h>
#include <Logging.h>
#include <StreamingJsonParser.h>
#include <WolfsslCrypto.h>
#include <ZipFile.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "BookKey.h"
#include "LcpDevice.h"
#include "LcpServiceConfig.h"
#include "network/HttpDownloader.h"

namespace lcpunlock {
namespace {

constexpr size_t MAX_LICENSE_BYTES = 256 * 1024;

struct WrappedKey {
  std::string epk, iv, ct, tag;
};

bool parseResponse(const std::string& response, WrappedKey& wrapped, int64_t& expiresAt) {
  struct Context {
    char current[16] = {};
    WrappedKey* key = nullptr;
    int64_t expires = 0;
  } ctx;
  ctx.key = &wrapped;
  JsonCallbacks callbacks = {};
  callbacks.ctx = &ctx;
  callbacks.onKey = [](void* ud, const char* name, size_t len) {
    auto& c = *static_cast<Context*>(ud);
    const size_t n = len < sizeof(c.current) - 1 ? len : sizeof(c.current) - 1;
    memcpy(c.current, name, n);
    c.current[n] = '\0';
  };
  callbacks.onString = [](void* ud, const char* value, size_t len) {
    auto& c = *static_cast<Context*>(ud);
    const size_t n = len < 128 ? len : 128;
    if (strcmp(c.current, "epk") == 0)
      c.key->epk.assign(value, n);
    else if (strcmp(c.current, "iv") == 0)
      c.key->iv.assign(value, n);
    else if (strcmp(c.current, "ct") == 0)
      c.key->ct.assign(value, n);
    else if (strcmp(c.current, "tag") == 0)
      c.key->tag.assign(value, n);
  };
  callbacks.onNumber = [](void* ud, const char* value, size_t) {
    auto& c = *static_cast<Context*>(ud);
    if (strcmp(c.current, "expires") == 0) c.expires = strtoll(value, nullptr, 10);
  };
  callbacks.onBool = [](void*, bool) {};
  callbacks.onNull = [](void*) {};
  callbacks.onObjectStart = [](void*) {};
  callbacks.onObjectEnd = [](void*) {};
  callbacks.onArrayStart = [](void*) {};
  callbacks.onArrayEnd = [](void*) {};
  StreamingJsonParser parser(callbacks);
  parser.feed(response.data(), response.size());
  if (parser.hasError() || wrapped.epk.empty() || wrapped.iv.empty() || wrapped.ct.empty() || wrapped.tag.empty())
    return false;
  expiresAt = ctx.expires > 0 ? ctx.expires : 0;
  return true;
}

bool parseLicense(const std::string& raw, freeink::content::LcpLicense& parsed) {
  if (raw.empty() || raw.size() > MAX_LICENSE_BYTES) return false;
  return freeink::content::parseLcpLicense(raw.data(), raw.size(), &parsed);
}

bool safeId(const std::string& value) {
  if (value.empty() || value.size() > 128) return false;
  for (const unsigned char ch : value) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
  }
  return true;
}

}  // namespace

bool embeddedLicense(const std::string& epubPath, std::string& licenseJson, freeink::content::LcpLicense& parsed) {
  licenseJson.clear();
  ZipFile zip(epubPath);
  size_t size = 0;
  if (!zip.getInflatedFileSize("META-INF/license.lcpl", &size) || size == 0 || size > MAX_LICENSE_BYTES) return false;
  std::unique_ptr<uint8_t, decltype(&free)> bytes(zip.readFileToMemory("META-INF/license.lcpl"), &free);
  if (!bytes) return false;
  licenseJson.assign(reinterpret_cast<const char*>(bytes.get()), size);
  if (parseLicense(licenseJson, parsed)) return true;
  licenseJson.clear();
  return false;
}

bool licenseFile(const std::string& path, std::string& licenseJson, freeink::content::LcpLicense& parsed) {
  licenseJson.clear();
  HalFile file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > MAX_LICENSE_BYTES) return false;
  licenseJson.resize(size);
  if (file.read(reinterpret_cast<uint8_t*>(&licenseJson[0]), size) != size || !parseLicense(licenseJson, parsed)) {
    licenseJson.clear();
    return false;
  }
  return true;
}

std::string userKey(const std::string& passphrase) {
  freeink::content::WolfsslCrypto crypto;
  uint8_t hash[32];
  freeink::content::lcpUserKey(crypto, passphrase.data(), passphrase.size(), hash);
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  char encoded[65];
  for (size_t i = 0; i < sizeof(hash); ++i) {
    encoded[i * 2] = HEX_DIGITS[hash[i] >> 4];
    encoded[i * 2 + 1] = HEX_DIGITS[hash[i] & 0x0f];
  }
  encoded[64] = '\0';
  return std::string(encoded, 64);
}

Result unlock(const std::string& epubPath, const std::string& licenseJson, const std::string& userKeyHex) {
  if (userKeyHex.size() != 64 || licenseJson.empty() || licenseJson.size() > MAX_LICENSE_BYTES) return Result::Failed;
  for (const unsigned char ch : userKeyHex)
    if (!std::isxdigit(ch)) return Result::Failed;
  std::string deviceId;
  bool notEnrollable = false;
  if (!lcpdevice::ensureEnrolled(deviceId, notEnrollable)) {
    return notEnrollable ? Result::NotEnrollable : Result::Failed;
  }

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!safeId(deviceId)) return Result::Failed;
    std::string request;
    request.reserve(licenseJson.size() + 160);
    request += "{\"device_id\":\"";
    request += deviceId;
    request += "\",\"user_key\":\"";
    request += userKeyHex;
    request += "\",\"license\":";
    request += licenseJson;
    request += '}';

    std::string response;
    int status = 0;
    const bool ok = HttpDownloader::postForm(lcpservice::UNLOCK_URL, request, response, &status);
    if (status == 401 && attempt == 0) {
      lcpdevice::forget();
      if (!lcpdevice::ensureEnrolled(deviceId, notEnrollable))
        return notEnrollable ? Result::NotEnrollable : Result::Failed;
      continue;
    }
    if (status == 403) return Result::WrongPassphrase;
    if (status == 422) return Result::Unsupported;
    WrappedKey wrapped;
    int64_t expires = 0;
    uint8_t contentKey[32] = {};
    if (!ok || !parseResponse(response, wrapped, expires) ||
        !lcpdevice::unwrapContentKey(wrapped.epk, wrapped.iv, wrapped.ct, wrapped.tag, contentKey)) {
      LOG_ERR("LCP", "LCP unlock failed (status %d)", status);
      return Result::Failed;
    }
    const bool saved = bookkey::write(epubPath, contentKey, sizeof(contentKey), expires);
    memset(contentKey, 0, sizeof(contentKey));
    return saved ? Result::Ok : Result::Failed;
  }
  return Result::Failed;
}

}  // namespace lcpunlock
