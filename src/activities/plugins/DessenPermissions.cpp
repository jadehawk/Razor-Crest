#if CROSSPOINT_DESSEN_APPS
#include "DessenPermissions.h"

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {
// ArduinoJson owns these allocations; the parser cannot grow past 12 KiB.
class ManifestAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    if (size > LIMIT - used) return nullptr;
    auto* block = static_cast<Block*>(std::malloc(sizeof(Block) + size));
    if (!block) return nullptr;
    block->size = size;
    used += size;
    return block + 1;
  }
  void deallocate(void* pointer) override {
    if (!pointer) return;
    auto* block = static_cast<Block*>(pointer) - 1;
    used -= block->size;
    std::free(block);
  }
  void* reallocate(void* pointer, size_t size) override {
    if (!pointer) return allocate(size);
    auto* block = static_cast<Block*>(pointer) - 1;
    const size_t old = block->size;
    if (size > LIMIT - (used - old)) return nullptr;
    auto* next = static_cast<Block*>(std::realloc(block, sizeof(Block) + size));
    if (!next) return nullptr;
    next->size = size;
    used = used - old + size;
    return next + 1;
  }

 private:
  struct alignas(std::max_align_t) Block {
    size_t size;
  };
  static constexpr size_t LIMIT = 12288;
  size_t used = 0;
};

bool keys(JsonObjectConst object, std::string_view allowed) {
  for (JsonPairConst pair : object) {
    const std::string key = "|" + std::string(pair.key().c_str()) + "|";
    if (allowed.find(key) == std::string_view::npos) return false;
  }
  return true;
}

bool jsonStore(std::string_view path) {
  const size_t marker = path.find(".json");
  return marker != path.npos && (marker + 5 == path.size() || path[marker + 5] == '.');
}

bool hostValid(std::string_view host) {
  if (host.empty() || host.size() > 128 || host.front() == '.' || host.back() == '.') return false;
  for (char c : host)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
  return host.find("..") == std::string_view::npos;
}

bool originOf(std::string_view url, std::string& result, bool originOnly) {
  const bool secure = url.substr(0, 8) == "https://";
  const size_t start = secure ? 8 : url.substr(0, 7) == "http://" ? 7 : 0;
  if (!start || url.size() > 2048) return false;
  for (unsigned char c : url)
    if (c <= 32 || c >= 127 || c == '\\' || c == '#') return false;
  const size_t end = url.find_first_of("/?", start);
  if (originOnly && end != url.npos) return false;
  auto authority = url.substr(start, end == url.npos ? end : end - start);
  const size_t colon = authority.find(':');
  const auto hostname = authority.substr(0, colon);
  std::string host(hostname);
  for (char& c : host)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if (!hostValid(host)) return false;
  uint32_t port = secure ? 443 : 80;
  if (colon != authority.npos) {
    const auto number = authority.substr(colon + 1);
    if (number.empty() || number.size() > 5 || number.front() == '0') return false;
    port = 0;
    for (char c : number) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + c - '0';
    }
    if (!port || port > 65535) return false;
  }
  result = (secure ? "https://" : "http://") + host + ":" + std::to_string(port);
  return true;
}
}  // namespace

bool DessenPermissions::canonical(std::string_view path, std::string& out) {
  out.clear();
  if (path.empty() || path.size() > 512 || path.front() != '/' || path.find('\0') != path.npos) return false;
  if (path == "/") {
    out = "/";
    return true;
  }
  for (size_t start = 1; start < path.size();) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == path.npos ? path.size() : slash;
    const auto component = path.substr(start, end - start);
    if (component.empty() || component.size() > 255 || component.front() == ' ' || component.back() == ' ' ||
        component.back() == '.' || component == "." || component == "..")
      return false;
    for (unsigned char c : component) {
      // All 8.3 aliases are denied, including intermediate directory aliases.
      if (c < 32 || c == 127 || std::strchr("\\:~*?\"<>|", c)) return false;
      out += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
    }
    if (slash == path.npos) break;
    if (slash + 1 == path.size()) return false;
    out += '/';
    start = slash + 1;
  }
  out.insert(out.begin(), '/');
  return true;
}

bool DessenPermissions::within(std::string_view path, std::string_view directory) {
  return path == directory || (path.size() > directory.size() && path.substr(0, directory.size()) == directory &&
                               path[directory.size()] == '/');
}

bool DessenPermissions::reserved(std::string_view path) {
  constexpr std::string_view protectedPrefixes[] = {
      "/.crosspoint/wifi.json", "/.crosspoint/opds.json",   "/.crosspoint/koreader.json",
      "/.crosspoint/lcp",       "/.crosspoint/opds_tokens", "/.crosspoint/device",
      "/.crosspoint/dsn"};
  for (auto prefix : protectedPrefixes)
    if (path.substr(0, prefix.size()) == prefix) return true;
  return false;
}

bool DessenPermissions::addRule(std::string_view path, uint8_t access, bool credential) {
  if (ruleCount == MAX_RULES || !access) return false;
  bool tree = path.size() > 3 && path.substr(path.size() - 3) == "/**";
  if (tree) path.remove_suffix(3);
  std::string normalized;
  if (!canonical(path, normalized) || normalized == "/" || normalized.size() > 190 || reserved(normalized))
    return false;
  for (auto root : {"/.crosspoint/plugins", "/plugins", "/.plugins"}) {
    if (within(normalized, root) && !within(normalized, appDir)) return false;
    if (within(root, normalized)) return false;
  }
  // Root-level firmware JSON stores require an explicit credential grant.
  if (within(normalized, "/.crosspoint") && normalized.find('/', 13) == normalized.npos && jsonStore(normalized) &&
      !credential)
    return false;
  if (credential && (tree || !within(normalized, "/.crosspoint") || normalized.find('/', 13) != normalized.npos ||
                     !jsonStore(normalized)))
    return false;
  rules[ruleCount++] = {std::move(normalized), access, tree, credential};
  return true;
}

bool DessenPermissions::parse(std::string_view manifest, std::string_view appDirectory) {
  valid = authorized = handoff = deviceIdentity = historyIdentity = library = false;
  ruleCount = originCount = 0;
  if (manifest.empty() || manifest.size() > MAX_MANIFEST || !canonical(appDirectory, appDir) || appDir == "/")
    return false;
  bool installed = false;
  for (std::string_view root : {"/.crosspoint/plugins/", "/plugins/", "/.plugins/"}) {
    if (appDir.size() > root.size() && appDir.compare(0, root.size(), root) == 0 &&
        appDir.find('/', root.size()) == appDir.npos)
      installed = true;
  }
  if (!installed) return false;
  ManifestAllocator allocator;
  JsonDocument document(&allocator);
  if (deserializeJson(document, manifest.data(), manifest.size(), DeserializationOption::NestingLimit(6)) ||
      document.overflowed() || !document.is<JsonObjectConst>())
    return false;
  const auto permissionValue = document["permissions"];
  if (!permissionValue.is<JsonObjectConst>()) return false;
  const auto permissions = permissionValue.as<JsonObjectConst>();
  if (!keys(permissions, "|filesystem||network||credentials||library|")) return false;
  if (!permissions["library"].isNull() && !permissions["library"].is<bool>()) return false;
  library = permissions["library"] | false;
  if (!permissions["filesystem"].isNull()) {
    if (!permissions["filesystem"].is<JsonObjectConst>()) return false;
    const auto fs = permissions["filesystem"].as<JsonObjectConst>();
    if (!keys(fs, "|app_directory||read||write||delete||list||book_key|")) return false;
    uint8_t own = 0;
    if (!fs["app_directory"].isNull()) {
      if (!fs["app_directory"].is<JsonArrayConst>() || fs["app_directory"].size() > 4) return false;
      for (JsonVariantConst item : fs["app_directory"].as<JsonArrayConst>()) {
        if (!item.is<const char*>()) return false;
        const std::string_view operation = item.as<const char*>();
        if (operation == "read")
          own |= 1;
        else if (operation == "write")
          own |= 2;
        else if (operation == "delete")
          own |= 4;
        else if (operation == "list")
          own |= 8;
        else
          return false;
      }
      if (own && !addRule(appDir + "/**", own)) return false;
    }
    struct Operation {
      const char* key;
      uint8_t mask;
    };
    constexpr Operation operations[] = {{"read", 1}, {"write", 2}, {"delete", 4}, {"list", 8}, {"book_key", 16}};
    for (const auto& operation : operations) {
      if (fs[operation.key].isNull()) continue;
      if (!fs[operation.key].is<JsonArrayConst>() || fs[operation.key].size() > MAX_RULES) return false;
      for (JsonVariantConst path : fs[operation.key].as<JsonArrayConst>()) {
        if (!path.is<const char*>() || !addRule(path.as<const char*>(), operation.mask)) return false;
      }
    }
  }
  if (!permissions["credentials"].isNull()) {
    if (!permissions["credentials"].is<JsonObjectConst>()) return false;
    const auto credentials = permissions["credentials"].as<JsonObjectConst>();
    if (!keys(credentials, "|shared|")) return false;
    if (!credentials["shared"].is<JsonArrayConst>() || credentials["shared"].size() > 4) return false;
    for (JsonVariantConst path : credentials["shared"].as<JsonArrayConst>()) {
      if (!path.is<const char*>() || !addRule(path.as<const char*>(), 7, true)) return false;
    }
  }
  if (!permissions["network"].isNull()) {
    if (!permissions["network"].is<JsonObjectConst>()) return false;
    const auto network = permissions["network"].as<JsonObjectConst>();
    if (!keys(network, "|origins||handoff||device_identity|") || !network["origins"].is<JsonArrayConst>() ||
        network["origins"].size() > MAX_ORIGINS)
      return false;
    for (JsonVariantConst value : network["origins"].as<JsonArrayConst>()) {
      if (!value.is<const char*>()) return false;
      std::string allowed;
      if (!originOf(value.as<const char*>(), allowed, true)) return false;
      origins[originCount++] = std::move(allowed);
    }
    if (!network["handoff"].isNull() && !network["handoff"].is<bool>()) return false;
    handoff = network["handoff"] | false;
    if (!network["device_identity"].isNull() && !network["device_identity"].is<bool>()) return false;
    deviceIdentity = network["device_identity"] | false;
    if (handoff && !originCount) return false;
  }
  // Optional grants live outside the strict required permission schema, so
  // older hosts can load the same manifest without granting new capabilities.
  if (!document["optional_permissions"].isUnbound()) {
    const auto optional = document["optional_permissions"];
    if (!optional.is<JsonObjectConst>()) return false;
    const auto grants = optional.as<JsonObjectConst>();
    if (!grants["history_identity"].isUnbound()) {
      if (!grants["history_identity"].is<bool>()) return false;
      historyIdentity = grants["history_identity"].as<bool>();
    }
    if (!grants["device_identity"].isUnbound()) {
      if (!grants["device_identity"].is<bool>() || !permissions["network"]["device_identity"].isUnbound()) return false;
      deviceIdentity = grants["device_identity"].as<bool>();
    }
  }
  if (deviceIdentity) {
    if (!originCount) return false;
    for (size_t i = 0; i < originCount; ++i)
      if (!origins[i].starts_with("https://")) return false;
  }
  valid = true;
  return true;
}

bool DessenPermissions::allows(std::string_view path, Access access) const {
  std::string normalized;
  if (!ready() || !canonical(path, normalized) || reserved(normalized)) return false;
  // Executable and permission declarations are immutable to the running guest.
  if (access != Access::Read && access != Access::List &&
      (normalized == appDir || normalized == appDir + "/app.bin" || normalized == appDir + "/manifest.json" ||
       normalized == appDir + "/device.json"))
    return false;
  for (size_t i = 0; i < ruleCount; ++i) {
    const auto& rule = rules[i];
    if (!(rule.access & static_cast<uint8_t>(access))) continue;
    if (rule.tree ? within(normalized, rule.path) : normalized == rule.path) return true;
  }
  return false;
}

bool DessenPermissions::allowsBook(std::string_view path) const {
  if (allows(path, Access::Read)) return true;
  std::string normalized;
  if (!allowsLibrary() || !canonical(path, normalized) || reserved(normalized)) return false;
  for (auto root : {"/.crosspoint", "/plugins", "/.plugins"})
    if (within(normalized, root)) return false;
  for (std::string_view extension : {".epub", ".xtc", ".xtch", ".txt", ".fb2", ".html", ".htm", ".md"})
    if (normalized.size() > extension.size() && normalized.substr(normalized.size() - extension.size()) == extension)
      return true;
  return false;
}

bool DessenPermissions::isSharedCredential(std::string_view path) const {
  std::string normalized;
  if (!ready() || !canonical(path, normalized)) return false;
  for (size_t i = 0; i < ruleCount; ++i)
    if (rules[i].credential && rules[i].path == normalized) return true;
  return false;
}

bool DessenPermissions::allowsUrl(std::string_view url) const {
  std::string origin;
  if (!allowsNetwork() || !originOf(url, origin, false)) return false;
  for (size_t i = 0; i < originCount; ++i)
    if (origin == origins[i]) return true;
  return false;
}

bool DessenPermissions::allowsDeviceIdentity(std::string_view url) const {
  return deviceIdentity && url.starts_with("https://") && allowsUrl(url);
}

#endif  // CROSSPOINT_DESSEN_APPS
