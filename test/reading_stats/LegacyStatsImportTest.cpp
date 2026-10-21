#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "../../src/util/ReadingStatsLegacyImport.h"
#include "../../src/util/ReadingStatsStore.h"
#include "fake_hal/HalStorage.h"
#include "fake_hal/RecentBooksStore.h"

FakeHalStorage Storage;
FakeRecentBooks RECENT_BOOKS;

static uint64_t hashPath(const std::string& text) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
  return hash;
}

static void addCrossInkBook(const std::string& path, uint32_t seconds, uint32_t turns) {
  const auto cache = "/.crosspoint/epub_" + std::to_string(hashPath(path)) + "/stats_v5.bin";
  std::string bytes(73, '\0');
  bytes[0] = 5;
  bytes[1] = 2;
  for (int i = 0; i < 4; ++i) {
    bytes[3 + i] = static_cast<char>((seconds >> (i * 8)) & 0xff);
    bytes[7 + i] = static_cast<char>((turns >> (i * 8)) & 0xff);
  }
  Storage.files[cache] = bytes;
  Storage.files[path] = "fake epub content";
}

int main() {
  using namespace readingstats;
  using namespace readingstats::legacy;
  Storage.files["/.crosspoint/reading_stats.json"] = R"JSON(
{"formatVersion":6,"readingDays":[],"legacyReadingDays":[],"sessionLog":[],
 "books":[
 {"bookId":"id1","path":"/Books/one.epub","title":"One","author":"A","totalReadingMs":3600500,"sessions":3,
  "readingDays":[{"dayOrdinal":20500,"readingMs":3600500}]},
 {"bookId":"id2","path":"/Books/two.epub","title":"Two","author":"B","totalReadingMs":61000,"sessions":1,
  "readingDays":[{"dayOrdinal":20501,"readingMs":61000}]}
]})JSON";
  auto first = importSource(Source::VCodex);
  assert(first.success && first.imported == 2 && first.alreadyImported == 0 && first.booksSeen == 2);
  Snapshot global{};
  assert(loadGlobal(global));
  assert(global.valid() && global.totalSeconds() == 3661 && global.sessions == 4 && global.pageTurns == 0);
  assert(global.secondsOnDay(20501) == 3600 && global.secondsOnDay(20502) == 61);
  assert(bookCount() == 2);
  BookEntry entry{};
  assert(bookAt(0, entry) && std::string(entry.title) == "One");
  Snapshot book{};
  assert(loadBook("/Books/one.epub", book) && book.totalSeconds() == 3600 && book.sessions == 3);

  auto second = importSource(Source::VCodex);
  assert(second.success && second.imported == 0 && second.alreadyImported == 2);
  assert(loadGlobal(global) && global.totalSeconds() == 3661);
  Storage.files["/.crosspoint/reading_stats.json"].replace(
      Storage.files["/.crosspoint/reading_stats.json"].find("3600500"), 7, "9600500");
  auto updated = importSource(Source::VCodex);
  assert(updated.success && updated.imported == 0 && updated.alreadyImported == 2);
  assert(loadGlobal(global) && global.totalSeconds() == 3661);

  // A corrupt complete receipt must stop imports, not be discarded.
  const auto receiptPath = "/.crosspoint/reading-stats/import_receipts_v1.bin";
  auto savedReceipts = Storage.files[receiptPath];
  Storage.files[receiptPath][4] ^= 0x40;
  auto corrupted = importSource(Source::VCodex);
  assert(!corrupted.success && corrupted.errors > 0);
  assert(loadGlobal(global) && global.totalSeconds() == 3661);
  Storage.files[receiptPath] = savedReceipts;

  // Both legacy firmwares can carry overlapping history. Refuse importing
  // a second source into this native database without a reconciliation tool.
  std::string otherGlobal(159, '\0');
  otherGlobal[0] = 3;
  otherGlobal[1] = 1;
  Storage.files["/.crosspoint/global_stats.bin"] = otherGlobal;
  auto conflict = importSource(Source::CrossInk);
  assert(!conflict.success && conflict.otherSourceAlreadyImported);
  assert(loadGlobal(global) && global.totalSeconds() == 3661);

  // Recover a power loss between writing the book and the global snapshot.
  Storage.files.clear();
  RECENT_BOOKS.entries.clear();
  Storage.files["/.crosspoint/reading_stats.json"] = R"JSON({"formatVersion":6,"books":[
    {"bookId":"three","path":"/Books/three.epub","totalReadingMs":120000,"sessions":2,"readingDays":[]}
  ]})JSON";
  Storage.failRenameTo = "/.crosspoint/reading-stats/global_v2.bin";
  const auto interrupted = importSource(Source::VCodex);
  assert(!interrupted.success && interrupted.errors);
  assert(Storage.exists("/.crosspoint/reading-stats/import_pending_v1.bin"));
  // Also simulate a torn receipt append: the pending marker allows only its
  // incomplete trailing bytes to be trimmed and safely replayed.
  Storage.files["/.crosspoint/reading-stats/import_receipts_v1.bin"] = "torn";
  Storage.failRenameTo.clear();
  assert(recoverLegacyImport());
  assert(!Storage.exists("/.crosspoint/reading-stats/import_pending_v1.bin"));
  assert(loadGlobal(global) && global.totalSeconds() == 120);
  assert(loadBook("/Books/three.epub", book) && book.totalSeconds() == 120);
  auto retried = importSource(Source::VCodex);
  assert(retried.success && retried.imported == 0 && retried.alreadyImported == 1);
  assert(loadGlobal(global) && global.totalSeconds() == 120);

  // CrossInk global totals are imported once; per-book history is filed
  // against cached original paths without adding those totals globally again.
  Storage.files.clear();
  RECENT_BOOKS.entries.clear();
  std::string cross(159, '\0');
  cross[0] = 3;
  cross[1] = 4;
  cross[5] = char(0x10);
  cross[6] = char(0x0e);
  cross[9] = 30;
  Storage.files["/.crosspoint/global_stats.bin"] = cross;
  addCrossInkBook("/Books/a.epub", 1200, 11);
  addCrossInkBook("/Books/b.epub", 2400, 19);
  RECENT_BOOKS.entries.push_back({"/Books/a.epub", "Book A", "Author A"});
  RECENT_BOOKS.entries.push_back({"/Books/b.epub", "Book B", "Author B"});
  auto ink = importSource(Source::CrossInk);
  assert(ink.success && ink.imported == 3 && ink.booksSeen >= 2);
  assert(loadGlobal(global) && global.totalSeconds() == 3600 && global.pageTurns == 30);
  assert(loadBook("/Books/a.epub", book) && book.totalSeconds() == 1200 && book.pageTurns == 11);
  auto inkAgain = importSource(Source::CrossInk);
  assert(inkAgain.success && inkAgain.imported == 0 && inkAgain.alreadyImported >= 3);
  assert(loadGlobal(global) && global.totalSeconds() == 3600);

  std::puts("Legacy import: vCodex books/days, CrossInk book/global, receipt dedup, interrupted write recovery PASS");
  return 0;
}
