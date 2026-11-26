#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "ReadingHistoryMigration.h"
#include "ReadingHistoryProvider.h"
#include "ReadingStatsBookIndex.h"

FakeHalStorage Storage;
using namespace readingHistory;
using namespace readingstats;
namespace {
constexpr char DIR[] = "/.crosspoint/reading-stats";
constexpr char LOG[] = "/.crosspoint/reading_sessions.jsonl";
constexpr char CURSOR[] = "/.crosspoint/reading_sessions_sync.json";
constexpr char RECEIPT[] = "/.crosspoint/reading_sessions.migration.json";
constexpr char BACKUP[] = "/.crosspoint/reading_sessions.migration.bak";
constexpr char TEMP[] = "/.crosspoint/reading_sessions.migration.tmp";
constexpr char A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr uint32_t DAY = 20782;

std::string id(const std::string& path) {
  if (path == "/Books/a.epub") return A;
  if (path == "/Send2Ereader/06.00 - Heart of the Mountain (2025).epub") return "b02c841aaf93e41d4259f3df0b263e61";
  if (path == "/Send2Ereader/05.00 - Graveyard of Demons (2024).epub") return "1fb83f5534e07ed3af834a37f63403fe";
  if (path == "/Send2Ereader/Vicious - #01.00.epub") return "fda4dcbaf8989184f0c0f592833df8b0";
  if (path == "/Send2Ereader/Project Hail Mary (2021).epub") return "54e353e866c12d176479255109d6a7c1";
  return {};
}
std::string bytes(const auto& value) { return {reinterpret_cast<const char*>(&value), sizeof(value)}; }

void seed() {
  Storage.files.clear();
  Storage.directories.clear();
  Storage.failRenameTo.clear();
  Storage.failRemoveDir = false;
  Storage.readLimit = SIZE_MAX;
  Storage.mkdir(DIR);
  BookEntry book;
  const std::string path = "/Books/a.epub";
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : path) hash = (hash ^ c) * 1099511628211ULL;
  book.setHash(hash);
  snprintf(book.path, sizeof(book.path), "%s", path.c_str());
  snprintf(book.title, sizeof(book.title), "Book A");
  snprintf(book.author, sizeof(book.author), "Author A");
  book.seal();
  Storage.files[std::string(DIR) + "/books_v1.bin"] = bytes(book);
  Snapshot s;
  s.addSession(120, 7, DAY + 1);
  s.addSession(240, 10, DAY);
  s.lastSequence = 2;
  s.seal();
  char suffix[17];
  snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(hash));
  Storage.files[std::string(DIR) + "/global_v2.bin"] = bytes(s);
  Storage.files[std::string(DIR) + "/book_" + suffix + "_v2.bin"] = bytes(s);
  TimeBuckets time;
  time.addSession(120, DAY + 1, 0);
  time.addSession(240, DAY, 1);
  time.lastSequence = 2;
  time.seal();
  Storage.files[std::string(DIR) + "/time_global_v1.bin"] = bytes(time);
  Storage.files[std::string(DIR) + "/time_" + suffix + "_v1.bin"] = bytes(time);
  Storage.files[LOG] = std::string("{\"doc\":\"") + A +
                       "\",\"start\":1795564800,\"secs\":120,\"tz\":0,\"path\":\"/Books/a.epub\",\"to\":4567}\n";
  Storage.files[CURSOR] = "{\"account\":\"reader\",\"offset\":42}";
  // Other firmware's data is deliberately outside the migration scope.
  Storage.files["/.crosspoint/reading_stats.json"] = "untouched";
}

void verify() {
  assert(migrationReady());
  Provider p;
  auto status = p.begin(A);
  while (status == Status::Scanning) status = p.step();
  assert(status == Status::Ready && p.summary());
  const auto& s = *p.summary();
  assert(s.baselineApplied && s.overall.seconds == 360 && s.overall.sessions == 2 && s.overall.turns == 17);
  assert(s.book.seconds == 360 && s.book.turns == 17 && s.overall.turnsComplete && s.book.turnsComplete);
  assert(s.books == 1 && !s.rejected && !s.incompleteTail);
  Day day;
  assert(p.readDay(DAY, day) && day.seconds == 120 && day.bookSeconds == 120);
  assert(p.readDay(DAY - 1, day) && day.seconds == 240 && day.bookSeconds == 240);
  assert(s.weekPeriods[0] == 120 && s.weekPeriods[1] == 240);
  BookInfo info;
  assert(p.readBooks(0, &info, 1) == Status::Ready && info.progress == 4567);
  assert(p.aggregationBuffersBytes() == 0);
}
void cases() {
  seed();
  const auto original = Storage.files[LOG], cursor = Storage.files[CURSOR];
  assert(migrateLegacy(id));
  verify();
  assert(Storage.files[LOG].starts_with(original) && Storage.files[CURSOR] == cursor);
  assert(!Storage.exists(DIR) && !Storage.exists(RECEIPT) && !Storage.exists(BACKUP));
  assert(Storage.files["/.crosspoint/reading_stats.json"] == "untouched");
  const auto result = Storage.files[LOG];
  assert(migrateLegacy(id) && Storage.files[LOG] == result);
  // Failed publication after the first rename. The exact old prefix and
  // validated candidate remain recoverable; no legacy data is deleted.
  seed();
  Storage.failRenameTo = LOG;
  assert(!migrateLegacy(id) && !migrationReady() && migrationPending());
  assert(!Storage.exists(LOG) && Storage.files[BACKUP] == original && Storage.exists(TEMP) && Storage.exists(DIR));
  Storage.failRenameTo.clear();
  assert(migrateLegacy(id));
  verify();
  assert(!Storage.exists(DIR) && Storage.files[CURSOR] == cursor);
  // Partial cleanup: receipt allows retry after sessions have been appended.
  seed();
  Storage.failRemoveDir = true;
  assert(migrateLegacy(id) && migrationReady());
  assert(Storage.exists(RECEIPT) && Storage.exists(DIR));
  Storage.files[LOG] +=
      std::string("{\"doc\":\"") + A + "\",\"start\":0,\"secs\":60,\"tracked_secs\":60,\"v\":2,\"turns\":5}\n";
  const auto extended = Storage.files[LOG];
  Storage.failRemoveDir = false;
  assert(migrateLegacy(id) && Storage.files[LOG] == extended && !Storage.exists(DIR));
  // Corruption, unrecognized records, orphan data and unresolved IDs leave
  // the source untouched and never publish a cleanup receipt.
  seed();
  Storage.files[std::string(DIR) + "/global_v2.bin"][12] ^= 1;
  assert(!migrateLegacy(id) && Storage.files[LOG] == original && Storage.exists(DIR) && !Storage.exists(RECEIPT));
  seed();
  Storage.files[std::string(DIR) + "/book_0000000000000000_v2.bin"] = "orphan";
  assert(!migrateLegacy(id) && Storage.files[LOG] == original && Storage.exists(DIR));
  seed();
  Storage.files[LOG] += "{unfinished";
  assert(!migrateLegacy(id) && Storage.exists(DIR));
  seed();
  assert(!migrateLegacy([](const std::string&) -> std::string { return {}; }) && Storage.exists(DIR));
  seed();
  Storage.failRenameTo = LOG;
  assert(!migrateLegacy(id));
  Storage.failRenameTo.clear();
  Storage.files[TEMP][10] ^= 1;
  assert(!migrateLegacy(id) && Storage.files[LOG] == original && Storage.exists(DIR));
  assert(!migrationPending());
  assert(migrateLegacy(id));
  verify();
}

void actualPicco(const char* path) {
  seed();
  Storage.files.clear();
  Storage.directories.clear();
  Storage.failRenameTo.clear();
  Storage.mkdir(DIR);
  // Directory listing is supplied by the test command; filenames are known
  // from the backed-up catalog and are never taken from the live device.
  const char* names[] = {"books_v1.bin",
                         "global_v2.bin",
                         "global_v2.bin.bak",
                         "time_global_v1.bin",
                         "time_global_v1.bin.bak",
                         "book_1114313a5de3b786_v2.bin",
                         "book_1114313a5de3b786_v2.bin.bak",
                         "book_6466f16687f34a63_v2.bin",
                         "book_aa036c23552c60bd_v2.bin",
                         "book_aa036c23552c60bd_v2.bin.bak",
                         "time_6466f16687f34a63_v1.bin",
                         "time_aa036c23552c60bd_v1.bin",
                         "time_aa036c23552c60bd_v1.bin.bak"};
  for (const char* name : names) {
    std::ifstream file(std::string(path) + "/reading-stats/" + name, std::ios::binary);
    assert(file.good());
    Storage.files[std::string(DIR) + "/" + name].assign(std::istreambuf_iterator<char>(file), {});
  }
  std::ifstream file(std::string(path) + "/reading_sessions.jsonl", std::ios::binary);
  assert(file.good());
  Storage.files[LOG].assign(std::istreambuf_iterator<char>(file), {});
  const auto original = Storage.files[LOG];
  assert(migrateLegacy(id) && Storage.files[LOG].starts_with(original));
  Provider p;
  assert(p.begin("b02c841aaf93e41d4259f3df0b263e61") == Status::Scanning);
  while (p.step() == Status::Scanning) {
  }
  assert(p.summary() && p.summary()->overall.seconds == 90914 && p.summary()->overall.sessions == 94 &&
         p.summary()->overall.turns == 3587 && p.summary()->book.seconds == 58637 && p.summary()->book.sessions == 73 &&
         p.summary()->book.turns == 2035 && p.summary()->books == 4);
  assert(!Storage.exists(DIR));
  std::puts("PASS automatic migration against copied Picco RST2 data");
}
}  // namespace
int main(int argc, char** argv) {
  cases();
  if (argc == 2) actualPicco(argv[1]);
  std::puts("PASS migration publication, recovery and safe cleanup");
}
