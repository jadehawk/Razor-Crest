#include <ArduinoJson.h>
#include <HalStorage.h>
#include <freeink/DessenServices.h>
#include <sdk/dessen_abi.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "activities/plugins/DessenReadingHistory.h"
#include "sync/ReadingSessionLog.h"

namespace {
constexpr uint32_t day = 20000;
const std::string a(32, 'a'), b(32, 'b');
std::string session(const std::string& id, uint32_t date, uint32_t secs, const char* title = "Book",
                    int progress = 1234, const char* path = "/book.epub", int tz = 0) {
  JsonDocument d;
  d["doc"] = id;
  d["start"] = static_cast<uint64_t>(date) * 86400 + 9 * 3600;
  d["secs"] = secs;
  d["tz"] = tz;
  d["title"] = title;
  d["to"] = progress;
  d["path"] = path;
  std::string line;
  serializeJson(d, line);
  return line + "\n";
}
uint64_t get(const uint8_t* p, unsigned n = 8) {
  uint64_t x = 0;
  for (unsigned i = 0; i < n; ++i) x |= uint64_t(p[i]) << (8 * i);
  return x;
}
void finish(DessenReadingHistory& q, int expected = 0) {
  int rc;
  unsigned steps = 0;
  do {
    rc = q.step();
    assert(++steps < 10000);
  } while (rc == dessen::kPending);
  assert(rc == expected);
}
void normal() {
  fake::reset();
  const std::string source = session(b, day - 2, 30, "Other", 10000, "/other.epub") + session(a, day, 60) +
                             session(a, day - 1, 20, "Last title", 4567) + session(b, day - 400, 10) +
                             session(b, 0, 15) + "bad json\n" + std::string(700, 'x') + "\n" +
                             session(a, day, 99).substr(0, 20);
  fake::add(readingSessionLog::LOG_PATH, source);
  DessenReadingHistory q;
  assert(q.begin(a, 0) == dessen::kPending);
  uint8_t data[1024]{};
  assert(q.read(DSN_STATS_SUMMARY, 0, data, 192) == DSN_ERR_INVALID);
  finish(q);
  assert(q.read(DSN_STATS_SUMMARY, 0, data, 192) == 192);
  assert(get(data, 4) == 1 && get(data + 4, 4) == DSN_STATS_INCOMPLETE_TAIL);
  assert(get(data + 8, 4) == day && get(data + 12, 4) == day);
  assert(get(data + 16, 4) == 3 && get(data + 20, 4) == 2 && get(data + 24, 4) == 2);
  assert(get(data + 32) == source.size() && get(data + 40) == 2);
  assert(get(data + 48) == 135 && get(data + 56) == 5 && get(data + 64) == 4);
  assert(get(data + 104) == 80 && get(data + 112) == 2 && get(data + 160) == 110);
  assert(q.read(DSN_STATS_DAYS, day - 3, data, 64) == 64);
  assert(get(data) == 0 && get(data + 16) == 30 && get(data + 32) == 20 && get(data + 40) == 20 &&
         get(data + 48) == 60 && get(data + 56) == 60);
  assert(q.read(DSN_STATS_BOOKS, 0, data, 32) == 32 && data[0] == 0xaa && data[16] == 0xbb);
  memset(data, 0x55, sizeof data);
  assert(q.read(DSN_STATS_BOOK_INFO, 0, data, 200) == 200);
  assert(get(data + 16, 2) == 4567 && data[18] == 10 && data[19] == 0);
  assert(std::string(reinterpret_cast<char*>(data + 20), 10) == "Last title" && data[30] == 0);
  assert(q.read(DSN_STATS_BOOKS, 2, data, 16) == 0);
  assert(q.read(DSN_STATS_BOOK_INFO, 2, data, 100) == 0);
  assert(q.read(DSN_STATS_DAYS, day + 1, data, 16) == DSN_ERR_INVALID);
  assert(q.read(DSN_STATS_DAYS, day, data, 17) == DSN_ERR_INVALID);
  assert(q.read(DSN_STATS_SUMMARY, 0, data, 191) == DSN_ERR_TOO_BIG);
  assert(fake::writesByPath.empty());
  assert(fake::files.at(readingSessionLog::LOG_PATH)->bytes.size() == source.size());
}
void anchorAndSnapshot() {
  fake::reset();
  auto source = session(a, day + 1, 25) + session(b, day - 370, 10) + session(a, day, 60) + session(a, day - 371, 15);
  fake::add(readingSessionLog::LOG_PATH, source);
  DessenReadingHistory q;
  assert(q.begin("/book.epub", day) == dessen::kPending);
  auto node = fake::files.at(readingSessionLog::LOG_PATH);
  auto extra = session(a, day, 900);
  node->bytes.insert(node->bytes.end(), extra.begin(), extra.end());
  finish(q);
  uint8_t data[192]{};
  assert(q.read(DSN_STATS_SUMMARY, 0, data, sizeof data) == 192);
  assert(get(data + 8, 4) == day && get(data + 12, 4) == day + 1);
  assert(get(data + 48) == 110 && get(data + 104) == 110);
  assert(get(data + 16, 4) == 2 && get(data + 160) == 60);
  assert(q.read(DSN_STATS_DAYS, day - 370, data, 16) == 16 && get(data) == 10);
  assert(q.read(DSN_STATS_DAYS, day, data, 16) == 16 && get(data) == 60);
}
void errors() {
  fake::reset();
  DessenReadingHistory missing;
  assert(missing.begin({}, 0) == DSN_STATS_NO_HISTORY);
  fake::add(readingSessionLog::LOG_PATH, session(a, day, 60));
  {
    DessenReadingHistory q;
    assert(q.begin({}, 0) == dessen::kPending);
    fake::failRead = 0;
    finish(q, DSN_ERR_IO);
  }
  {
    DessenReadingHistory q;
    assert(q.begin({}, 0) == dessen::kPending);
    fake::failAlloc = 0;
    finish(q, DSN_STATS_NO_MEMORY);
  }
  {
    DessenReadingHistory q;
    assert(q.begin({}, 0) == dessen::kPending);
    finish(q);
    uint8_t out[100];
    memset(out, 0x55, sizeof out);
    fake::failRead = 0;
    assert(q.read(DSN_STATS_BOOK_INFO, 0, out, sizeof out) == DSN_ERR_IO);
    assert(std::all_of(std::begin(out), std::end(out), [](auto v) { return v == 0x55; }));
    fake::failAlloc = 0;
    assert(q.read(DSN_STATS_BOOK_INFO, 0, out, sizeof out) == DSN_STATS_NO_MEMORY);
  }
}
void bookLimit() {
  fake::reset();
  std::string source;
  for (unsigned i = 1025; i > 0; --i) {
    char id[33];
    snprintf(id, sizeof id, "%032x", i);
    source += session(id, day, 10);
  }
  fake::add(readingSessionLog::LOG_PATH, source);
  DessenReadingHistory q;
  assert(q.begin({}, 0) == dessen::kPending);
  finish(q);
  uint8_t out[192]{};
  assert(q.read(DSN_STATS_SUMMARY, 0, out, sizeof out) == 192);
  assert(get(out + 4, 4) == DSN_STATS_BOOKS_LIMITED && get(out + 24, 4) == 1024);
  assert(get(out + 48) == 10250 && get(out + 56) == 1025);
  assert(q.read(DSN_STATS_BOOKS, 1023, out, 32) == 16);
}
}  // namespace

int main() {
  normal();
  anchorAndSnapshot();
  errors();
  bookLimit();
  puts("Dessen reading history: PASS (snapshot, calendar, books, malformed input, memory and I/O failures, limits)");
}
