#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "network/LocalSendHistory.h"

static int checks = 0;
static void require(bool ok, const char* label) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
  }
}

int main() {
  using localsend::History;
  History history;
  History::Entry entry{};
  require(history.count() == 0 && !history.read(0, entry), "new history is empty");
  testFiles["/Books/LocalSend/protected.epub"] = {1, 2, 3};
  for (int i = 0; i < 64; ++i) {
    const auto name = "book-" + std::to_string(i) + ".epub";
    require(history.append("/Books/LocalSend", name.c_str()), "append SD history entry");
  }
  require(history.count() == 64, "all 64 records persist beyond prior eight-entry cap");
  require(history.read(0, entry) && !std::strcmp(entry.name, "book-63.epub"), "newest first");
  require(history.read(63, entry) && !std::strcmp(entry.name, "book-0.epub"), "oldest still accessible");
  History::Entry window[16]{};
  require(history.readWindow(20, window, 16) == 16 && !std::strcmp(window[0].name, "book-43.epub") &&
              !std::strcmp(window[15].name, "book-28.epub"),
          "random-access window after many entries");
  require(history.readWindow(62, window, 16) == 2 && history.readWindow(64, window, 16) == 0,
          "last partial window and end of history");
  require(!history.read(-1, entry) && !history.eraseAt(64), "out-of-bounds reads/deletes rejected");
  require(history.eraseAt(20) && history.count() == 63, "remove one arbitrary row");
  require(history.read(20, entry) && !std::strcmp(entry.name, "book-42.epub"), "newest ordering after removal");
  require(testFiles.count("/Books/LocalSend/protected.epub") == 1, "history deletion does not remove saved book");
  require(history.append("/", "root.epub") && history.append("/", "root.epub") && history.count() == 65,
          "duplicate history records accepted as separate transfers");
  require(history.erasePath("/root.epub") == 2 && history.count() == 63, "remove all matching paths only");
  require(history.erasePath("/root.epub") == 0, "no-op erase does not change original");
  History reopened;
  require(reopened.count() == 63 && reopened.read(0, entry) && !std::strcmp(entry.name, "book-63.epub"),
          "history survives opening a new accessor");
  testFiles[History::PATH].push_back(0x99);
  require(history.append("/LocalSend", "recovered.epub"), "append repairs incomplete trailing record");
  require(history.count() == 64 && history.read(0, entry) && !std::strcmp(entry.name, "recovered.epub"),
          "recovered entry readable");
  const auto before = testFiles[History::PATH];
  testRenameFails = true;
  require(!history.eraseAt(0) && history.count() == 64 && testFiles[History::PATH] == before,
          "replace failure leaves original history untouched");
  testRenameFails = false;
  testRenameFailSource = History::TEMP;
  require(!history.eraseAt(0) && history.count() == 64 && testFiles[History::PATH] == before &&
              !testFiles.count(History::BACKUP),
          "failed temp publish restores original index");
  testRenameFailSource.clear();
  testFiles[History::BACKUP] = std::move(testFiles[History::PATH]);
  testFiles.erase(History::PATH);
  require(history.count() == 64 && history.read(0, entry) && !std::strcmp(entry.name, "recovered.epub") &&
              !testFiles.count(History::BACKUP),
          "power loss between backup and promote recovers old index");
  testShortWrite = true;
  require(!history.eraseAt(0) && testFiles[History::PATH] == before, "partial rewrite never destroys original history");
  testShortWrite = false;
  require(!history.append("/Books/../bad", "oops.epub") && !history.append("/Books", "../bad.epub"),
          "unsafe paths and names rejected");
  require(!history.read(64, entry), "read remains bounded after all operations");
  std::vector<uint8_t> atCapacity(History::MAX_ENTRIES * sizeof(History::Entry));
  for (int i = 0; i < History::MAX_ENTRIES; ++i) {
    History::Entry record{};
    std::snprintf(record.folder, sizeof(record.folder), "%s", "/Books");
    std::snprintf(record.name, sizeof(record.name), "bulk-%d.epub", i);
    std::memcpy(atCapacity.data() + i * sizeof(record), &record, sizeof(record));
  }
  testFiles[History::PATH] = std::move(atCapacity);
  require(history.count() == History::MAX_ENTRIES, "FreeInk history index maximum supported");
  require(history.append("/Books", "next.epub") && history.count() == History::MAX_ENTRIES,
          "at the UI index limit new receipts displace only the oldest history entry");
  require(history.read(0, entry) && !std::strcmp(entry.name, "next.epub") &&
              history.read(History::MAX_ENTRIES - 1, entry) && !std::strcmp(entry.name, "bulk-1.epub"),
          "32,767-entry rolling history remains ordered");
  testRemoveFailPath = History::PATH;
  require(!history.clear() && history.count() == History::MAX_ENTRIES,
          "failed clear leaves all indexed entries untouched");
  testRemoveFailPath.clear();
  require(history.clear() && history.count() == 0 && !testFiles.count(History::PATH) &&
              !testFiles.count(History::BACKUP) && !testFiles.count(History::TEMP),
          "clear removes only history index and auxiliary files");
  require(testFiles.count("/Books/LocalSend/protected.epub") == 1, "clear History does not delete any received file");
  require(history.clear() && history.count() == 0, "clearing empty History is idempotent");
  require(history.append("/Books", "after-clear.epub") && history.count() == 1 && history.read(0, entry) &&
              !std::strcmp(entry.name, "after-clear.epub"),
          "history continues receiving after a bulk clear");

  std::printf("PASS: %d LocalSend SD-backed history checks; %d records remain\n", checks, history.count());
}
