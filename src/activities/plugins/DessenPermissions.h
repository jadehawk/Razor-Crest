#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// Installation authorizes validated manifest scopes without interactive prompts.
class DessenPermissions {
 public:
  enum class Access : uint8_t { Read = 1, Write = 2, Delete = 4, List = 8, BookKey = 16 };
  static constexpr size_t MAX_RULES = 16;
  static constexpr size_t MAX_ORIGINS = 8;
  static constexpr size_t MAX_MANIFEST = 4096;
  bool parse(std::string_view manifest, std::string_view appDirectory);
  void authorize() { authorized = valid; }
  bool ready() const { return valid && authorized; }
  bool allows(std::string_view path, Access access) const;
  bool allowsUrl(std::string_view url) const;
  bool isSharedCredential(std::string_view path) const;
  bool allowsLibrary() const { return ready() && library; }
  bool allowsBook(std::string_view path) const;
  bool allowsNetwork() const { return ready() && originCount != 0; }
  bool allowsHandoff() const { return allowsNetwork() && handoff; }
  bool allowsDeviceIdentity(std::string_view url) const;
  bool allowsHistoryIdentity() const { return ready() && historyIdentity; }
  static bool canonical(std::string_view path, std::string& out);

 private:
  struct Rule {
    std::string path;
    uint8_t access = 0;
    bool tree = false;
    bool credential = false;
  };
  bool addRule(std::string_view path, uint8_t access, bool credential = false);
  static bool within(std::string_view path, std::string_view directory);
  static bool reserved(std::string_view path);
  std::array<Rule, MAX_RULES> rules;
  std::array<std::string, MAX_ORIGINS> origins;
  std::string appDir;
  size_t ruleCount = 0, originCount = 0;
  bool handoff = false, deviceIdentity = false, historyIdentity = false, library = false, valid = false,
       authorized = false;
};
