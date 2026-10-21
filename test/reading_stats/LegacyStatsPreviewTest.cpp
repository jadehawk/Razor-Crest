#include <cassert>
#include <cstdio>
#include <string>

#include "../../src/util/ReadingStatsLegacyPreview.h"
#include "fake_hal/HalStorage.h"

FakeHalStorage Storage;
using readingstats::legacy::inspect;
using readingstats::legacy::Source;

int main() {
  auto missing = inspect(Source::VCodex);
  assert(!missing.present && !missing.supported);
  auto missingCross = inspect(Source::CrossInk);
  assert(!missingCross.present && !missingCross.supported);

  std::string crossInk(159, '\0');
  crossInk[0] = 3;
  crossInk[1] = 5;
  crossInk[5] = char(0x10);
  crossInk[6] = char(0x0e);
  crossInk[9] = 21;
  Storage.files["/.crosspoint/global_stats.bin"] = crossInk;
  auto cross = inspect(Source::CrossInk);
  assert(cross.present && cross.supported);
  assert(cross.totals.sessions == 5 && cross.totals.totalSeconds == 3600 && cross.totals.turns == 21);
  Storage.files["/.crosspoint/global_stats.bin"] = std::string(73, '\0');
  assert(!inspect(Source::CrossInk).supported);

  Storage.files["/.crosspoint/reading_stats.json"] = R"JSON(
{"formatVersion":6,"readingDays":[{"dayOrdinal":20000,"readingMs":3599000}],"legacyReadingDays":[],
 "sessionLog":[{"dayOrdinal":20000,"sessionMs":3599000,"bookId":"book-one"}],
 "books":[
 {"bookId":"one","path":"/Library/one.epub","title":"Escaped \"books\"","author":"Author",
  "totalReadingMs":3600500,"sessions":3,"readingDays":[{"dayOrdinal":20000,"readingMs":3600500}]},
 {"bookId":"two","path":"/Library/two.epub","totalReadingMs":61000,"sessions":1,
  "knownPaths":["/Another/two.epub"],"readingDays":[]}
]})JSON";
  auto v = inspect(Source::VCodex);
  assert(v.present && v.supported && !v.truncated);
  assert(v.books == 2 && v.totals.sessions == 4 && v.totals.totalSeconds == 3661);
  assert(v.totals.turns == 0);

  Storage.files["/.crosspoint/reading_stats.json"] = R"JSON({"formatVersion":5,"books":[]})JSON";
  assert(!inspect(Source::VCodex).supported);
  Storage.files["/.crosspoint/reading_stats.json"] =
      R"JSON({"formatVersion":6,"books":[{"path":"../escape","totalReadingMs":1,"sessions":1}]})JSON";
  assert(!inspect(Source::VCodex).supported);
  Storage.files["/.crosspoint/reading_stats.json"] =
      R"JSON({"formatVersion":6,"books":[{"path":"/Library/ok","totalReadingMs":1,"sessions":1}]ERROR})JSON";
  assert(!inspect(Source::VCodex).supported);
  std::puts("Legacy streaming preview: CrossInk and vCodex source tests PASS");
  return 0;
}
