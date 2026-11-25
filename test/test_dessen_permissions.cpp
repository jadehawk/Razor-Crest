#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/activities/plugins/DessenPermissions.h"

int main(int argc, char** argv) {
  using Access = DessenPermissions::Access;
  const std::string manifest = R"({
    "permissions": {
      "filesystem": {
        "app_directory":["read","write","delete","list"],
        "read":["/.crosspoint/reading-stats/**","/Books/**"],
        "write":["/exports/report.txt"],
        "delete":["/exports/old.txt"],
        "list":["/Books"]
      },
      "credentials":{"shared":["/.crosspoint/shared-token.json"]},
      "network":{"origins":["https://example.com"],"handoff":true}
    }
  })";
  DessenPermissions p;
  assert(p.parse(R"({"permissions":{}})", "/.crosspoint/plugins/first"));
  assert(p.parse(R"({"permissions":{"filesystem":{"app_directory":["read","write","delete","list"]}}})",
                 "/.crosspoint/plugins/first"));
  assert(p.parse(manifest, "/.crosspoint/plugins/first"));
  assert(!p.allows("/.crosspoint/plugins/first/cache", Access::Read));
  assert(!p.allowsUrl("https://example.com/"));
  p.authorize();
  assert(p.ready());
  assert(p.allows("/.CROSSPOINT/PLUGINS/FIRST/cache", Access::Write));
  assert(p.allows("/Books/Book.epub", Access::Read));
  assert(!p.allows("/Books/Book.epub", Access::Write));
  assert(p.allows("/exports/report.txt", Access::Write));
  assert(!p.allows("/exports/report.txt", Access::Delete));
  assert(p.allows("/exports/old.txt", Access::Delete));
  assert(!p.allows("/exports/old.txt", Access::Write));
  assert(p.allows("/Books", Access::List));
  assert(!p.allows("/Books", Access::Write));
  assert(p.isSharedCredential("/.CROSSPOINT/shared-token.json"));
  assert(!p.isSharedCredential("/Books/a"));
  assert(p.allows("/.crosspoint/shared-token.json", Access::Read));
  assert(p.allows("/.crosspoint/shared-token.json", Access::Delete));
  assert(!p.allows("/.crosspoint/shared-token.json.bak", Access::Read));
  assert(!p.allows("/.crosspoint/plugins/second/cache", Access::Read));
  for (auto file : {"app.bin", "manifest.json", "device.json"}) {
    assert(!p.allows(std::string("/.crosspoint/plugins/first/") + file, Access::Write));
    assert(!p.allows(std::string("/.crosspoint/plugins/first/") + file, Access::Delete));
  }
  for (auto path : {"/Books/../.crosspoint/wifi.json", "/Books//file", "/Books/./file", "/Books/file.", "/Books/ file",
                    "/Books/file ", "/BOOKS~1/file", "/Books/file/", "/Books/file:stream", "/Books/a\\file",
                    "/Books/a?file", "/.crosspoint/wifi.json", "/.crosspoint/opds_tokens.json",
                    "/.crosspoint/lcp_passphrases.json", "/.crosspoint/dsn-state/file"}) {
    assert(!p.allows(path, Access::Read));
  }
  assert(p.allowsUrl("https://example.com/a?b=c"));
  assert(p.allowsUrl("https://EXAMPLE.COM:443/a"));
  for (auto url :
       {"http://example.com/", "https://example.com.evil/", "https://example.com@evil/", "https://evil@example.com/",
        "https://example.com:444/", "https://example.com./", "https://example.com\\@evil/",
        "https://example.com/#token", "https://example.com/\nInjected: value", "//example.com/"})
    assert(!p.allowsUrl(url));
  const auto bad = [&](const std::string& body) {
    DessenPermissions other;
    assert(!other.parse(body, "/.crosspoint/plugins/second"));
    other.authorize();
    assert(!other.ready());
    assert(!other.allows("/Books/a", Access::Read));
  };
  bad("{}");
  bad(R"({"permissions":{"unknown":true}})");
  bad(R"({"permissions":{"filesystem":{"read":["/.crosspoint/plugins/first/**"]}}})");
  bad(R"({"permissions":{"filesystem":{"read":["/.crosspoint/**"]}}})");
  bad(R"({"permissions":{"filesystem":{"read":["/.crosspoint/wifi.json"]}}})");
  bad(R"({"permissions":{"credentials":{"shared":["/.crosspoint/opds_tokens.json"]}}})");
  bad(R"({"permissions":{"filesystem":{"read":["/.crosspoint/shared-token.json"]}}})");
  bad(R"({"permissions":{"filesystem":{"read":["/.crosspoint/shared-token.json.bak"]}}})");
  bad(R"({"permissions":{"credentials":{"shared":["/exports/token.json"]}}})");
  bad(R"({"permissions":{"network":{"origins":["https://*.example.com"]}}})");
  bad(R"({"permissions":{"network":{"origins":["https://example.com"],"handoff":"yes"}}})");
  bad(std::string(4097, ' '));
  bad(std::string("{\"permissions\":") + std::string(100, '[') + "0" + std::string(100, ']') + "}");
  assert(p.parse(R"({"permissions":{}})", "/.crosspoint/plugins/first"));
  p.authorize();
  assert(!p.allows("/Books/a", Access::Read));
  assert(!p.allowsUrl("https://example.com/"));
  assert(!p.allowsLibrary());
  assert(!p.allowsBook("/private/novel.epub"));
  assert(p.parse(R"({"permissions":{"library":true}})", "/plugins/reader"));
  p.authorize();
  assert(p.allowsLibrary());
  DessenPermissions local;
  assert(local.parse(R"({"permissions":{"network":{"origins":["http://192.168.1.2:8080"]}}})", "/plugins/local"));
  local.authorize();
  assert(local.allowsUrl("http://192.168.1.2:8080/files"));
  assert(!local.allowsUrl("https://192.168.1.2:8080/files"));
  assert(!local.allowsUrl("http://192.168.1.2:8081/files"));
  assert(p.allowsBook("/private/novel.epub"));
  assert(!p.allowsBook("/.crosspoint/wifi.json"));
  assert(!p.allowsBook("/.crosspoint/plugins/other/secret.epub"));
  assert(!p.allows("/private/novel.epub", Access::Read));
  if (argc > 1) {
    std::ifstream file(argv[1]);
    assert(file.good());
    std::string manifest((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DessenPermissions installed;
    assert(installed.parse(manifest, "/.crosspoint/plugins/reading-statistics"));
    installed.authorize();
    assert(installed.allows("/.crosspoint/reading_sessions.jsonl", Access::Read));
    assert(!installed.allows("/.crosspoint/reading_sessions.jsonl", Access::Write));
    assert(installed.allows("/.crosspoint/reading-statistics-token.json", Access::Delete));
    assert(installed.allowsUrl("https://sync.techy-notes.com/api/device/code"));
    assert(!installed.allowsUrl("http://sync.techy-notes.com/api/device/code"));
    assert(installed.allowsLibrary() && installed.allowsHandoff());
  }
}
