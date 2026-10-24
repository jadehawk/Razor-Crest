#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <new>
#include <string>

#include "ReadingStatsLegacyMerge.h"
#include "ReadingStatsBookMedia.h"

namespace readingstats {
namespace {
constexpr char kDir[] = "/.crosspoint/reading-stats";
constexpr char kGlobal[] = "/.crosspoint/reading-stats/global_v2.bin";
constexpr char kPending[] = "/.crosspoint/reading-stats/pending_v1.bin";
constexpr char kLegacyPending[] = "/.crosspoint/reading-stats/import_pending_v1.bin";
constexpr char kLegacyReceipts[] = "/.crosspoint/reading-stats/import_receipts_v1.bin";

// A source/book is imported at most once. The receipt is durable only after
// the book/global snapshots are safely written. A leftover pending marker
// replays idempotently before new native reading sessions are recorded.
struct ImportReceipt {
  uint32_t magic = 0x31505249;  // IRP1
  uint32_t source = 0;
  uint32_t keyLo = 0;
  uint32_t keyHi = 0;
  uint32_t checksum = 0;
  void seal() { checksum = checksumBytes(this, offsetof(ImportReceipt, checksum)); }
  bool valid() const {
    return magic == 0x31505249 && source >= 1 && source <= 2 &&
           checksum == checksumBytes(this, offsetof(ImportReceipt, checksum));
  }
  uint64_t key() const { return (static_cast<uint64_t>(keyHi) << 32) | keyLo; }
};
static_assert(sizeof(ImportReceipt) == 20);

struct ImportPending {
  uint32_t magic = 0x31504D49;  // IMP1
  uint32_t source = 0;
  uint32_t sequence = 0;
  uint32_t flags = 1;  // bit 0: add historical totals to global snapshot
  uint32_t keyLo = 0;
  uint32_t keyHi = 0;
  uint32_t bookLo = 0;
  uint32_t bookHi = 0;
  Snapshot delta{};
  uint32_t checksum = 0;
  uint64_t key() const { return (static_cast<uint64_t>(keyHi) << 32) | keyLo; }
  uint64_t bookHash() const { return (static_cast<uint64_t>(bookHi) << 32) | bookLo; }
  void seal() {
    delta.seal();
    checksum = checksumBytes(this, offsetof(ImportPending, checksum));
  }
  bool valid() const {
    return magic == 0x31504D49 && source >= 1 && source <= 2 && sequence && (flags & ~1u) == 0 && delta.valid() &&
           checksum == checksumBytes(this, offsetof(ImportPending, checksum));
  }
};

enum class ReceiptLookup { Missing, Already, OtherSource, Invalid };
bool recoverImportPending();

uint64_t pathHash(const char* path) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    hash = (hash ^ *p) * 1099511628211ULL;
  return hash;
}

std::string bookFile(const uint64_t hash) {
  char name[96];
  snprintf(name, sizeof(name), "%s/book_%016llx_v2.bin", kDir, static_cast<unsigned long long>(hash));
  return name;
}

template <typename Record>
bool readValid(const std::string& path, Record& out) {
  HalFile file;
  if (!Storage.openFileForRead("RST", path, file)) return false;
  const int count = file.read(reinterpret_cast<uint8_t*>(&out), sizeof(out));
  file.close();
  return count == static_cast<int>(sizeof(out)) && out.valid();
}

bool loadPath(const std::string& path, Snapshot& out) {
  if (readValid(path, out) || readValid(path + ".bak", out)) return true;
  out = Snapshot{};
  return false;
}

bool ensureDir() {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  return Storage.exists(kDir) || Storage.mkdir(kDir);
}

template <typename Record>
bool writeTemp(const std::string& path, const Record& value) {
  const std::string tmp = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("RST", tmp, file)) return false;
  const bool complete = file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
  file.flush();
  const bool closed = file.close();
  if (complete && closed) return true;
  Storage.remove(tmp.c_str());
  return false;
}

bool savePath(const std::string& path, Snapshot& stats) {
  if (!ensureDir()) return false;
  stats.seal();
  if (!writeTemp(path, stats)) return false;
  const std::string tmp = path + ".tmp";
  const std::string backup = path + ".bak";
  const bool hadOld = Storage.exists(path.c_str());
  if (hadOld) {
    Storage.remove(backup.c_str());
    if (!Storage.rename(path.c_str(), backup.c_str())) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (Storage.rename(tmp.c_str(), path.c_str())) return true;
  if (hadOld) Storage.rename(backup.c_str(), path.c_str());
  return false;
}

uint32_t localDayOrdinal(const int64_t epoch) {
  if (epoch <= 0) return 0;
  const time_t t = static_cast<time_t>(epoch);
  struct tm local{};
  if (!localtime_r(&t, &local)) return 0;
  int year = local.tm_year + 1900;
  const unsigned month = static_cast<unsigned>(local.tm_mon + 1);
  const unsigned day = static_cast<unsigned>(local.tm_mday);
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned shiftedMonth = month > 2 ? month - 3 : month + 9;
  const unsigned doy = (153 * shiftedMonth + 2) / 5 + day - 1;
  const unsigned yoeDays = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + yoeDays - 719468;
  return days >= 0 && days < UINT32_MAX - 1 ? static_cast<uint32_t>(days + 1) : 0;
}

bool apply(const Pending& txn, Snapshot& stats, const std::string& path) {
  if (!applyOnce(stats, txn)) return true;
  return savePath(path, stats);
}

// One persistent transaction marker is applied to both snapshots by sequence.
// Replaying it after a crash cannot double-count an already-written snapshot.
bool recoverPending() {
  if (!Storage.exists(kPending)) return recoverImportPending();
  Pending txn;
  if (!readValid(kPending, txn)) {
    LOG_ERR("RST", "Invalid transaction marker; statistics need inspection");
    return false;
  }
  const std::string path = bookFile(txn.bookHash());
  Snapshot book;
  loadPath(path, book);
  if (!apply(txn, book, path)) return false;
  Snapshot global;
  loadPath(kGlobal, global);
  if (!apply(txn, global, kGlobal)) return false;
  return Storage.remove(kPending) && recoverImportPending();
}

bool writePending(Pending& txn) {
  if (!ensureDir()) return false;
  txn.seal();
  if (!writeTemp(kPending, txn)) return false;
  return Storage.rename((std::string(kPending) + ".tmp").c_str(), kPending);
}

// A torn *trailing* receipt is recoverable while the pending journal exists.
// Existing valid receipts and corruption in earlier entries remain untouched.
bool validateReceipts(const bool pendingJournal) {
  if (!Storage.exists(kLegacyReceipts)) return true;
  HalFile file = Storage.open(kLegacyReceipts, O_RDWR);
  if (!file) return false;
  uint64_t size = file.fileSize();
  const uint64_t complete = size / sizeof(ImportReceipt);
  if (complete > 4096) {
    file.close();
    return false;
  }
  for (uint64_t i = 0; i < complete; ++i) {
    ImportReceipt item{};
    if (file.read(&item, sizeof(item)) != sizeof(item) || !item.valid()) {
      // A whole corrupt receipt might predate this transaction. Never
      // discard it and risk counting already-imported history a second time.
      file.close();
      return false;
    }
  }
  if (size % sizeof(ImportReceipt) != 0) {
    if (!pendingJournal || !file.truncate(complete * sizeof(ImportReceipt))) {
      file.close();
      return false;
    }
  }
  file.close();
  return true;
}

ReceiptLookup findReceipt(const uint32_t source, const uint64_t key) {
  if (!Storage.exists(kLegacyReceipts)) return ReceiptLookup::Missing;
  HalFile file;
  if (!Storage.openFileForRead("RST", kLegacyReceipts, file)) return ReceiptLookup::Invalid;
  if (file.fileSize() % sizeof(ImportReceipt)) {
    file.close();
    return ReceiptLookup::Invalid;
  }
  const uint64_t count = file.fileSize() / sizeof(ImportReceipt);
  if (count > 4096) {
    file.close();
    return ReceiptLookup::Invalid;
  }
  bool seen = false;
  bool otherSource = false;
  for (uint64_t i = 0; i < count; ++i) {
    ImportReceipt item{};
    if (file.read(&item, sizeof(item)) != sizeof(item) || !item.valid()) {
      file.close();
      return ReceiptLookup::Invalid;
    }
    if (item.source == source && item.key() == key) seen = true;
    if (item.source != source) otherSource = true;
  }
  file.close();
  return seen ? ReceiptLookup::Already : (otherSource ? ReceiptLookup::OtherSource : ReceiptLookup::Missing);
}

bool writeReceipt(const ImportPending& txn) {
  if (findReceipt(txn.source, txn.key()) != ReceiptLookup::Missing) return false;
  ImportReceipt item{};
  item.source = txn.source;
  item.keyLo = txn.keyLo;
  item.keyHi = txn.keyHi;
  item.seal();
  HalFile file = Storage.open(kLegacyReceipts, O_RDWR | O_CREAT);
  if (!file || !file.seek64(file.fileSize())) {
    if (file) file.close();
    return false;
  }
  const bool complete = file.write(&item, sizeof(item)) == sizeof(item);
  file.flush();
  const bool closed = file.close();
  return complete && closed;
}

bool recoverImportPending() {
  if (!Storage.exists(kLegacyPending)) return true;
  // The ~1.5 KiB journal stays off the small X3 task stack. Import is an
  // explicit user action, not a reader hot path.
  auto pending = std::unique_ptr<ImportPending>(new (std::nothrow) ImportPending());
  if (!pending) return false;
  ImportPending& txn = *pending;
  if (!readValid(kLegacyPending, txn)) {
    LOG_ERR("RST", "Invalid legacy import marker; refusing new writes");
    return false;
  }
  if (!validateReceipts(true)) return false;
  const auto receipt = findReceipt(txn.source, txn.key());
  if (receipt == ReceiptLookup::Invalid) return false;
  if (receipt == ReceiptLookup::Already) return Storage.remove(kLegacyPending);

  if (txn.bookHash()) {
    const auto path = bookFile(txn.bookHash());
    Snapshot book{};
    if (!loadPath(path, book) && (Storage.exists(path.c_str()) || Storage.exists((path + ".bak").c_str())))
      return false;
    if (book.lastSequence < txn.sequence) {
      if (!legacy::mergeSnapshot(book, txn.delta)) return false;
      book.lastSequence = txn.sequence;
      if (!savePath(path, book)) return false;
    }
  }
  Snapshot global{};
  if (!loadPath(kGlobal, global) &&
      (Storage.exists(kGlobal) || Storage.exists((std::string(kGlobal) + ".bak").c_str())))
    return false;
  if (global.lastSequence < txn.sequence) {
    // Book imports affect the global total too; the CrossInk global-only
    // transaction has no bookHash. This sequence also reserves the ID.
    if ((txn.flags & 1u) && !legacy::mergeSnapshot(global, txn.delta)) return false;
    global.lastSequence = txn.sequence;
    if (!savePath(kGlobal, global)) return false;
  }
  if (!writeReceipt(txn)) return false;
  return Storage.remove(kLegacyPending);
}

}  // namespace

bool loadGlobal(Snapshot& out) {
  if (!recoverPending()) LOG_ERR("RST", "Failed to recover pending global stats");
  return loadPath(kGlobal, out);
}

bool loadBookHash(const uint64_t hash, Snapshot& out) {
  if (!recoverPending()) LOG_ERR("RST", "Failed to recover pending book stats");
  return loadPath(bookFile(hash), out);
}

bool loadBook(const char* bookPath, Snapshot& out) {
  if (!recoverPending()) LOG_ERR("RST", "Failed to recover pending book stats");
  if (!bookPath || !*bookPath) {
    out = Snapshot{};
    return false;
  }
  return loadPath(bookFile(pathHash(bookPath)), out);
}

bool recoverLegacyImport() { return recoverPending(); }

LegacyResult importLegacy(const LegacySource source, const char* stableKey, const char* bookPath, const char* title,
                          const char* author, const Snapshot& delta, const bool addToGlobal) {
  if ((source != LegacySource::CrossInk && source != LegacySource::VCodex) || !stableKey || !*stableKey ||
      !delta.valid() || !recoverPending())
    return LegacyResult::Error;
  if (bookPath && (*bookPath != '/' || strstr(bookPath, "/../") || strstr(bookPath, "/./") || strstr(bookPath, "\\") ||
                   strlen(bookPath) > 512))
    return LegacyResult::Error;
  if (source == LegacySource::VCodex && (!bookPath || !*bookPath)) return LegacyResult::Error;

  const uint64_t sourceKeyHash = pathHash(stableKey);
  const auto lookup = findReceipt(static_cast<uint32_t>(source), sourceKeyHash);
  if (lookup == ReceiptLookup::Already) return LegacyResult::AlreadyImported;
  if (lookup == ReceiptLookup::OtherSource) {
    LOG_ERR("RST", "Another legacy source was already imported; refusing possible double-counting");
    return LegacyResult::OtherSource;
  }
  if (lookup != ReceiptLookup::Missing) return LegacyResult::Error;

  const uint64_t bookHash = bookPath && *bookPath ? pathHash(bookPath) : 0;
  if (bookHash && !ensureBookIndexed(bookHash, bookPath, title, author)) return LegacyResult::Error;

  Snapshot global{};
  if (!loadPath(kGlobal, global) &&
      (Storage.exists(kGlobal) || Storage.exists((std::string(kGlobal) + ".bak").c_str())))
    return LegacyResult::Error;
  if (global.lastSequence == UINT32_MAX || Storage.exists(kLegacyPending)) return LegacyResult::Error;

  auto pending = std::unique_ptr<ImportPending>(new (std::nothrow) ImportPending());
  if (!pending) return LegacyResult::Error;
  ImportPending& txn = *pending;
  txn.source = static_cast<uint32_t>(source);
  txn.sequence = global.lastSequence + 1;
  txn.flags = addToGlobal ? 1u : 0u;
  txn.keyLo = static_cast<uint32_t>(sourceKeyHash);
  txn.keyHi = static_cast<uint32_t>(sourceKeyHash >> 32);
  txn.bookLo = static_cast<uint32_t>(bookHash);
  txn.bookHi = static_cast<uint32_t>(bookHash >> 32);
  txn.delta = delta;
  txn.seal();
  if (!ensureDir() || !writeTemp(kLegacyPending, txn)) return LegacyResult::Error;
  if (!Storage.rename((std::string(kLegacyPending) + ".tmp").c_str(), kLegacyPending)) return LegacyResult::Error;
  if (!recoverImportPending()) {
    LOG_ERR("RST", "Legacy import transaction retained for recovery");
    return LegacyResult::Error;
  }
  return LegacyResult::Imported;
}

bool rememberOpenedBook(const char* bookPath, const char* title, const char* author) {
  if (!bookPath || !*bookPath) return false;
  return ensureBookIndexed(pathHash(bookPath), bookPath, title, author);
}

// Implemented by the bounded book index, without loading the complete index
// into RAM. The separate global lifetime snapshot is deliberately preserved.
bool removeIndexedBook(uint64_t hash);

bool removeBookRecord(const uint64_t hash) {
  if (!hash || !recoverPending()) return false;
  if (!removeIndexedBook(hash)) return false;
  const std::string path = bookFile(hash);
  bool ok = true;
  for (const std::string& filename : {path, path + ".bak", path + ".tmp"}) {
    if (Storage.exists(filename.c_str()) && !Storage.remove(filename.c_str())) ok = false;
  }
  if (!ok) LOG_ERR("RST", "Could not remove every snapshot for book %016llx", static_cast<unsigned long long>(hash));
  media::remove(hash);
  return ok;
}

bool record(const char* bookPath, const uint32_t seconds, const uint32_t turns, const int64_t trustedStartEpoch,
            const char* title, const char* author) {
  if (!bookPath || !*bookPath || seconds < 10) return false;
  if (!recoverPending()) return false;
  if (!ensureBookIndexed(pathHash(bookPath), bookPath, title, author)) return false;

  Snapshot global;
  loadPath(kGlobal, global);
  if (global.lastSequence == UINT32_MAX) {
    LOG_ERR("RST", "Statistics sequence exhausted");
    return false;
  }

  Pending txn;
  txn.sequence = global.lastSequence + 1;
  txn.seconds = seconds;
  txn.turns = turns;
  txn.day = localDayOrdinal(trustedStartEpoch);
  txn.setBookHash(pathHash(bookPath));
  if (!writePending(txn)) {
    LOG_ERR("RST", "Failed to persist reading transaction");
    return false;
  }
  if (!recoverPending()) {
    LOG_ERR("RST", "Reading transaction saved for retry");
    return false;
  }
  return true;
}
}  // namespace readingstats
