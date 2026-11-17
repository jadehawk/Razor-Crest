#include "components/home/HomeLayoutJson.h"
#include "components/home/HomeLayoutSession.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace home_grid::json;

namespace {
struct Reader {
  const std::string& input;
  size_t offset = 0;
  int read() {
    return offset < input.size() ? static_cast<unsigned char>(input[offset++]) : -1;
  }
  size_t readBytes(char* destination, size_t count) {
    const size_t remaining = input.size() - offset;
    const size_t amount = std::min(count, remaining);
    std::memcpy(destination, input.data() + offset, amount);
    offset += amount;
    return amount;
  }
};

struct FakeHalFile {
  Reader memory;
  uint64_t claimedSize;

  explicit FakeHalFile(const std::string& bytes, uint64_t declared = 0)
      : memory{bytes}, claimedSize(declared ? declared : bytes.size()) {}
  uint64_t fileSize64() const { return claimedSize; }
  int read() { return memory.read(); }
  int read(void* buffer, size_t bytes) {
    return static_cast<int>(memory.readBytes(static_cast<char*>(buffer), bytes));
  }
};

std::string read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint16_t scene(void*, const char* name) {
  if (std::strcmp(name, "oversized.hero") == 0) return 2; // Invalid-geometry fixture.
  const char* known[] = {"halo.hero", "snoopy.hero", "snoopy.footer", "mixed.hero"};
  for (uint16_t i = 0; i < 4; ++i)
    if (std::strcmp(known[i], name) == 0) return static_cast<uint16_t>(i + 2);
  return 0;
}
uint16_t bitmap(void*, const char* name) {
  return std::strcmp(name, "home_background") == 0 ? 1 : 0;
}
uint16_t provider(void*, const char* plugin, const char* widget) {
  return std::strcmp(plugin, "reading-statistics") == 0 &&
                 std::strcmp(widget, "reading_snapshot") == 0 ? 7 : 0;
}
Resources resources() {
  Resources result{};
  result.scene = scene;
  result.bitmap = bitmap;
  result.provider = provider;
  result.preferredMenuHeight = 265;
  return result;
}

void testLayouts(const char* path, int rows, int widgets) {
  std::string input = read(path);
  Reader reader{input};
  Parsed parsed;
  const auto result = parseStream(reader, input.size(), resources(), parsed);
  if (result != Status::Ok) {
    std::cerr << "Failed fixture " << path << " status " << static_cast<int>(result) << "\n";
  }
  assert(result == Status::Ok);
  assert(parsed.layout.rowCount == rows);
  assert(parsed.layout.columnCount == widgets);
  assert(parsed.widgets.widgetCount == widgets);
  assert(parsed.widgets.widgets[0].kind == home_widgets::Kind::Header);
  home_grid::activation::Active active;
  auto policy = home_grid::activation::Policy{true, true, true, true};
  assert(home_grid::activation::prepare(parsed.layout, parsed.widgets,
      home_grid::Screen{528, 792, 14, 14, 0, 40}, policy, active) ==
      home_grid::activation::Status::Ready);
  // Picco touch safe areas are caller-owned, never inferred from a theme.
  assert(home_grid::activation::prepare(parsed.layout, parsed.widgets,
      home_grid::Screen{480, 800, 14, 14, 0, 8}, policy, active) ==
      home_grid::activation::Status::Ready);
}

std::string replaceOnce(std::string str, const std::string& target, const std::string& replacement);

void testKnownFixtures() {
  testLayouts("test/fixtures/home_layout_v1/halo.json", 4, 4);
  testLayouts("test/fixtures/home_layout_v1/snoopy.json", 5, 5);
  testLayouts("test/fixtures/home_layout_v1/mixed.json", 5, 8);
  testLayouts("test/fixtures/home_layout_v1/missing_plugin.json", 3, 3);

  auto parsed = Parsed{};
  std::string input = read("test/fixtures/home_layout_v1/halo.json");
  Reader r{input};
  assert(parseStream(r, input.size(), resources(), parsed) == Status::Ok);
  assert(parsed.widgets.underlayCount == 1);
  assert(parsed.widgets.underlays[0].scene == 2);
  assert(parsed.widgets.underlays[0].fromRow == 1);
  assert(parsed.widgets.underlays[0].throughRow == 2);
  assert(parsed.layout.rows[3].mode == home_grid::RowMode::Content);
  assert(parsed.layout.rows[3].preferredHeight == 265);
  assert(parsed.menuRowHeight == 46);

  std::string missing = read("test/fixtures/home_layout_v1/missing_plugin.json");
  Reader pm{missing};
  assert(parseStream(pm, missing.size(), resources(), parsed) == Status::Ok);
  assert(parsed.widgets.widgets[1].kind == home_widgets::Kind::Plugin);
  assert(parsed.widgets.widgets[1].resource == 0);
  assert(parsed.widgets.widgets[1].fallback == home_widgets::Kind::Empty);

  std::string transparent = replaceOnce(missing, "\"type\": \"empty\"",
                                         "\"type\": \"empty\", \"panel\": \"transparent\"");
  Reader fallbackReader{transparent};
  assert(parseStream(fallbackReader, transparent.size(), resources(), parsed) == Status::Ok);
  assert(parsed.widgets.widgets[1].fallbackPanel ==
         static_cast<uint8_t>(home_widgets::Panel::Transparent));

  std::string impossible = read("test/fixtures/home_layout_v1/invalid_minima.json");
  Reader bad{impossible};
  const auto decoded = parseStream(bad, impossible.size(), resources(), parsed);
  if (decoded == Status::Ok) {
    home_grid::activation::Active active;
    assert(home_grid::activation::prepare(parsed.layout, parsed.widgets,
        home_grid::Screen{528, 792, 14, 14, 0, 40},
        {true,true,true,true},active) != home_grid::activation::Status::Ready);
  } else {
    assert(decoded == Status::InvalidLayout || decoded == Status::InvalidSchema);
  }
}

std::string replaceOnce(std::string str, const std::string& target, const std::string& replacement) {
  const auto index = str.find(target);
  assert(index != std::string::npos);
  str.replace(index, target.size(), replacement);
  return str;
}
void reject(std::string json, Status expected) {
  Reader input{json};
  Parsed parsed;
  parsed.widgets.widgetCount = 10;
  const auto status = parseStream(input, json.size(), resources(), parsed);
  if (status != expected) {
    std::cerr << "Expected " << static_cast<int>(expected) << " received "
              << static_cast<int>(status) << "\n";
  }
  assert(status == expected);
  assert(parsed.widgets.widgetCount == 0); // Never retain a partial plan.
}

void testStrictSchemaAndResourceValidation() {
  const std::string halo = read("test/fixtures/home_layout_v1/halo.json");
  reject(replaceOnce(halo, "\"schema_version\": 1",
                     "\"schema_version\": 1, \"schema_version\": 1"),
         Status::InvalidJson); // Raw object duplicates are rejected, not overwritten.
  reject(replaceOnce(halo, "\"schema_version\": 1", "\"schema_version\": null"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"row_gap\": 0", "\"row_gap\": null"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"background\": {\n    \"mode\": \"none\"\n  }",
                     "\"background\": null"), Status::InvalidSchema);
  reject(replaceOnce(halo, "\"panel\": \"opaque\"", "\"panel\": null"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"schema_version\": 1", "\"schema_version\": 2"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"weight\": 4", "\"weight\": 99"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"halo.hero\"", "\"../bad\""),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"halo.hero\"", "\"unregistered\""),
         Status::MissingResource);
  reject(replaceOnce(halo, "\"type\": \"native.header\"", "\"type\": \"native.clock\""),
         Status::InvalidLayout);
  reject(replaceOnce(halo, "\"panel\": \"transparent\"", "\"panel\": \"rgb\""),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"home_shortcuts\"", "\"external_script\""),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"row_height\": 46", "\"row_height\": 500"),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"id\": \"hero\"", "\"id\": \"header\""),
         Status::InvalidSchema);
  reject(replaceOnce(halo, "\"id\": \"hero\"", "\"id\": \"hero\", \"not_supported\": true"),
         Status::InvalidSchema);

  const std::string mixed = read("test/fixtures/home_layout_v1/mixed.json");
  reject(replaceOnce(mixed, "\"home_background\"", "\"not_declared\""),
         Status::MissingResource);
  reject(replaceOnce(mixed, "\"active_days\"", "\"secret_stat\""),
         Status::InvalidSchema);
  reject(replaceOnce(mixed, "\"focus\": [", "\"focus\": [105,"),
         Status::InvalidSchema);
}

void testHalFileReader() {
  const std::string halo = read("test/fixtures/home_layout_v1/halo.json");
  FakeHalFile file{halo};
  Parsed output;
  assert(parseFile(file, resources(), output) == Status::Ok);
  assert(output.layout.rowCount == 4 && output.widgets.underlayCount == 1);

  FakeHalFile oversized{halo, kMaxLayoutBytes + 1};
  assert(parseFile(oversized, resources(), output) == Status::TooLarge);
  assert(output.widgets.widgetCount == 0);
  FakeHalFile truncated{halo, halo.size() - 3};
  assert(parseFile(truncated, resources(), output) == Status::InvalidJson);
  assert(output.widgets.widgetCount == 0);
  FakeHalFile shortRead{halo, halo.size() + 5};
  assert(parseFile(shortRead, resources(), output) == Status::InvalidJson);
  assert(output.widgets.widgetCount == 0);
}

void testColdPathSession() {
  const std::string halo = read("test/fixtures/home_layout_v1/halo.json");
  home_grid::session::State state{};
  const home_grid::activation::Policy policy{true, true, true, true};
  FakeHalFile complete{halo};
  assert(home_grid::session::load(complete, resources(),
                                 home_grid::Screen{528, 792, 14, 14, 0, 40},
                                 policy, state) == Status::Ok);
  assert(state.active.enabled);
  assert(state.parsed.widgets.widgetCount == 4);

  // A failed subsequent load must wipe *all* previously activated geometry
  // and descriptors, not silently keep the old theme's scene pointers.
  FakeHalFile untrusted{halo};
  home_grid::activation::Policy denied = policy;
  denied.supportsWidgets = false;
  assert(home_grid::session::load(untrusted, resources(),
                                 home_grid::Screen{528, 792}, denied, state) ==
         Status::InvalidLayout);
  assert(!state.active.enabled && state.parsed.widgets.widgetCount == 0);
  assert(state.parsed.layout.rowCount == 0);
  assert(untrusted.memory.offset == 0); // Disabled capability must not read SD.

  FakeHalFile tooLarge{halo, kMaxLayoutBytes + 1};
  assert(home_grid::session::load(tooLarge, resources(),
                                 home_grid::Screen{528, 792}, policy, state) ==
         Status::TooLarge);
  assert(!state.active.enabled && state.parsed.widgets.widgetCount == 0);

  FakeHalFile smallScreen{halo};
  assert(home_grid::session::load(smallScreen, resources(),
                                 home_grid::Screen{40, 40}, policy, state) ==
         Status::InvalidLayout);
  assert(!state.active.enabled && state.parsed.layout.rowCount == 0);
}

void testByteCapAndGarbage() {
  const std::string json = read("test/fixtures/home_layout_v1/snoopy.json");
  reject(json + " malicious", Status::InvalidJson);
  reject(json.substr(0, json.size()-4), Status::InvalidJson);
  Reader tooLarge{json};
  Parsed parsed;
  assert(parseStream(tooLarge, kMaxLayoutBytes+1, resources(), parsed) == Status::TooLarge);
  assert(!parsed.widgets.widgetCount);

  // A claimed short read never admits a partial document.
  Reader tooShort{json};
  assert(parseStream(tooShort, json.size()-5, resources(), parsed) == Status::InvalidJson);
  assert(!parsed.widgets.widgetCount);
  Reader tooLong{json};
  assert(parseStream(tooLong, json.size()+5, resources(), parsed) == Status::InvalidJson);
  assert(!parsed.widgets.widgetCount);
}
}

int main() {
  testKnownFixtures();
  testStrictSchemaAndResourceValidation();
  testHalFileReader();
  testColdPathSession();
  testByteCapAndGarbage();
}
