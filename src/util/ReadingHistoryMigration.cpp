#include "ReadingHistoryMigration.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ReadingHistory.h"
#include "ReadingStatsBookIndex.h"

namespace readingHistory {
namespace {
using readingstats::BookEntry;
using readingstats::Snapshot;
using readingstats::TimeBuckets;
constexpr char DIR[] = "/.crosspoint/reading-stats";
constexpr char LOG[] = "/.crosspoint/reading_sessions.jsonl";
constexpr char TEMP[] = "/.crosspoint/reading_sessions.migration.tmp";
constexpr char BACKUP[] = "/.crosspoint/reading_sessions.migration.bak";
constexpr char RECEIPT[] = "/.crosspoint/reading_sessions.migration.json";
constexpr char RECEIPT_TEMP[] = "/.crosspoint/reading_sessions.migration.json.tmp";
constexpr char CATALOG[] = "/.crosspoint/reading-stats/books_v1.bin";
constexpr uint64_t HASH_INIT = 14695981039346656037ULL;
constexpr size_t CHUNK = 4096;
bool ready = false;
void (*yieldMigrationWork)() = nullptr;
void yieldWork() {
  if (yieldMigrationWork) yieldMigrationWork();
}

uint64_t hash64(uint64_t h, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}
uint32_t hash32(uint32_t h, const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<uint8_t>(data[i])) * 16777619u;
  return h;
}
bool removeIfPresent(const char* path) { return !Storage.exists(path) || Storage.remove(path); }

struct Receipt {
  bool hadLog = false;
  uint64_t oldSize = 0, oldHash = HASH_INIT, newSize = 0, newHash = HASH_INIT;
};

bool readReceipt(Receipt& receipt) {
  HalFile file;
  char buf[256];
  if (!Storage.openFileForRead("RMIG", RECEIPT, file) || file.isDirectory() || file.fileSize64() >= sizeof(buf))
    return false;
  const size_t n = file.fileSize64();
  if (file.read(buf, n) != static_cast<int>(n)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, buf, n) || doc["v"] != 1 || !doc["had_log"].is<bool>() || !doc["old_size"].is<uint64_t>() ||
      !doc["old_hash"].is<uint64_t>() || !doc["new_size"].is<uint64_t>() || !doc["new_hash"].is<uint64_t>())
    return false;
  receipt = {doc["had_log"].as<bool>(), doc["old_size"].as<uint64_t>(), doc["old_hash"].as<uint64_t>(),
             doc["new_size"].as<uint64_t>(), doc["new_hash"].as<uint64_t>()};
  return receipt.newSize > receipt.oldSize;
}

bool writeReceipt(const Receipt& r) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["had_log"] = r.hadLog;
  doc["old_size"] = r.oldSize;
  doc["old_hash"] = r.oldHash;
  doc["new_size"] = r.newSize;
  doc["new_hash"] = r.newHash;
  char buf[256];
  const size_t n = measureJson(doc);
  if (doc.overflowed() || n >= sizeof(buf) || serializeJson(doc, buf, sizeof(buf)) != n) return false;
  HalFile file;
  if (!Storage.openFileForWrite("RMIG", RECEIPT_TEMP, file)) return false;
  const bool written = file.write(buf, n) == n;
  const bool closed = file.close();
  return written && closed && Storage.rename(RECEIPT_TEMP, RECEIPT);
}

bool matches(const char* path, uint64_t size, uint64_t hash, char* buffer, bool allowSuffix = false) {
  HalFile file;
  if (!Storage.openFileForRead("RMIG", path, file) || file.isDirectory() || file.fileSize64() < size ||
      (!allowSuffix && file.fileSize64() != size))
    return false;
  uint64_t actual = HASH_INIT;
  while (size) {
    const size_t n = std::min<uint64_t>(size, CHUNK);
    if (file.read(buffer, n) != static_cast<int>(n)) return false;
    actual = hash64(actual, buffer, n);
    size -= n;
    yieldWork();
  }
  return actual == hash;
}

// The receipt is published only after the candidate has been fully parsed
// and verified. Never delete originals until the exact verified prefix is
// installed at the canonical log path. Cleanup can be retried independently.
bool recover(const Receipt& r, char* buffer) {
  if (!matches(LOG, r.newSize, r.newHash, buffer, true)) {
    if (!matches(TEMP, r.newSize, r.newHash, buffer)) {
      // Do not overwrite any existing log. If publication moved the old file
      // but never installed the candidate, restore the verified original.
      if (!Storage.exists(LOG) && Storage.exists(DIR) && r.hadLog && matches(BACKUP, r.oldSize, r.oldHash, buffer) &&
          Storage.rename(BACKUP, LOG)) {
        LOG_ERR("RMIG", "Candidate invalid; original history restored");
        removeIfPresent(TEMP);
        removeIfPresent(RECEIPT_TEMP);
        removeIfPresent(RECEIPT);
      }
      return false;
    }
    if (Storage.exists(LOG)) {
      if (!r.hadLog || Storage.exists(BACKUP) || !matches(LOG, r.oldSize, r.oldHash, buffer) ||
          !Storage.rename(LOG, BACKUP))
        return false;
    } else if (r.hadLog && !matches(BACKUP, r.oldSize, r.oldHash, buffer)) {
      return false;
    }
    if (!Storage.rename(TEMP, LOG) || !matches(LOG, r.newSize, r.newHash, buffer)) return false;
  }
  ready = true;
  // Covers are derived, but some books have no original file left. Retain the
  // existing previews when possible, independently of the old BIN store.
  if (Storage.exists("/.crosspoint/reading-stats/covers") && !Storage.exists("/.crosspoint/reading-history-covers") &&
      !Storage.rename("/.crosspoint/reading-stats/covers", "/.crosspoint/reading-history-covers")) {
    LOG_ERR("RMIG", "Verified migration; cover transfer will retry");
    return true;
  }
  if ((Storage.exists(DIR) && !Storage.removeDir(DIR)) || Storage.exists(DIR) || !removeIfPresent(BACKUP) ||
      !removeIfPresent(TEMP) || !removeIfPresent(RECEIPT_TEMP) || !removeIfPresent(RECEIPT)) {
    LOG_ERR("RMIG", "Verified migration; legacy cleanup will retry");
  } else {
    LOG_INF("RMIG", "Verified historical migration; legacy BIN store removed");
  }
  return true;
}

template <class T>
bool readSnapshot(const std::string& path, T& out, bool required) {
  if (!Storage.exists(path.c_str())) {
    // A missing primary with a backup needs explicit recovery, not a guess.
    return !required && !Storage.exists((path + ".bak").c_str());
  }
  HalFile f;
  return Storage.openFileForRead("RMIG", path, f) && !f.isDirectory() && f.fileSize64() == sizeof(T) &&
         f.read(&out, sizeof(T)) == static_cast<int>(sizeof(T)) && out.valid();
}

struct Mapping {
  uint64_t pathHash = 0;
  DocumentId id{};
  uint64_t priorSeconds = 0;
  uint16_t progress = UINT16_MAX;
};

void hexId(const DocumentId& id, char (&out)[33]) {
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < id.size(); ++i) {
    out[2 * i] = hex[id[i] >> 4];
    out[2 * i + 1] = hex[id[i] & 15];
  }
  out[32] = 0;
}

bool validDay(uint32_t ordinal) { return !ordinal || (ordinal >= 10958 && ordinal <= 47483); }

bool validStats(const Snapshot& s, const TimeBuckets& t) {
  if (!validDay(s.latestDay) || !validDay(t.latestDay) || t.lastSequence > s.lastSequence) return false;
  uint64_t daily = 0, periods = 0;
  for (uint32_t i = 0; i < readingstats::kHistoryDays && i < s.latestDay; ++i) daily += s.secondsOnDay(s.latestDay - i);
  for (auto n : t.total) periods += n;
  return daily <= s.totalSeconds() && periods <= s.totalSeconds();
}

struct Writer {
  HalFile& file;
  char* line;
  uint32_t records = 0, hash = 2166136261u;
  bool row(const JsonDocument& doc, bool body = true) {
    const size_t n = measureJson(doc);
    if (doc.overflowed() || n > MAX_LINE || serializeJson(doc, line, MAX_LINE + 1) != n) return false;
    line[n] = '\n';
    if (file.write(line, n + 1) != n + 1) return false;
    if (body) {
      hash = hash32(hash, line, n + 1);
      ++records;
    }
    return true;
  }
};

bool emitScope(Writer& writer, const Snapshot& stats, const TimeBuckets& time, const BookEntry* book,
               const Mapping* mapping, bool totalsOnly) {
  JsonDocument doc;
  char id[33];
  if (mapping) {
    hexId(mapping->id, id);
    doc["doc"] = id;
  }
  doc["v"] = 1;
  if (totalsOnly) {
    doc["type"] = "rc-baseline-total";
    doc["seconds"] = stats.totalSeconds();
    doc["sessions"] = stats.sessions;
    doc["turns"] = stats.pageTurns;
    doc["latest"] = stats.latestDay ? stats.latestDay - 1 : 0;
    auto periods = doc["periods"].to<JsonArray>();
    for (auto n : time.total) periods.add(n);
    if (book) {
      doc["title"] = book->title;
      doc["author"] = book->author;
      doc["path"] = book->path;
      if (mapping->progress != UINT16_MAX) doc["to"] = mapping->progress;
      if (measureJson(doc) > MAX_LINE) doc.remove("path");
      if (measureJson(doc) > MAX_LINE) doc.remove("author");
      if (measureJson(doc) > MAX_LINE) doc.remove("title");
    }
    return writer.row(doc);
  }
  doc["type"] = "rc-baseline-day";
  for (uint32_t i = 0; i < readingstats::kHistoryDays && i < stats.latestDay; ++i) {
    const uint32_t ordinal = stats.latestDay - i;
    const auto seconds = stats.secondsOnDay(ordinal);
    if (!seconds) continue;
    doc["day"] = ordinal - 1;
    doc["seconds"] = seconds;
    if (!writer.row(doc)) return false;
  }
  doc.remove("seconds");
  doc["type"] = "rc-baseline-week";
  for (uint32_t i = 0; i < readingstats::kTimeHistoryDays && i < time.latestDay; ++i) {
    const uint32_t ordinal = time.latestDay - i;
    const auto& values = time.recent[ordinal % readingstats::kTimeHistoryDays];
    if (!(values[0] || values[1] || values[2] || values[3])) continue;
    doc["day"] = ordinal - 1;
    auto periods = doc["periods"].to<JsonArray>();
    for (auto n : values) periods.add(n);
    if (!writer.row(doc)) return false;
  }
  return true;
}

// Validate the source record and bind each old session to a known catalog
// document. Ambiguous identity or history not covered by RST2 blocks cutover.
bool inspectSession(char* line, size_t length, Mapping* mappings, size_t count, uint64_t& seconds) {
  if (!length || line[0] != '{' || line[length - 1] != '}') return false;
  JsonDocument doc;
  if (deserializeJson(doc, line, length) || !doc.is<JsonObject>() ||
      (!doc["type"].isNull() && doc["type"] != "session") || (!doc["v"].isNull() && doc["v"] != 1))
    return false;
  DocumentId id{};
  const auto text = doc["doc"].as<JsonString>();
  if (text.isNull() || !decodeDocumentId({text.c_str(), text.size()}, id) || !doc["secs"].is<uint32_t>() ||
      doc["secs"].as<uint32_t>() < 10 || doc["secs"].as<uint32_t>() > 604800)
    return false;
  for (size_t i = 0; i < count; ++i) {
    auto& m = mappings[i];
    if (m.id != id) continue;
    const auto path = doc["path"].as<JsonString>();
    if (!path.isNull() && hash64(HASH_INIT, path.c_str(), path.size()) != m.pathHash) return false;
    const auto n = doc["secs"].as<uint32_t>();
    m.priorSeconds += n;
    seconds += n;
    if (doc["to"].is<uint16_t>() && doc["to"].as<uint16_t>() <= 10000) m.progress = doc["to"].as<uint16_t>();
    return true;
  }
  return false;
}

// Recognize only native legacy files. Reject orphans instead of deleting data
// that cannot be represented by a verified catalog entry.
bool checkDirectory(const Mapping* mappings, size_t count) {
  auto dir = Storage.open(DIR);
  if (!dir || !dir.isDirectory()) return false;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    char name[96]{};
    if (!file.getName(name, sizeof(name))) return false;
    const std::string_view n(name);
    if (file.isDirectory()) {
      if (n != "covers") return false;
      continue;
    }
    if (n == "global_v2.bin" || n == "global_v2.bin.bak" || n == "time_global_v1.bin" ||
        n == "time_global_v1.bin.bak" || n == "books_v1.bin" || n == "books_v1.bin.bak" ||
        n == "import_receipts_v1.bin")
      continue;
    if (n.size() < 28 || !(n.starts_with("book_") || n.starts_with("time_"))) return false;
    const auto suffix = n.substr(21);
    if ((n.starts_with("book_") && suffix != "_v2.bin" && suffix != "_v2.bin.bak") ||
        (n.starts_with("time_") && suffix != "_v1.bin" && suffix != "_v1.bin.bak"))
      return false;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      char hex[17];
      snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(mappings[i].pathHash));
      if (n.substr(5, 16) == hex) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

bool prepare(ResolveDocument resolve, Receipt& receipt, char* buffer) {
  auto global = makeUniqueNoThrow<Snapshot>();
  auto book = makeUniqueNoThrow<Snapshot>();
  auto entry = makeUniqueNoThrow<BookEntry>();
  auto line = makeUniqueNoThrow<char[]>(MAX_LINE + 2);
  if (!global || !book || !entry || !line) return false;
  TimeBuckets globalTime{}, time{};
  if (!readSnapshot(std::string(DIR) + "/global_v2.bin", *global, false) ||
      !readSnapshot(std::string(DIR) + "/time_global_v1.bin", globalTime, false) || !validStats(*global, globalTime))
    return false;
  HalFile catalog;
  const bool hasCatalog = Storage.exists(CATALOG);
  if (!hasCatalog && Storage.exists("/.crosspoint/reading-stats/books_v1.bin.bak")) return false;
  if (hasCatalog && (!Storage.openFileForRead("RMIG", CATALOG, catalog) || catalog.isDirectory() ||
                     catalog.fileSize64() % sizeof(BookEntry) || catalog.fileSize64() / sizeof(BookEntry) > 2048))
    return false;
  const size_t count = hasCatalog ? catalog.fileSize64() / sizeof(BookEntry) : 0;
  auto mappings = makeUniqueNoThrow<Mapping[]>(std::max<size_t>(1, count));
  if (!mappings) return false;
  for (size_t i = 0; i < count; ++i) {
    if (catalog.read(entry.get(), sizeof(BookEntry)) != sizeof(BookEntry) || !entry->valid() || entry->path[0] != '/' ||
        hash64(HASH_INIT, entry->path, strlen(entry->path)) != entry->hash())
      return false;
    auto& m = mappings[i];
    m.pathHash = entry->hash();
    if (!decodeDocumentId(resolve(entry->path), m.id)) return false;
    for (size_t j = 0; j < i; ++j)
      if (m.id == mappings[j].id || m.pathHash == mappings[j].pathHash) return false;
    yieldWork();
  }
  if (!checkDirectory(mappings.get(), count)) return false;

  HalFile dest;
  if (!Storage.openFileForWrite("RMIG", TEMP, dest)) return false;
  receipt.hadLog = Storage.exists(LOG);
  uint64_t prior = 0;
  if (receipt.hadLog) {
    HalFile old;
    if (!Storage.openFileForRead("RMIG", LOG, old) || old.isDirectory()) return false;
    receipt.oldSize = old.fileSize64();
    uint64_t copied = 0;
    size_t length = 0;
    while (copied < receipt.oldSize) {
      const size_t n = std::min<uint64_t>(CHUNK, receipt.oldSize - copied);
      if (old.read(buffer, n) != static_cast<int>(n) || dest.write(buffer, n) != n) return false;
      receipt.oldHash = hash64(receipt.oldHash, buffer, n);
      for (size_t j = 0; j < n; ++j) {
        const char c = buffer[j];
        if (c == '\n') {
          if (length && line[length - 1] == '\r') --length;
          if (!inspectSession(line.get(), length, mappings.get(), count, prior)) return false;
          length = 0;
        } else {
          if (length >= MAX_LINE + 1) return false;
          line[length++] = c;
        }
      }
      copied += n;
      yieldWork();
    }
    if (length || prior > global->totalSeconds()) return false;
  }
  // Fixed-width decimal with spaces (valid JSON) allows us to patch the count
  // after streaming the body without storing a second copy of it in memory.
  auto beginLine = [&](unsigned records) {
    return snprintf(line.get(), MAX_LINE + 2, "{\"type\":\"rc-baseline-begin\",\"v\":1,\"records\":%8u}\n", records);
  };
  const int beginSize = beginLine(0);
  if (beginSize <= 0 || dest.write(line.get(), beginSize) != static_cast<size_t>(beginSize)) return false;
  Writer writer{dest, line.get()};
  for (const bool totals : {true, false}) {
    if (!emitScope(writer, *global, globalTime, nullptr, nullptr, totals) || (hasCatalog && !catalog.seek64(0)))
      return false;
    for (size_t i = 0; i < count; ++i) {
      if (catalog.read(entry.get(), sizeof(BookEntry)) != sizeof(BookEntry) || !entry->valid() ||
          entry->hash() != mappings[i].pathHash)
        return false;
      char suffix[17];
      snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(entry->hash()));
      *book = {};
      time = {};
      if (!readSnapshot(std::string(DIR) + "/book_" + suffix + "_v2.bin", *book, false) ||
          !readSnapshot(std::string(DIR) + "/time_" + suffix + "_v1.bin", time, false) || !validStats(*book, time) ||
          book->lastSequence > global->lastSequence || mappings[i].priorSeconds > book->totalSeconds() ||
          !emitScope(writer, *book, time, entry.get(), &mappings[i], totals))
        return false;
      yieldWork();
    }
  }
  JsonDocument end;
  end["type"] = "rc-baseline-end";
  end["v"] = 1;
  end["hash"] = writer.hash;
  if (!writer.row(end, false)) return false;
  receipt.newSize = dest.fileSize64();
  if (beginLine(writer.records) != beginSize || !dest.seek64(receipt.oldSize) ||
      dest.write(line.get(), beginSize) != static_cast<size_t>(beginSize) || !dest.close())
    return false;
  if (hasCatalog) catalog.close();
  mappings.reset();
  book.reset();
  entry.reset();
  line.reset();

  // Independently parse the reopened candidate before any rename/cleanup.
  auto query = makeUniqueNoThrow<Query>();
  HalFile candidate;
  if (!query || query->begin() != Status::Scanning || !Storage.openFileForRead("RMIG", TEMP, candidate) ||
      candidate.fileSize64() != receipt.newSize)
    return false;
  uint64_t read = 0, prefixHash = HASH_INIT;
  while (read < receipt.newSize) {
    const size_t n = std::min<uint64_t>(CHUNK, receipt.newSize - read);
    if (candidate.read(buffer, n) != static_cast<int>(n)) return false;
    receipt.newHash = hash64(receipt.newHash, buffer, n);
    if (read < receipt.oldSize) prefixHash = hash64(prefixHash, buffer, std::min<uint64_t>(n, receipt.oldSize - read));
    if (query->feed(buffer, n) != Status::Scanning) return false;
    read += n;
    yieldWork();
  }
  if (prefixHash != receipt.oldHash || query->finish() != Status::Ready) return false;
  const auto& s = *query->summary();
  if (!s.baselineApplied || s.incompleteTail || s.rejected || s.booksLimited != (count > MAX_BOOKS) ||
      s.books != std::min(count, MAX_BOOKS) || s.overall.seconds != global->totalSeconds() ||
      s.overall.sessions != global->sessions || s.overall.turns != global->pageTurns)
    return false;
  for (unsigned p = 0; p < 4; ++p)
    if (s.overall.periods[p] != globalTime.total[p]) return false;
  for (uint32_t i = 0; i < readingstats::kHistoryDays && i < global->latestDay; ++i) {
    const uint32_t ordinal = global->latestDay - i;
    if (query->dayAt(ordinal - 1).seconds != global->secondsOnDay(ordinal)) return false;
  }
  LOG_INF("RMIG", "Verified candidate sessions=%llu books=%u bytes=%llu",
          static_cast<unsigned long long>(s.overall.sessions), static_cast<unsigned>(s.books),
          static_cast<unsigned long long>(receipt.newSize));
  return true;
}
}  // namespace

bool migrationReady() { return ready; }

bool migrationPending() { return Storage.exists(RECEIPT) || Storage.exists(BACKUP); }

bool migrateLegacy(ResolveDocument resolve, bool (*recoverLegacy)(), void (*yieldCallback)()) {
  yieldMigrationWork = yieldCallback;
  ready = false;
  if (!Storage.exists(DIR) && !Storage.exists(RECEIPT) && !Storage.exists(BACKUP)) {
    ready = true;
    return true;
  }
  auto buffer = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buffer) return false;
  Receipt receipt;
  if (Storage.exists(RECEIPT)) {
    if (readReceipt(receipt) && recover(receipt, buffer.get())) return true;
    LOG_ERR("RMIG", "Interrupted migration needs recovery; originals retained");
    return false;
  }
  if (Storage.exists(BACKUP) || !resolve || (recoverLegacy && !recoverLegacy()) ||
      !prepare(resolve, receipt, buffer.get()) || !writeReceipt(receipt) || !recover(receipt, buffer.get())) {
    LOG_ERR("RMIG", "Migration not committed; originals retained");
    return false;
  }
  return true;
}
}  // namespace readingHistory
