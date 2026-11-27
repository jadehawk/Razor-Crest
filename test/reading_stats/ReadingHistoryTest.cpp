#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <new>
#include <string>

#include "ReadingHistory.h"
#include "ReadingHistoryProvider.h"
#include "ReadingHistorySource.h"
#include "activities/plugins/DessenReadingStats.h"

using namespace readingHistory;

// Fault injection for the explicit fallible array allocations used by the
// provider. A per-allocation ceiling models a fragmented heap; the countdown
// exercises rollback after partially allocating the day ring.
static size_t arrayLimit = SIZE_MAX;
static size_t arrayBudget = SIZE_MAX;
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (size > arrayLimit || arrayBudget == 0) return nullptr;
  if (arrayBudget != SIZE_MAX) --arrayBudget;
  return ::operator new[](size);
}
FakeHalStorage Storage;
namespace {
constexpr char LOG[] = "/.crosspoint/reading_sessions.jsonl";
constexpr char A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr int64_t START = 1764115200;  // 2025-11-26 00:00 UTC
constexpr uint32_t DAY = START / 86400;

std::string session(std::string_view id, int64_t start = START, unsigned secs = 120, int tz = 0) {
  return "{\"doc\":\"" + std::string(id) + "\",\"start\":" + std::to_string(start) +
         ",\"secs\":" + std::to_string(secs) + ",\"tz\":" + std::to_string(tz) + "}\n";
}

void feed(Query& q, const std::string& data, size_t chunk = 37) {
  for (size_t offset = 0; offset < data.size(); offset += chunk) {
    assert(q.feed(data.data() + offset, std::min(chunk, data.size() - offset)) == Status::Scanning);
  }
}

void edgeCases() {
  Query q;
  assert(q.begin("invalid") == Status::InvalidQuery);
  assert(!q.summary());
  assert(q.begin(A) == Status::Scanning);
  const std::string first = session(A, START, 120, -300);
  const std::string second = session(B, START + 6 * 3600, 240);
  const std::string third = session("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", START, 60);
  const std::string undated = session(A, 0, 30);
  const std::string noTz = "{\"doc\":\"" + std::string(A) + "\",\"start\":" + std::to_string(START) + ",\"secs\":20}\n";
  const std::string baseline =
      "{\"type\":\"baseline\",\"doc\":\"" + std::string(A) + "\",\"start\":1764115200,\"secs\":120}\n";
  const std::string bad = "{bad}\n" + session(A, START, 9) + session("nope") + session(B, START, 604801) +
                          std::string(MAX_LINE + 1, 'x') + "\n" + first.substr(0, first.size() - 1) + second;
  const std::string truncated = "{\"doc\":\"";
  const std::string all = first + second + third + undated + noTz + baseline + bad + truncated;
  feed(q, all, 1);
  assert(!q.summary() && !q.bookAt(0));
  assert(q.finish() == Status::Ready);
  const auto& r = *q.summary();
  assert(r.bytes == all.size() && r.rejected == 6 && r.incompleteTail);
  assert(r.overall.sessions == 5 && r.overall.seconds == 470 && r.overall.datedSessions == 3);
  assert(r.book.sessions == 4 && r.book.seconds == 230 && r.book.datedSessions == 2);
  assert(r.overall.periods[0] == 240 && r.overall.periods[2] == 120 && r.overall.periods[3] == 60);
  assert(r.anchorDay == DAY && r.activeDays == 2 && r.bookActiveDays == 2 && r.books == 2);
  assert(q.dayAt(DAY - 1).seconds == 120 && q.dayAt(DAY - 1).bookSeconds == 120);
  assert(q.dayAt(DAY).seconds == 300 && q.dayAt(DAY).bookSeconds == 60);
  assert(q.bookAt(0)->front() == 0xaa && q.bookAt(1)->front() == 0xbb);
  assert(!q.bookAt(2));
  assert(q.begin(A, DAY + HISTORY_DAYS) == Status::Scanning);
  feed(q, first + second);
  q.finish();
  assert(q.summary()->overall.seconds == 360 && q.summary()->activeDays == 0);
  assert(q.summary()->weekPeriods[0] == 0 && q.summary()->weekPeriods[3] == 0);
  assert(!q.dayAt(DAY).seconds);
  assert(q.begin() == Status::Scanning);
  feed(q, session(B, START + HISTORY_DAYS * 86400LL) + session(A, START));
  q.finish();
  assert(q.summary()->activeDays == 1 && q.summary()->overall.sessions == 2);
  assert(!q.dayAt(DAY).seconds);
  assert(q.begin() == Status::Scanning);
  assert(q.feed(nullptr, 1) == Status::IoError && !q.summary());
  assert(q.begin() == Status::Scanning);
  q.stop(Status::Cancelled);
  assert(q.finish() == Status::Cancelled && !q.summary());
}

uint64_t getLe(const uint8_t* data, unsigned n) {
  uint64_t value = 0;
  for (unsigned i = 0; i < n; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
  return value;
}

void wire() {
  Query q;
  uint8_t out[DSN_STATS_SUMMARY_BYTES + 1];
  std::memset(out, 0x5a, sizeof(out));
  using dessenReadingStats::read;
  assert(read(q, DSN_STATS_SUMMARY, 0, out, sizeof(out)) == DSN_ERR_INVALID);
  q.begin(A, DAY);
  feed(q, session(A, START, 120) + session(B, START, 240));
  q.finish();
  assert(read(q, DSN_STATS_SUMMARY, 0, out, DSN_STATS_SUMMARY_BYTES - 1) == DSN_ERR_TOO_BIG);
  assert(out[0] == 0x5a);
  assert(read(q, DSN_STATS_SUMMARY, 0, out, sizeof(out)) == DSN_STATS_SUMMARY_BYTES);
  assert(out[DSN_STATS_SUMMARY_BYTES] == 0x5a);
  assert(getLe(out + 184, 8) == 360);
  assert(getLe(out, 4) == DSN_STATS_VERSION && getLe(out + 8, 4) == DAY);
  assert(getLe(out + 16, 4) == 1 && getLe(out + 20, 4) == 1 && getLe(out + 24, 4) == 2);
  assert(getLe(out + 48, 8) == 360 && getLe(out + 56, 8) == 2);
  assert(getLe(out + 104, 8) == 120 && getLe(out + 112, 8) == 1);
  assert(read(q, DSN_STATS_DAYS, DAY - 1, out, 32) == 32);
  assert(getLe(out, 8) == 0 && getLe(out + 16, 8) == 360 && getLe(out + 24, 8) == 120);
  assert(read(q, DSN_STATS_DAYS, DAY, out, 32) == 16);
  assert(read(q, DSN_STATS_DAYS, DAY + 1, out, 16) == DSN_ERR_INVALID);
  assert(read(q, DSN_STATS_BOOKS, 0, out, 17) == DSN_ERR_INVALID);
  assert(read(q, DSN_STATS_BOOKS, 0, out, 32) == 32 && out[0] == 0xaa && out[16] == 0xbb);
  assert(read(q, DSN_STATS_BOOKS, 1, out, 32) == 16 && out[0] == 0xbb);
  assert(read(q, DSN_STATS_BOOKS, 2, out, 32) == 0);
  assert(read(q, 99, 0, out, 16) == DSN_ERR_INVALID);
  assert(dessenReadingStats::resultCode(Status::Missing) == DSN_STATS_NO_HISTORY);
  assert(dessenReadingStats::resultCode(Status::NoMemory) == DSN_STATS_NO_MEMORY);
}

void capacity() {
  Query q;
  assert(q.begin() == Status::Scanning);
  for (int n = MAX_BOOKS + 10; n >= 0; --n) {
    char id[33];
    std::snprintf(id, sizeof(id), "%032x", static_cast<unsigned>(n));
    feed(q, session(id), 4096);
  }
  q.finish();
  assert(q.summary()->books == MAX_BOOKS && q.summary()->booksLimited);
  assert(q.summary()->overall.sessions == MAX_BOOKS + 11);
  assert(q.bookAt(0)->back() == 0);
  assert((*q.bookAt(MAX_BOOKS - 1))[14] == 3 && q.bookAt(MAX_BOOKS - 1)->back() == 255);
}

void fragmentedHistory() {
  Storage.files.clear();
  Storage.files[LOG] = session(A, START, 120) + session(B, START + 86400, 60);
  Provider p;
  auto finish = [&](Status status) {
    while (status == Status::Scanning) status = p.step();
    return status;
  };
  // The previous 8904-byte contiguous ring and 4096-byte input buffer
  // cannot be allocated under this ceiling. The new ring and buffer can.
  arrayLimit = 2048;
  assert(finish(p.begin(A)) == Status::Ready);
  assert(p.summary()->overall.seconds == 180 && p.summary()->book.seconds == 120);
  const auto prefix = p.expectedBytes(), hash = p.verifiedSourceHash();
  Storage.files[LOG] += session(A, START + 2 * 86400, 60);
  assert(finish(p.begin(A)) == Status::Ready);
  assert(p.summary()->overall.sessions == 3 && p.summary()->overall.seconds == 240);
  Day day{};
  assert(p.readDay(DAY + 2, day) && day.seconds == 60 && day.bookSeconds == 60);
  assert(finish(p.begin(A, 0, false, prefix)) == Status::Ready);
  assert(p.verifiedSourceHash() == hash && p.summary()->overall.sessions == 2);
  // Fail after some blocks have been allocated. All allocations must be
  // released, and a retry must recover the exact same history.
  p.cancel();
  Storage.files.erase("/.crosspoint/reading_sessions-history.cache");
  arrayBudget = 4;
  assert(finish(p.begin()) == Status::NoMemory);
  assert(!p.summary() && p.aggregationBuffersBytes() == 0);
  arrayBudget = SIZE_MAX;
  assert(finish(p.begin()) == Status::Ready);
  assert(p.summary()->overall.seconds == 240 && p.aggregationBuffersBytes() == 0);
  p.cancel();
  arrayLimit = SIZE_MAX;
  Storage.files.clear();
  puts("PASS fragmented heap allocation ceiling, rollback, append and pinned prefix");
}

void provider() {
  Provider p;
  assert(p.begin() == Status::Missing && p.progressPercent() == 0);
  Storage.files[LOG] = "";
  assert(p.begin() == Status::Ready && p.summary()->overall.sessions == 0);
  assert(p.progressPercent() == 100);
  Storage.files[LOG] = session(A);
  for (int i = 0; i < 100; ++i) Storage.files[LOG] += session(B);
  const auto original = Storage.files[LOG];
  assert(p.begin(A) == Status::Scanning && p.expectedBytes() == original.size());
  assert(p.progressPercent() == 0 && p.processedSessions() == 0);
  assert(p.step() == Status::Scanning && p.processedBytes() == 2048);
  assert(p.progressPercent() == 2048 * 100 / original.size());
  assert(p.processedSessions() > 0 && p.processedSessions() < 101);
  p.cancel();
  assert(p.step() == Status::Cancelled && !p.summary() && p.progressPercent() == 0);
  assert(p.begin(A) == Status::Scanning);
  // The append is outside the opened snapshot, so it belongs to the next query.
  Storage.files[LOG] += session(A);
  while (p.step() == Status::Scanning) {
  }
  assert(p.summary()->overall.sessions == 101 && p.summary()->book.sessions == 1);
  assert(p.processedBytes() == original.size());
  assert(p.progressPercent() == 100 && p.processedSessions() == 101 && !p.rejectedLines());
  assert(p.begin(A) == Status::Scanning);
  while (p.step() == Status::Scanning) {
  }
  assert(p.summary()->overall.sessions == 102 && p.summary()->book.sessions == 2);
  assert(Storage.files[LOG] == original + session(A));
  Storage.readLimit = 4100;
  assert(p.begin() == Status::Scanning);
  Status shortRead;
  do {
    shortRead = p.step();
  } while (shortRead == Status::Scanning);
  assert(shortRead == Status::IoError && !p.summary() && p.progressPercent() == 0);
  Storage.readLimit = SIZE_MAX;
  assert(p.begin() == Status::Scanning);
  while (p.step() == Status::Scanning) {
  }
  assert(p.summary()->overall.sessions == 102);
}

constexpr char CACHE[] = "/.crosspoint/reading_sessions-history.cache";
constexpr char TEMP[] = "/.crosspoint/reading_sessions-history.cache.tmp";

void compareProvider(const Provider& p, const Query& q) {
  uint8_t actual[1024], expected[1024];
  const auto* s = q.summary();
  assert(s && p.summary() && p.aggregationBuffersBytes() == 0);
  assert(dessenReadingStats::read(p, DSN_STATS_SUMMARY, 0, actual, 192) == 192);
  assert(dessenReadingStats::read(q, DSN_STATS_SUMMARY, 0, expected, 192) == 192);
  assert(!std::memcmp(actual, expected, 192));
  for (size_t i = 0; i < s->books; ++i) {
    DocumentId id{};
    uint32_t offset;
    assert(p.readBook(i, id, offset) && id == *q.bookAt(i) && offset == q.bookOffsetAt(i));
  }
  if (s->anchorDay) {
    const uint32_t first = s->anchorDay - HISTORY_DAYS + 1;
    for (uint32_t day = first; day <= s->anchorDay;) {
      const auto n = std::min<uint32_t>(64, s->anchorDay - day + 1);
      assert(dessenReadingStats::read(p, DSN_STATS_DAYS, day, actual, n * 16) == static_cast<int>(n * 16));
      assert(dessenReadingStats::read(q, DSN_STATS_DAYS, day, expected, n * 16) == static_cast<int>(n * 16));
      assert(!std::memcmp(actual, expected, n * 16));
      day += n;
    }
  }
}

void cacheCases() {
  Storage.files.clear();
  Provider p;
  auto check = [&](std::string_view doc, uint32_t anchor, uint64_t parsedBytes) {
    const auto begin = p.begin(doc, anchor);
    assert(begin == Status::Scanning || begin == Status::Ready);
    Status status = begin;
    while (status == Status::Scanning) status = p.step();
    assert(status == Status::Ready && p.parsedBytes() == parsedBytes);
    Query q;
    assert(q.begin(doc, anchor) == Status::Scanning);
    feed(q, Storage.files[LOG], 4096);
    assert(q.finish() == Status::Ready);
    compareProvider(p, q);
  };
  Storage.files[LOG] = session(A) + session(B, START + 86400, 240);
  check(A, 0, Storage.files[LOG].size());
  assert(Storage.exists(CACHE) && !Storage.exists(TEMP));
  const std::string cold = Storage.files[CACHE];
  check(A, 0, 0);
  assert(Storage.files[CACHE] == cold);
  // Appending a later date advances the inferred anchor, updates the latest
  // book offset, and must not double-count old calendar or weekly totals.
  const std::string suffix = session(A, START + 372LL * 86400, 360) + session(B, START + 371LL * 86400);
  Storage.files[LOG] += suffix;
  check(A, 0, suffix.size());
  check(A, 0, 0);
  // Different document or a different fixed date is a different query.
  check(B, 0, Storage.files[LOG].size());
  check(B, DAY + 372, Storage.files[LOG].size());
  check(B, DAY + 372, 0);
  // Same length, different contents: a matching size is not a valid key.
  const auto pos = Storage.files[LOG].find("\"secs\":120");
  assert(pos != std::string::npos);
  Storage.files[LOG].replace(pos + 7, 3, "180");
  check(B, DAY + 372, Storage.files[LOG].size());
  // Cache corruption and truncation both cause complete rebuilds.
  Storage.files[CACHE][250] ^= 1;
  check(B, DAY + 372, Storage.files[LOG].size());
  Storage.files[CACHE].resize(100);
  check(B, DAY + 372, Storage.files[LOG].size());
  Storage.files[LOG] = session(A);
  check(A, 0, Storage.files[LOG].size());
  // Incomplete tail can be displayed but must not be resumed across append.
  const auto pending = session(B);
  Storage.files[LOG] += pending.substr(0, pending.size() - 1);
  check(A, 0, pending.size() - 1);
  assert(p.summary()->incompleteTail);
  check(A, 0, 0);
  Storage.files[LOG] += "\n";
  check(A, 0, Storage.files[LOG].size());
  assert(!p.summary()->incompleteTail);
  // Interrupted publication leaves authoritative history intact.
  const auto old = Storage.files[CACHE];
  Storage.files[LOG] += session(A);
  const auto source = Storage.files[LOG];
  Storage.failRenameTo = CACHE;
  assert(p.begin(A) == Status::Scanning);
  Status status;
  do {
    status = p.step();
  } while (status == Status::Scanning);
  assert(status == Status::IoError && !p.summary() && p.aggregationBuffersBytes() == 0);
  assert(Storage.files[CACHE] == old && Storage.files[LOG] == source && !Storage.exists(TEMP));
  Storage.failRenameTo.clear();
  Storage.files[TEMP] = "interrupted";
  check(A, 0, session(A).size());
  assert(!Storage.exists(TEMP));
  // Cancellation during a warm verification leaves the previous cache intact.
  const auto complete = Storage.files[CACHE];
  assert(p.begin(A) == Status::Scanning && p.validatingCache());
  p.cancel();
  assert(p.step() == Status::Cancelled && p.aggregationBuffersBytes() == 0);
  assert(Storage.files[CACHE] == complete);
  check(A, 0, 0);
  Storage.files.clear();
}

void bookInfo() {
  Provider p;
  const std::string a = session(A);
  const std::string b = session(B);
  const std::string latest =
      R"({"doc":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","start":1764115200,"secs":120,"tz":0,"title":"A \"quoted\" title","to":8123})"
      "\n";
  Storage.files[LOG] = a + b + std::string(4100, 'x') + "\n" + latest;
  assert(p.begin() == Status::Scanning);
  while (p.step() == Status::Scanning) {
  }
  DocumentId id{};
  uint32_t offset;
  assert(p.readBook(0, id, offset) && offset == a.size() + b.size() + 4101);
  uint8_t out[2 * DSN_STATS_BOOK_INFO_BYTES + 1];
  std::memset(out, 0x5a, sizeof(out));
  assert(dessenReadingStats::readBookInfo(p, 0, out, 2 * DSN_STATS_BOOK_INFO_BYTES) == 200);
  assert(out[0] == 0xaa && getLe(out + 16, 2) == 8123 && out[18] == 16);
  assert(std::string(reinterpret_cast<char*>(out + 20), out[18]) == "A \"quoted\" title");
  assert(out[100] == 0xbb && getLe(out + 116, 2) == UINT16_MAX && out[118] == 0 && out[200] == 0x5a);
  assert(dessenReadingStats::readBookInfo(p, 2, out, 100) == 0);
  assert(dessenReadingStats::readBookInfo(p, 0, out, 99) == DSN_ERR_INVALID);
  Storage.files[LOG] = a;
  std::memset(out, 0x5a, sizeof(out));
  assert(dessenReadingStats::readBookInfo(p, 0, out, 100) == DSN_ERR_IO && out[0] == 0x5a);
}

void migrationFixture(const char* logPath, const char* reportPath) {
  std::ifstream input(logPath, std::ios::binary), reportFile(reportPath);
  assert(input.good() && reportFile.good());
  const std::string log((std::istreambuf_iterator<char>(input)), {});
  const std::string reportText((std::istreambuf_iterator<char>(reportFile)), {});
  JsonDocument report;
  assert(!deserializeJson(report, reportText));
  const auto global = report["global"].as<JsonObjectConst>();
  const auto books = report["books"].as<JsonArrayConst>();
  assert(!global.isNull() && !books.isNull());
  Storage.files.clear();
  Storage.files[LOG] = log;
  Provider p;
  auto verify = [&](const char* id, JsonObjectConst selected) {
    assert(p.begin(id) == Status::Scanning);
    while (p.step() == Status::Scanning) {
    }
    const auto* summary = p.summary();
    assert(summary && summary->baselineApplied && !summary->rejected);
    assert(summary->overall.seconds == global["seconds"].as<uint64_t>());
    assert(summary->overall.sessions == global["sessions"].as<uint64_t>());
    assert(summary->books == books.size() && !summary->booksLimited);
    assert(summary->book.seconds == selected["seconds"].as<uint64_t>());
    assert(summary->book.sessions == selected["sessions"].as<uint64_t>());
    for (unsigned i = 0; i < 4; ++i) {
      assert(summary->overall.periods[i] == global["periods"][i].as<uint64_t>());
      assert(summary->book.periods[i] == selected["periods"][i].as<uint64_t>());
      uint64_t week = 0;
      for (unsigned d = 0; d < 7; ++d) {
        const auto date = std::to_string(summary->anchorDay - d);
        week += global["week"][date][i].as<uint64_t>();
      }
      assert(summary->weekPeriods[i] == week);
    }
    unsigned active = 0, bookActive = 0;
    for (uint32_t d = summary->anchorDay - HISTORY_DAYS + 1; d <= summary->anchorDay; ++d) {
      Day day;
      const auto key = std::to_string(d);
      assert(p.readDay(d, day));
      assert(day.seconds == global["days"][key].as<uint64_t>());
      assert(day.bookSeconds == selected["days"][key].as<uint64_t>());
      active += day.seconds != 0;
      bookActive += day.bookSeconds != 0;
    }
    assert(summary->activeDays == active && summary->bookActiveDays == bookActive);
    assert(p.aggregationBuffersBytes() == 0);
    assert(p.begin(id) == Status::Scanning && p.validatingCache());
    while (p.step() == Status::Scanning) {
    }
    assert(p.summary() && p.summary()->baselineApplied && p.parsedBytes() == 0);
    assert(p.summary()->overall.seconds == global["seconds"].as<uint64_t>());
    assert(p.summary()->book.seconds == selected["seconds"].as<uint64_t>());
  };
  for (const auto book : books) {
    verify(book["doc"].as<const char*>(), book["stats"].as<JsonObjectConst>());
  }
  for (size_t i = 0; i < p.summary()->books; ++i) {
    BookInfo info;
    assert(p.readBooks(i, &info, 1) == Status::Ready);
    bool found = false;
    for (const auto book : books) {
      DocumentId id{};
      assert(decodeDocumentId(book["doc"].as<const char*>(), id));
      if (id != info.id) continue;
      assert(std::string(info.title, info.titleLength) == book["title"].as<const char*>());
      assert(info.progress == (book["to"].is<uint16_t>() ? book["to"].as<uint16_t>() : UINT16_MAX));
      found = true;
    }
    assert(found);
  }
  // Verify latest post-baseline sessions still increment aggregate totals once.
  const auto id = books[0]["doc"].as<const char*>();
  verify(id, books[0]["stats"].as<JsonObjectConst>());
  const auto suffix = session(id, static_cast<int64_t>(p.summary()->anchorDay + 1) * 86400, 120);
  Storage.files[LOG] += suffix;
  assert(p.begin(id) == Status::Scanning && p.validatingCache());
  while (p.step() == Status::Scanning) {
  }
  assert(p.parsedBytes() == suffix.size() && p.summary()->baselineApplied);
  assert(p.summary()->overall.sessions == global["sessions"].as<uint64_t>() + 1);
  assert(p.summary()->overall.seconds == global["seconds"].as<uint64_t>() + 120);
  assert(p.summary()->book.seconds == books[0]["stats"]["seconds"].as<uint64_t>() + 120);
  // Interrupted or corrupt baseline must fail instead of reporting partial totals.
  const auto end = log.rfind(R"({"type":"rc-baseline-end)");
  assert(end != std::string::npos);
  Query q;
  q.begin();
  assert(q.feed(log.data(), end) == Status::Scanning);
  assert(q.finish() == Status::IoError && !q.summary());
  std::string corrupted = log;
  const auto row = corrupted.find("rc-baseline-total", static_cast<size_t>(report["prefix_bytes"].as<uint64_t>()));
  assert(row != std::string::npos);
  const auto digit = corrupted.find("seconds", row) + 9;
  assert(digit < corrupted.size() && corrupted[digit] >= '0' && corrupted[digit] <= '9');
  corrupted[digit] = corrupted[digit] == '9' ? '8' : '9';
  q.begin();
  assert(q.feed(corrupted.data(), corrupted.size()) == Status::IoError && !q.summary());
  q.begin();
  assert(q.feed(log.data(), log.size()) == Status::Scanning);
  const auto prefix = report["prefix_bytes"].as<size_t>();
  assert(q.feed(log.data() + prefix, log.size() - prefix) == Status::IoError);
  p.cancel();
  Storage.files.clear();
  std::puts(
      "PASS real Picco historical baseline, all book/day/period totals, warm cache, append, and invalid baseline "
      "rejection");
}

void extendedSessions() {
  Storage.files.clear();
  Storage.files[LOG] = std::string("{\"doc\":\"") + A +
                       "\",\"start\":1795564800,\"tz\":0,\"secs\":120,\"tracked_secs\":150,\"v\":2,\"turns\":7,"
                       "\"title\":\"Book\",\"path\":\"/Books/a.epub\",\"author\":\"Author\"}\n";
  Storage.files[LOG] += std::string("{\"doc\":\"") + A +
                        "\",\"start\":0,\"secs\":90,\"tracked_secs\":90,\"v\":2,\"turns\":3,\"title\":\"Book\","
                        "\"path\":\"/Books/a.epub\",\"author\":\"Author\"}\n";
  Provider p;
  auto verify = [&]() {
    auto status = p.begin(A, 0, true);
    while (status == Status::Scanning) status = p.step();
    assert(status == Status::Ready);
    const auto& s = *p.summary();
    assert(s.overall.seconds == 240 && s.overall.sessions == 2 && s.overall.datedSessions == 1);
    assert(s.overall.turns == 10 && s.overall.turnsComplete && s.book.turns == 10);
    Day day{};
    assert(p.readDay(s.anchorDay, day) && day.seconds == 120 && day.bookSeconds == 120);
    BookInfo info{};
    assert(p.readBooks(0, &info, 1) == Status::Ready && info.seconds == 240);
    assert(std::string(info.author) == "Author" && std::string(info.path) == "/Books/a.epub");
    uint8_t turns[24]{};
    assert(dessenReadingStats::read(p, 6, 0, turns, sizeof(turns)) == 24);
    assert(turns[0] == 1 && turns[4] == 3 && turns[8] == 10 && turns[16] == 10);
  };
  verify();
  verify();
  assert(p.parsedBytes() == 0);
  p.cancel();
  Storage.files.clear();
  std::puts("PASS extended local/dated durations, turns, metadata and cached detail");
}

void fixture(const char* path, uint64_t sessions, size_t books) {
  std::ifstream file(path, std::ios::binary);
  assert(file.good());
  Query q;
  assert(q.begin() == Status::Scanning);
  char chunk[4096];
  while (file.read(chunk, sizeof(chunk)) || file.gcount()) {
    assert(q.feed(chunk, file.gcount()) == Status::Scanning);
  }
  assert(!file.bad());
  q.finish();
  const auto& r = *q.summary();
  assert(r.overall.sessions == sessions && r.overall.seconds == sessions * 120);
  assert(r.overall.datedSessions == sessions && r.books == books && !r.booksLimited);
  assert(r.activeDays == 365 && !r.rejected && !r.incompleteTail);
  uint64_t periods = 0;
  for (const auto seconds : r.overall.periods) periods += seconds;
  assert(periods == r.overall.seconds);
  uint64_t days = 0;
  for (size_t i = 0; i < HISTORY_DAYS; ++i) days += q.dayAt(r.anchorDay - i).seconds;
  assert(days == r.overall.seconds);
  Storage.files.clear();
  file.clear();
  file.seekg(0);
  Storage.files[LOG].assign(std::istreambuf_iterator<char>(file), {});
  Provider provider;
  const auto sourceSize = Storage.files[LOG].size();
  assert(provider.begin() == Status::Scanning);
  while (provider.step() == Status::Scanning) {
  }
  assert(provider.parsedBytes() == sourceSize);
  compareProvider(provider, q);
  assert(provider.begin() == Status::Scanning && provider.validatingCache());
  while (provider.step() == Status::Scanning) {
  }
  assert(provider.parsedBytes() == 0);
  compareProvider(provider, q);
  const auto suffix = session(A, static_cast<int64_t>(r.anchorDay + 1) * 86400, 180);
  Storage.files[LOG] += suffix;
  assert(provider.begin() == Status::Scanning && provider.validatingCache());
  while (provider.step() == Status::Scanning) {
  }
  assert(provider.parsedBytes() == suffix.size());
  Query extended;
  assert(extended.begin() == Status::Scanning);
  feed(extended, Storage.files[LOG], 4096);
  assert(extended.finish() == Status::Ready);
  compareProvider(provider, extended);
  provider.cancel();
  Storage.files.clear();
  std::printf("PASS fixture: %llu sessions, %zu books, %llu seconds\n", static_cast<unsigned long long>(sessions),
              books, static_cast<unsigned long long>(r.overall.seconds));
}
}  // namespace

void sourceProvenance() {
  using namespace readingHistory;
  Storage.files.clear();
  const auto initial = session(A);
  Storage.files[LOG] = initial;
  Provider p;
  auto load = [&](uint64_t prefix = UINT64_MAX) {
    auto status = p.begin({}, 0, false, prefix);
    while (status == Status::Scanning) status = p.step();
    assert(status == Status::Ready);
  };
  uint8_t identity[40]{};
  load();
  assert(readSourceIdentity(p, 0, identity, sizeof(identity)) == 40 && identity[4] == 0);
  assert(readSourceIdentity(p, 1, nullptr, 0) == 0);
  const auto owned = Storage.files[LOG];
  assert(owned.starts_with(initial) && owned.find("rc-history-owner") != std::string::npos);
  load();
  assert(readSourceIdentity(p, 0, identity, sizeof(identity)) == 40 && identity[4] == 1);
  assert(p.summary()->overall.sessions == 1);
  const auto hash = p.verifiedSourceHash();
  const auto id = p.summary()->sourceId;
  assert(id != DocumentId{});
  assert(readSourceIdentity(p, 1, nullptr, 0) < 0);
  load();
  assert(p.parsedBytes() == 0 && p.summary()->sourceId == id && p.verifiedSourceHash() == hash);
  // Pinned prefix remains queryable after new reading is appended.
  auto tagged = session(B);
  tagged.insert(tagged.size() - 2, ",\"device\":\"a0a1a2a3a4a5\"");
  Storage.files[LOG] += tagged;
  load();
  assert(p.summary()->overall.sessions == 2 && p.summary()->sourceId == id);
  assert(readSourceIdentity(p, 0, identity, sizeof(identity)) == 40);
  load(owned.size());
  assert(p.summary()->overall.sessions == 1 && p.verifiedSourceHash() == hash);
  // Foreign events and untagged post-ownership events do not affect local
  // availability but prohibit exporting a claimed single-reader source.
  Storage.files[LOG] += initial;
  load();
  assert(p.summary()->sourceConflict && readSourceIdentity(p, 0, identity, sizeof(identity)) == DSN_ERR_DENIED);
  Storage.files[LOG] = owned;
  const auto macAt = Storage.files[LOG].find("a0a1a2a3a4a5");
  assert(macAt != std::string::npos);
  Storage.files[LOG].replace(macAt, 12, "b0b1b2b3b4b5");
  load();
  assert(readSourceIdentity(p, 0, identity, sizeof(identity)) == DSN_ERR_DENIED);
  assert(readSourceIdentity(p, 1, nullptr, 0) < 0);
  p.cancel();
  Storage.files.clear();
  puts("PASS generic ownership, foreign source rejection and pinned snapshots");
}

int main(int argc, char** argv) {
  edgeCases();
  wire();
  capacity();
  provider();
  fragmentedHistory();
  cacheCases();
  bookInfo();
  extendedSessions();
  sourceProvenance();
  std::puts("PASS query and provider regressions");
  if (argc == 5 || argc == 7) {
    fixture(argv[1], 1000, 10);
    fixture(argv[2], 10000, 10);
    fixture(argv[3], 100000, 10);
    fixture(argv[4], 100000, 1000);
    if (argc == 7) migrationFixture(argv[5], argv[6]);
  } else {
    assert(argc == 1);
  }
}
