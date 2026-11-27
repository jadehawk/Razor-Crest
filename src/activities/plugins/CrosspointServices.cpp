#if CROSSPOINT_DESSEN_APPS
#include "CrosspointServices.h"

#include <Bitmap.h>
#include <BookKey.h>
#include <Crypto.h>  // freeink::content::Crypto, RsaKeyPairDer
#include <DeviceSecret.h>
#include <Epub.h>
#include <FreeInkUIGfxRenderer.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>  // declares the `Frontlight` singleton
#include <HalGPIO.h>        // declares the `gpio` singleton (USB/charge detection)
#include <HalHaptics.h>
#include <HalMemory.h>
#include <HalPowerManager.h>  // declares the `powerManager` singleton
#include <HalStorage.h>
#include <I18n.h>  // I18N, Language, LANGUAGE_CODES
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <ReaderHardwareIdentity.h>
#include <Utf8.h>
#include <WiFi.h>
#include <WifiCredentialStore.h>
#include <WolfsslCrypto.h>  // freeink::content::WolfsslCrypto
#include <Xtc.h>
#include <esp_wifi.h>
#include <qrcode.h>
#include <sdk/dessen_abi.h>
#include <strings.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>

#include "DessenReadingStats.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"  // CoverIcon, the fallback-cover glyph
#include "network/HttpDownloader.h"
#include "sync/SyncEngine.h"
#include "util/BookProgress.h"
#include "util/DeviceHardwareProfile.h"
#include "util/FirmwareIdentity.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/ReadingHistoryMigration.h"
#include "util/ReadingHistorySource.h"
#include "util/ReadingStatsSleepSource.h"
#include "util/ScreenshotUtil.h"

extern GfxRenderer renderer;

using freeink::content::Crypto;
using freeink::content::RsaKeyPairDer;
using freeink::content::WolfsslCrypto;

namespace {
bool tlsHeapAvailable() {
  const uint32_t minBlock = HalMemory::getPsramHeap().totalBytes > 0 ? HttpDownloader::MIN_TLS_MAX_ALLOC_PSRAM
                                                                   : HttpDownloader::MIN_TLS_MAX_ALLOC;
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP && ESP.getMaxAllocHeap() >= minBlock;
}

// Preserve the guest buffer on failure and stop staging at its capacity.
class BoundedOutput {
 public:
  explicit BoundedOutput(size_t limit) : limit(limit) {}

  bool append(const void* src, size_t len) {
    if (!len) return true;
    if (len > limit - used) {
      error = DSN_ERR_TOO_BIG;
      return false;
    }
    const size_t needed = used + len;
    if (needed > capacity) {
      const size_t nextCapacity = std::max(needed, std::min(limit, std::max<size_t>(256, capacity * 2)));
      auto next = makeUniqueNoThrow<uint8_t[]>(nextCapacity);
      if (!next) {
        LOG_ERR("DSN", "OOM: %zu bytes for syscall output", nextCapacity);
        error = DSN_ERR_IO;
        return false;
      }
      if (used) memcpy(next.get(), data.get(), used);
      data = std::move(next);
      capacity = nextCapacity;
    }
    memcpy(data.get() + used, src, len);
    used = needed;
    return true;
  }

  int copyTo(void* out) const {
    if (used) memcpy(out, data.get(), used);
    return static_cast<int>(used);
  }

  size_t size() const { return used; }
  int error = 0;

 private:
  const size_t limit;
  size_t capacity = 0;
  size_t used = 0;
  std::unique_ptr<uint8_t[]> data;
};

bool shelfControl(unsigned char c) { return c < 0x20 || c == 0x7f; }

bool shelfPathValid(std::string_view path) {
  return !path.empty() && path.front() == '/' && path.size() <= DSN_BOOK_PATH_MAX &&
         std::none_of(path.begin(), path.end(), shelfControl);
}

int writeShelfRow(char* out, uint32_t cap, std::string_view path, std::string_view title, std::string_view author,
                  std::string_view series = {}) {
  if (!shelfPathValid(path)) {
    if (!cap) return DSN_ERR_TOO_BIG;
    *out = '\n';
    return 1;
  }
  constexpr size_t TEXT_MAX = 200;  // Size accepted by Dessen's text syscall.
  if (title.size() > TEXT_MAX) title = title.substr(0, utf8SafeTruncateBuffer(title.data(), TEXT_MAX));
  if (author.size() > TEXT_MAX) author = author.substr(0, utf8SafeTruncateBuffer(author.data(), TEXT_MAX));
  // A fifth optional column carries the library's formatted series and index.
  // Bound it to keep a complete row within the existing ABI's record limit.
  constexpr size_t SERIES_MAX = 100;
  if (series.size() > SERIES_MAX) series = series.substr(0, utf8SafeTruncateBuffer(series.data(), SERIES_MAX));
  const size_t baseBytes = path.size() + title.size() + author.size() + 2 + 4;  // two separators + "\t-1\n"
  if (!series.empty() && baseBytes + 1 + series.size() > DSN_SHELF_RECORD_MAX)
    series = series.substr(0, utf8SafeTruncateBuffer(series.data(), DSN_SHELF_RECORD_MAX - baseBytes - 1));
  const size_t bytes = baseBytes + (series.empty() ? 0 : 1 + series.size());
  if (bytes > cap) return DSN_ERR_TOO_BIG;

  char* pos = out;
  memcpy(pos, path.data(), path.size());
  pos += path.size();
  for (std::string_view field : {title, author}) {
    *pos++ = '\t';
    for (char c : field) *pos++ = shelfControl(c) ? ' ' : c;
  }
  memcpy(pos, "\t-1", 3);
  pos += 3;
  if (!series.empty()) {
    *pos++ = '\t';
    for (char c : series) *pos++ = shelfControl(c) ? ' ' : c;
  }
  *pos++ = '\n';
  return static_cast<int>(pos - out);
}

// PKCS#7: pad to a 16-byte boundary, or validate+strip. Returns padded length
// (encrypt) / plaintext length (decrypt), or -1 on bad padding.
int pkcs7Strip(const uint8_t* buf, uint32_t len) {
  if (len == 0 || (len & 15)) return -1;
  const uint8_t pad = buf[len - 1];
  if (pad < 1 || pad > 16 || pad > len) return -1;
  for (uint32_t i = 0; i < pad; i++)
    if (buf[len - 1 - i] != pad) return -1;
  return static_cast<int>(len - pad);
}
}  // namespace

// Cache the native Recent tab's overlap lookup for this launcher's lifetime.
struct CrosspointServices::ShelfOverlap {
  static constexpr size_t MAX_OPENED = RecentBooksStore::MAX_RECENT_BOOKS;
  uint64_t paths[MAX_OPENED] = {};
  uint16_t rows[MAX_OPENED] = {};
  uint16_t openedCount = 0, bookCount = 0, nextFirstSeen = 0, hiddenCount = 0;
  uint32_t indexSize = 0;

  bool resolve(library::LibraryIndexFile& index, const library::BookIdentity* identities, uint16_t count) {
    const auto& header = index.header();
    if (header.selfSize == indexSize && header.bookCount == bookCount && header.nextFirstSeen == nextFirstSeen &&
        count == openedCount && std::equal(identities, identities + count, paths, [](const auto& book, uint64_t hash) {
          return book.pathHash == hash;
        }))
      return true;

    uint16_t ascending[MAX_OPENED];
    if (!index.recentRowsFor(identities, count, ascending)) return false;

    // An opened file can have changed size since the current index was written.
    library::BookIdentity unresolved[MAX_OPENED];
    uint16_t pending[MAX_OPENED], n = 0;
    for (uint16_t i = 0; i < count; ++i) {
      if (ascending[i] == UINT16_MAX && identities[i].fileSize) {
        pending[n] = i;
        unresolved[n++] = {identities[i].pathHash, 0};
      }
    }
    if (n) {
      uint16_t found[MAX_OPENED];
      if (!index.recentRowsFor(unresolved, n, found)) return false;
      for (uint16_t i = 0; i < n; ++i) ascending[pending[i]] = found[i];
    }

    indexSize = header.selfSize;
    bookCount = header.bookCount;
    nextFirstSeen = header.nextFirstSeen;
    openedCount = count;
    hiddenCount = 0;
    for (uint16_t i = 0; i < count; ++i) {
      paths[i] = identities[i].pathHash;
      if (ascending[i] < bookCount) rows[hiddenCount++] = bookCount - 1 - ascending[i];
    }
    std::sort(rows, rows + hiddenCount);
    hiddenCount = std::unique(rows, rows + hiddenCount) - rows;
    return true;
  }
};

struct CrosspointServices::ReadingStatsRequest {
  readingHistory::Provider provider;
  char key[DSN_BOOK_PATH_MAX + 1]{};
  uint32_t anchorDay = 0;
  uint64_t prefixBytes = UINT64_MAX;
  unsigned long began = 0;
  unsigned long lastProgressLog = 0;
  bool started = false;
};

CrosspointServices::CrosspointServices(std::string appName, std::string appDir)
    : appName_(std::move(appName)), appDir_(std::move(appDir)), crypto_(makeUniqueNoThrow<WolfsslCrypto>()) {}
CrosspointServices::~CrosspointServices() {
  if (pending_ == Op::Fetch && !done_) {
    result_ = DSN_ERR_IO;
    cleanupDownload();
  }
}

bool CrosspointServices::loadPermissions() {
  if (!crypto_) {
    LOG_ERR("DSN", "Could not allocate cryptography services");
    return false;
  }
  std::string manifest;
  if (!Storage.readFileToString("DSN", appDir_ + "/manifest.json", DessenPermissions::MAX_MANIFEST, manifest) ||
      !permissions_.parse(manifest, appDir_)) {
    LOG_ERR("DSN", "Invalid app permissions");
    return false;
  }
  crypto_->sha256(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size(), permissionDigest_);
  return true;
}

// ---- core ----
uint32_t CrosspointServices::timeNow() {
  const time_t t = ::time(nullptr);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

// Local wall-clock for home themes (clock + date). The RTC keeps UTC; halClock
// applies the user's timezone. 8 bytes LE: u16 year, u8 month,day,hour,min,sec,wday.
int CrosspointServices::clock(uint8_t* out) {
  struct tm t;
  if (!halClock.localTime(t)) return DSN_ERR_UNAVAILABLE;  // clock unset
  const uint16_t year = static_cast<uint16_t>(t.tm_year + 1900);
  out[0] = static_cast<uint8_t>(year & 0xff);
  out[1] = static_cast<uint8_t>(year >> 8);
  out[2] = static_cast<uint8_t>(t.tm_mon + 1);
  out[3] = static_cast<uint8_t>(t.tm_mday);
  out[4] = static_cast<uint8_t>(t.tm_hour);
  out[5] = static_cast<uint8_t>(t.tm_min);
  out[6] = static_cast<uint8_t>(t.tm_sec);
  out[7] = static_cast<uint8_t>(t.tm_wday);
  return 0;
}

bool CrosspointServices::randomBytes(uint8_t* out, uint32_t n) {
  crypto_->randomBytes(out, n);
  return true;
}

// Per-app device id = hex(sha256(deviceSecret || appName)) Ã¢â‚¬” matches the web
// status route, and is unlinkable across apps.
int CrosspointServices::deviceId(char* out, uint32_t cap) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return DSN_ERR_UNAVAILABLE;
  uint8_t input[32 + 64];
  memcpy(input, secret, 32);
  size_t nameLen;
  if (appName_.size() > 64) {
    // Fold a long name to its full-name digest so names sharing the first 64
    // bytes still get distinct ids (plain truncation would collide them).
    crypto_->sha256(reinterpret_cast<const uint8_t*>(appName_.data()), appName_.size(), input + 32);
    nameLen = 32;
  } else {
    memcpy(input + 32, appName_.data(), appName_.size());
    nameLen = appName_.size();
  }
  uint8_t digest[32];
  crypto_->sha256(input, 32 + nameLen, digest);
  if (65 > cap) return DSN_ERR_TOO_BIG;
  for (size_t i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
  return 64;
}

int CrosspointServices::locale(char* out, uint32_t cap) {
  const char* code = LANGUAGE_CODES[static_cast<int>(I18N.getLanguage())];
  const size_t n = strlen(code);
  if (n + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, code, n + 1);
  return static_cast<int>(n);
}

// ---- storage ----
// Validate the spelling separately from the requested operation's authorization.
int CrosspointServices::resolve(const char* path, uint32_t maxLen) {
  if (!path || path[0] != '/' || strlen(path) > maxLen) return DSN_ERR_INVALID;
  // Reject ambiguous FAT spellings before any capability comparison.
  for (const char* first = path + 1; *first;) {
    const char* end = strchr(first, '/');
    if (!end) end = first + strlen(first);
    if (end == first || *first == ' ' || end[-1] == ' ' || end[-1] == '.') return DSN_ERR_INVALID;
    for (const char* c = first; c < end; ++c) {
      const unsigned char ch = static_cast<unsigned char>(*c);
      if (ch < 32 || ch == 127 || ch == '\\' || ch == ':' || ch == '~') return DSN_ERR_INVALID;
    }
    if (*end && !end[1]) return DSN_ERR_INVALID;
    first = *end ? end + 1 : end;
  }
  return 0;
}

int CrosspointServices::fsRead(const char* path, uint8_t* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::Read)) return DSN_ERR_UNAVAILABLE;
  // The virtual app-local file exposes the optional capture operation without a new guest ABI.
  if (appName_ == readingstatssleep::kId && path == appDir_ + readingstatssleep::kCaptureRequest) {
    if (cap == 0 || PluginLocations::findPluginDir(readingstatssleep::kId) != appDir_) return DSN_ERR_UNAVAILABLE;
    out[0] = 1;
    return 1;
  }
  HalFile f;
  if (!Storage.openFileForRead("DSN", path, f)) return DSN_ERR_IO;
  const size_t size = f.fileSize();
  if (size > cap) return DSN_ERR_TOO_BIG;
  const int got = f.read(out, size);
  return got == static_cast<int>(size) ? static_cast<int>(size) : DSN_ERR_IO;
}

int CrosspointServices::fsReadAt(const char* path, uint32_t offset, uint8_t* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::Read)) return DSN_ERR_UNAVAILABLE;
  HalFile f;
  if (!Storage.openFileForRead("DSN", path, f)) return DSN_ERR_IO;
  if (offset >= f.fileSize()) return 0;
  if (!f.seek(offset)) return DSN_ERR_IO;
  const int got = f.read(out, cap);
  return got < 0 ? DSN_ERR_IO : got;
}

int CrosspointServices::fsWrite(const char* path, const uint8_t* data, uint32_t n) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::Write)) return DSN_ERR_DENIED;
  if (appName_ == readingstatssleep::kId && path == appDir_ + readingstatssleep::kCaptureRequest) {
    if (n != 1 || data[0] > 1 || PluginLocations::findPluginDir(readingstatssleep::kId) != appDir_ ||
        !readingstatssleep::enabled(appDir_))
      return DSN_ERR_UNAVAILABLE;
    const std::string destination =
        appDir_ + (data[0] ? readingstatssleep::kServerImage : readingstatssleep::kDeviceImage);
    const std::string temp = destination + ".tmp";
    if (ScreenshotUtil::saveFramebufferAsBmp(temp.c_str(), renderer) &&
        Storage.replaceFile(temp.c_str(), destination.c_str()))
      return 0;
    Storage.remove(temp.c_str());
    return DSN_ERR_IO;
  }
  const bool credential = permissions_.isSharedCredential(path);
  if (credential && n > MAX_SHARED_CREDENTIAL_SIZE) return DSN_ERR_TOO_BIG;
  constexpr const char* temporary = "/.crosspoint/dsn-write.tmp";
  HalFile file;
  if (!Storage.openFileForWrite("DSN", temporary, file)) return DSN_ERR_IO;
  const size_t wrote = file.write(data, n);
  const bool closed = file.close();
  if (wrote != n || !closed || !Storage.replaceFile(temporary, path)) {
    Storage.remove(temporary);
    return DSN_ERR_IO;
  }
  pluginhttp::notifyFileChanged(path);
  return 0;
}

int CrosspointServices::fsList(const char* path, char* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::List)) return DSN_ERR_UNAVAILABLE;
  HalFile dir;
  if (!Storage.openFileForRead("DSN", path, dir) || !dir.isDirectory()) return DSN_ERR_IO;
  BoundedOutput entries(cap);
  for (HalFile e = dir.openNextFile(); e; e = dir.openNextFile()) {
    char name[257];
    size_t len = e.getName(name, sizeof name - 1);
    if (len == 0) continue;
    if (e.isDirectory()) name[len++] = '/';
    name[len++] = '\n';
    if (!entries.append(name, len)) return entries.error;
  }
  return entries.copyTo(out);
}

int CrosspointServices::fsDelete(const char* path) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::Delete)) return DSN_ERR_UNAVAILABLE;
  if (!Storage.remove(path)) return DSN_ERR_IO;
  pluginhttp::notifyFileChanged(path);
  return 0;
}

int CrosspointServices::fsAppDir(char* out, uint32_t cap) {
  if (appDir_.size() + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, appDir_.c_str(), appDir_.size() + 1);
  return static_cast<int>(appDir_.size());
}

int CrosspointServices::keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t expires) {
  if (int e = resolve(path)) return e;
  if (!permissions_.allows(path, DessenPermissions::Access::BookKey)) return DSN_ERR_UNAVAILABLE;
  if (n > bookkey::MAX_KEY_LEN) return DSN_ERR_INVALID;
  return bookkey::write(path, key, n, expires ? static_cast<int64_t>(expires) : 0) ? 0 : DSN_ERR_IO;
}

// ---- launcher / home ----
int CrosspointServices::recents(char* out, uint32_t cap) {
  if (!permissions_.allowsLibrary()) return DSN_ERR_DENIED;
  std::string acc;
  for (const RecentBook& b : RECENT_BOOKS.getBooks()) {
    char pct[8];
    snprintf(pct, sizeof pct, "%d", loadBookProgress(b.path));  // -1 when unknown
    acc += b.path;
    acc += '\t';
    // Normalize control chars to space (as writeShelfRow does) so a title or
    // author with an embedded tab/newline cannot forge columns or rows.
    for (char c : b.title) acc += shelfControl(c) ? ' ' : c;
    acc += '\t';
    for (char c : b.author) acc += shelfControl(c) ? ' ' : c;
    acc += '\t';
    acc += pct;
    acc += '\n';
  }
  if (acc.size() > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, acc.data(), acc.size());
  return static_cast<int>(acc.size());
}

int CrosspointServices::recentShelf(uint32_t first, uint32_t count, char* out, uint32_t cap) {
  if (!permissions_.allowsLibrary()) return DSN_ERR_DENIED;
  if (count > DSN_SHELF_PAGE_MAX) return DSN_ERR_INVALID;
  if (!count) return 0;

  const RecentBook* opened[ShelfOverlap::MAX_OPENED];
  library::BookIdentity identities[ShelfOverlap::MAX_OPENED];
  uint16_t openedCount = 0;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (openedCount == ShelfOverlap::MAX_OPENED) break;
    if (!shelfPathValid(book.path)) continue;
    auto file = Storage.open(book.path.c_str());
    if (!file || file.isDirectory()) continue;
    identities[openedCount] = {library::clixPathHash(book.path.data(), book.path.size()),
                               static_cast<uint32_t>(file.fileSize())};
    opened[openedCount++] = &book;
  }
  // Resolve series from the existing library index, not from EPUB parsing or a
  // separate app-side metadata cache. An absent index retains the old shelf row.
  library::LibraryIndexFile index;
  const char* indexPath = library::libraryIndexPath();
  const bool indexReady = index.open(indexPath);
  uint16_t openedRows[ShelfOverlap::MAX_OPENED] = {};
  const bool haveOpenedRows = indexReady && openedCount && index.recentRowsFor(identities, openedCount, openedRows);
  uint32_t used = 0;
  while (first < openedCount && count) {
    const RecentBook& book = *opened[first++];
    std::string series;
    if (haveOpenedRows && openedRows[first - 1] != UINT16_MAX) {
      const uint16_t ordinal = index.ordinalForRow(library::SortOrder::RecentAsc, openedRows[first - 1]);
      library::ClixRecord record{};
      if (ordinal != UINT16_MAX && index.readRecord(ordinal, record)) index.readSeries(record, series);
    }
    const int bytes = writeShelfRow(out + used, cap - used, book.path, book.title, book.author, series);
    if (bytes < 0) return used ? static_cast<int>(used) : bytes;
    used += bytes;
    --count;
  }
  if (!count) return static_cast<int>(used);

  uint32_t row = first - openedCount;
  if (!indexReady) return Storage.exists(indexPath) ? DSN_ERR_IO : static_cast<int>(used);
  if (row >= index.bookCount()) return static_cast<int>(used);
  if (!shelfOverlap_) shelfOverlap_ = makeUniqueNoThrow<ShelfOverlap>();
  if (!shelfOverlap_ || !shelfOverlap_->resolve(index, identities, openedCount)) return DSN_ERR_IO;

  size_t hidden = 0;
  while (hidden < shelfOverlap_->hiddenCount && shelfOverlap_->rows[hidden] <= row) {
    ++hidden;
    ++row;
  }
  uint16_t ordinals[DSN_SHELF_PAGE_MAX];
  uint32_t n = 0;
  for (; n < count && row < index.bookCount(); ++row) {
    if (hidden < shelfOverlap_->hiddenCount && shelfOverlap_->rows[hidden] == row) {
      ++hidden;
      continue;
    }
    ordinals[n++] = index.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(row));
  }
  if (index.ioFailed()) return DSN_ERR_IO;
  if (!n) return static_cast<int>(used);
  auto folders = makeUniqueNoThrow<library::LibraryIndexFile::FolderLookup[]>(n);
  if (!folders) return DSN_ERR_IO;
  library::ClixRecord rec{};
  for (uint32_t i = 0; i < n; ++i) {
    folders[i].id = index.readRecord(ordinals[i], rec) ? rec.folderId : UINT16_MAX;
    if (index.ioFailed()) return DSN_ERR_IO;
  }
  if (!index.readFolders(folders.get(), n)) return DSN_ERR_IO;

  std::string name, title, author, series;
  for (uint32_t i = 0; i < n; ++i) {
    std::string path;
    if (folders[i].path[0] && index.readRecord(ordinals[i], rec) && index.readName(rec, name)) {
      path = library::joinLibraryPath(folders[i].path, name);
      if (!index.readTitle(rec, title)) title = name;
      index.readAuthor(rec, author);
      if (!index.readSeries(rec, series)) series.clear();
    } else {
      series.clear();
    }
    if (index.ioFailed()) return DSN_ERR_IO;
    const int bytes = writeShelfRow(out + used, cap - used, path, title, author, series);
    if (bytes < 0) return used ? static_cast<int>(used) : bytes;
    used += bytes;
  }
  return static_cast<int>(used);
}

int CrosspointServices::readingStatsStart(const uint8_t* book, uint32_t length, uint32_t anchorDay) {
  if (!readingHistory::migrationReady()) return DSN_ERR_IO;
  if (!permissions_.allows("/.crosspoint/reading_sessions.jsonl", DessenPermissions::Access::Read))
    return DSN_ERR_DENIED;
  if (pending_ != Op::None || length > DSN_BOOK_PATH_MAX || (length && (!book || memchr(book, 0, length))) ||
      anchorDay > 47482)  // last supported local day, including UTC offset
    return DSN_ERR_INVALID;
  // A bounded prefix query is server-independent and has the same read grant.
  uint64_t prefixBytes = UINT64_MAX;
  if (length && book[0] == '@') {
    if (length != 18 && length != 50) return DSN_ERR_INVALID;
    if (book[17] != ':') return DSN_ERR_INVALID;
    prefixBytes = 0;
    for (unsigned i = 1; i < 17; ++i) {
      const auto c = book[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return DSN_ERR_INVALID;
      prefixBytes = (prefixBytes << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    if (prefixBytes == UINT64_MAX) return DSN_ERR_INVALID;
    book += 18;
    length -= 18;
  }
  const bool path = length && book[0] == '/';
  if (length && !path && length != 32) return DSN_ERR_INVALID;
  if (length && !path) {
    const std::string_view id(reinterpret_cast<const char*>(book), length);
    if (id.find_first_not_of("0123456789abcdefABCDEF") != id.npos) return DSN_ERR_INVALID;
  }
  readingStats_.reset();
  auto request = makeUniqueNoThrow<ReadingStatsRequest>();
  if (!request) {
    LOG_ERR("RHIST", "Cannot allocate %u-byte statistics request", static_cast<unsigned>(sizeof(ReadingStatsRequest)));
    return DSN_STATS_NO_MEMORY;
  }
  if (length) memcpy(request->key, book, length);
  if (path && (resolve(request->key, DSN_BOOK_PATH_MAX) || !permissions_.allowsBook(request->key)))
    return DSN_ERR_DENIED;
  request->anchorDay = anchorDay;
  request->prefixBytes = prefixBytes;
  request->began = millis();
  readingStats_ = std::move(request);
  pending_ = Op::ReadingStats;
  done_ = false;
  readingStatsActive_.store(true, std::memory_order_release);
  return dessen::kPending;
}

unsigned CrosspointServices::readingStatsProgress() const {
  return readingStats_ ? readingStats_->provider.progressPercent() : 0;
}

void CrosspointServices::cancelReadingStats() {
  if (!readingStatsBusy()) return;
  if (readingStats_) {
    LOG_INF("RHIST", "cancelled bytes=%llu/%llu sessions=%llu elapsed_ms=%lu",
            static_cast<unsigned long long>(readingStats_->provider.processedBytes()),
            static_cast<unsigned long long>(readingStats_->provider.expectedBytes()),
            static_cast<unsigned long long>(readingStats_->provider.processedSessions()),
            millis() - readingStats_->began);
    readingStats_->provider.cancel();
    readingStats_.reset();
  }
  result_ = DSN_ERR_IO;
  done_ = true;
  readingStatsActive_.store(false, std::memory_order_release);
}

bool CrosspointServices::serviceReadingStats() {
  using readingHistory::Status;
  if (!readingStats_) {
    result_ = DSN_ERR_INVALID;
    done_ = true;
    readingStatsActive_.store(false, std::memory_order_release);
    return true;
  }
  auto& request = *readingStats_;
  Status state;
  if (!request.started) {
    request.started = true;
    if (request.key[0] == '/') {
      const auto document = syncEngine::documentId(request.key);
      state = document.empty() ? Status::IoError : request.provider.begin(document, request.anchorDay);
    } else {
      state = request.provider.begin(request.key, request.anchorDay, false, request.prefixBytes);
    }
    request.lastProgressLog = millis();
    LOG_INF("RHIST", "start bytes=%llu cpu_mhz=%u free=%u max_block=%u",
            static_cast<unsigned long long>(request.provider.expectedBytes()),
            static_cast<unsigned>(getCpuFrequencyMhz()), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  } else {
    const auto sliceStart = millis();
    unsigned chunks = 0;
    do {
      state = request.provider.step();
    } while (state == Status::Scanning && ++chunks < 4 && millis() - sliceStart < 12);
  }
  if (state == Status::Scanning) {
    const auto now = millis();
    if (now - request.lastProgressLog >= 2000) {
      request.lastProgressLog = now;
      LOG_INF("RHIST",
              "progress bytes=%llu/%llu percent=%u sessions=%llu rejected=%llu elapsed_ms=%lu cpu_mhz=%u free=%u "
              "max_block=%u phase=%s parsed_bytes=%llu",
              static_cast<unsigned long long>(request.provider.processedBytes()),
              static_cast<unsigned long long>(request.provider.expectedBytes()), request.provider.progressPercent(),
              static_cast<unsigned long long>(request.provider.processedSessions()),
              static_cast<unsigned long long>(request.provider.rejectedLines()), now - request.began,
              static_cast<unsigned>(getCpuFrequencyMhz()), static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()), request.provider.validatingCache() ? "verify" : "parse",
              static_cast<unsigned long long>(request.provider.parsedBytes()));
    }
    return false;
  }
  result_ = dessenReadingStats::resultCode(state);
  if (state == Status::Ready) {
    const auto* summary = request.provider.summary();
    LOG_INF("RHIST",
            "ready sessions=%llu books=%u elapsed_ms=%lu free=%u bytes=%llu rejected=%llu cpu_mhz=%u max_block=%u "
            "limited=%u tail=%u parsed_bytes=%llu scratch_bytes=%u",
            static_cast<unsigned long long>(summary->overall.sessions), static_cast<unsigned>(summary->books),
            millis() - request.began, static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned long long>(summary->bytes), static_cast<unsigned long long>(summary->rejected),
            static_cast<unsigned>(getCpuFrequencyMhz()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(summary->booksLimited), static_cast<unsigned>(summary->incompleteTail),
            static_cast<unsigned long long>(request.provider.parsedBytes()),
            static_cast<unsigned>(request.provider.aggregationBuffersBytes()));
  } else {
    LOG_ERR("RHIST", "failed status=%u code=%d bytes=%llu/%llu elapsed_ms=%lu free=%u max_block=%u",
            static_cast<unsigned>(state), result_, static_cast<unsigned long long>(request.provider.processedBytes()),
            static_cast<unsigned long long>(request.provider.expectedBytes()), millis() - request.began,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    readingStats_.reset();
  }
  done_ = true;
  readingStatsActive_.store(false, std::memory_order_release);
  return true;
}

int CrosspointServices::readingStatsRead(uint32_t kind, uint32_t first, uint8_t* out, uint32_t cap) {
  if (!permissions_.allows("/.crosspoint/reading_sessions.jsonl", DessenPermissions::Access::Read))
    return DSN_ERR_DENIED;
  if (kind == DSN_STATS_CAPABILITY) return first ? DSN_ERR_INVALID : DSN_STATS_VERSION;
  if (kind == DSN_STATS_RELEASE) {
    if (first || pending_ != Op::None) return DSN_ERR_INVALID;
    readingStats_.reset();
    return 0;
  }
  if (kind == 7 && first == 2) return permissions_.allowsHistoryIdentity() ? 1 : DSN_ERR_DENIED;
  if (!readingStats_) return DSN_ERR_INVALID;
  if (kind == 7) {
    if (!permissions_.allowsHistoryIdentity() || pending_ != Op::None) return DSN_ERR_DENIED;
    const int result = readingHistory::readSourceIdentity(readingStats_->provider, first, out, cap);
    if (first == 1 && result == 0) readingStats_.reset();
    return result;
  }
  if (kind == DSN_STATS_BOOK_INFO) return dessenReadingStats::readBookInfo(readingStats_->provider, first, out, cap);
  return dessenReadingStats::read(readingStats_->provider, kind, first, out, cap);
}

int CrosspointServices::bookProgress(const char* path) {
  if (resolve(path, DSN_BOOK_PATH_MAX) || !permissions_.allowsBook(path)) return -1;
  return loadBookProgress(path);
}

// The firmware's fallback cover (drawCoverPlaceholder) as a 1-bit bitmap: a
// white top band with the book glyph, a black body, and a border. Returned by
// bookCover() whenever a book has no usable cover, so every book shows one.
static int packPlaceholderCover(uint8_t* out, uint32_t cap, int w, int h) {
  const uint32_t stride = (static_cast<uint32_t>(w) + 7) / 8;
  const uint32_t need = 4 + stride * static_cast<uint32_t>(h);
  if (need > cap) return DSN_ERR_TOO_BIG;
  out[0] = static_cast<uint8_t>(w);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h);
  out[3] = static_cast<uint8_t>(h >> 8);
  uint8_t* px = out + 4;
  memset(px, 0, stride * static_cast<uint32_t>(h));
  auto set = [&](int x, int y) {
    if (x >= 0 && x < w && y >= 0 && y < h) px[static_cast<uint32_t>(y) * stride + (x >> 3)] |= 0x80 >> (x & 7);
  };
  const int top = h / 3;
  for (int y = top; y < h; ++y)
    for (int x = 0; x < w; ++x) set(x, y);  // black body (bottom two thirds)
  for (int x = 0; x < w; ++x) set(x, 0), set(x, h - 1);
  for (int y = 0; y < h; ++y) set(0, y), set(w - 1, y);  // border
  if (w >= 36 && top >= 36) {                            // book glyph, centred in the top band
    const int ix = (w - 32) / 2, iy = (top - 32) / 2;
    for (int row = 0; row < 32; ++row)
      for (int col = 0; col < 32; ++col)
        if (((CoverIcon[row * 4 + (col >> 3)] >> (7 - (col & 7))) & 1) == 0) set(ix + col, iy + row);  // bit0 = ink
  }
  return static_cast<int>(need);
}

#ifndef BOARD_HAS_PSRAM
// Reuse a thumbnail already generated by Home. The requested Dessen height
// may differ from the cached Home height, so choose the closest cached size.
static std::string cachedBookThumbnail(const std::string& path, uint32_t height) {
  std::string pattern;
  if (FsHelpers::hasReflowableBookExtension(path)) {
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("DSN", "OOM: %zu bytes for cover cache path", sizeof(Epub));
      return {};
    }
    pattern = epub->getThumbBmpPath();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc xtc(path, "/.crosspoint");
    pattern = xtc.getThumbBmpPath();
  } else {
    return {};
  }
  const size_t slash = pattern.find_last_of('/');
  const size_t marker = pattern.find("[HEIGHT]", slash + 1);
  if (slash == std::string::npos || marker == std::string::npos) return {};
  const std::string directory = pattern.substr(0, slash);
  const std::string_view prefix(pattern.data() + slash + 1, marker - slash - 1);
  const std::string_view suffix(pattern.data() + marker + 8, pattern.size() - marker - 8);
  HalFile dir;
  if (!Storage.openFileForRead("DSN", directory, dir) || !dir.isDirectory()) return {};
  std::string best;
  uint32_t bestDistance = UINT32_MAX;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory() || entry.size() < sizeof(BmpHeader)) continue;
    char name[64] = {};
    const size_t len = entry.getName(name, sizeof(name) - 1);
    if (len >= sizeof(name)) continue;
    const std::string_view candidate(name, len);
    // Stock CrossPoint caches EPUB thumbs as thumb_<height>.bmp.
    // Razor-Crest also caches fill/crop thumbs under its own prefix.
    constexpr std::string_view upstreamPrefix = "thumb_";
    const size_t prefixLength = candidate.substr(0, prefix.size()) == prefix                   ? prefix.size()
                                : candidate.substr(0, upstreamPrefix.size()) == upstreamPrefix ? upstreamPrefix.size()
                                                                                               : 0;
    if (!prefixLength || len <= prefixLength + suffix.size() || candidate.substr(len - suffix.size()) != suffix)
      continue;
    const auto digits = candidate.substr(prefixLength, len - prefixLength - suffix.size());
    if (digits.size() > 4) continue;
    uint32_t cachedHeight = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9') {
        cachedHeight = 0;
        break;
      }
      cachedHeight = cachedHeight * 10 + (digit - '0');
    }
    if (cachedHeight == 0 || cachedHeight > 1024) continue;
    const uint32_t distance = cachedHeight > height ? cachedHeight - height : height - cachedHeight;
    if (distance > bestDistance || (distance == bestDistance && prefixLength != prefix.size())) continue;
    bestDistance = distance;
    best.assign(name, len);
    if (distance == 0 && prefixLength == prefix.size()) break;
  }
  return best.empty() ? std::string() : directory + "/" + best;
}

// Stream only one cached BMP row plus its luminance values into the guest's
// existing output buffer. The 1024-pixel source limit caps scratch at 5 KiB.
static int packCachedBookCover(const std::string& thumb, uint32_t height, uint8_t* out, uint32_t cap) {
  HalFile file;
  if (thumb.empty() || !Storage.openFileForRead("DSN", thumb, file)) return DSN_ERR_UNAVAILABLE;
  auto bmp = makeUniqueNoThrow<Bitmap>(file);
  if (!bmp) {
    LOG_ERR("DSN", "OOM: %zu bytes for cached cover reader", sizeof(Bitmap));
    return DSN_ERR_IO;
  }
  if (bmp->parseHeaders() != BmpReaderError::Ok) return DSN_ERR_IO;
  const int sourceWidth = bmp->getWidth(), sourceHeight = bmp->getHeight();
  const int rowBytes = bmp->getRowBytes();
  if (sourceWidth <= 0 || sourceWidth > 1024 || sourceHeight <= 0 || sourceHeight > 1024 || rowBytes <= 0 ||
      rowBytes > 4096)
    return DSN_ERR_TOO_BIG;
  const size_t scratchBytes = static_cast<size_t>(sourceWidth) + rowBytes;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(scratchBytes);
  if (!scratch) {
    LOG_ERR("DSN", "OOM: %zu bytes for cached cover row", scratchBytes);
    return DSN_ERR_IO;
  }
  uint8_t* gray = scratch.get();
  uint8_t* row = gray + sourceWidth;
  const int maxWidth = static_cast<int>(height) * 2 / 3;
  int h = std::min(static_cast<int>(height), maxWidth * sourceHeight / sourceWidth);
  int w = std::max(1, h * sourceWidth / sourceHeight);
  while (h > 0 && 4u + static_cast<uint32_t>((w + 7) / 8) * h > cap) {
    --h;
    w = std::max(1, h * sourceWidth / sourceHeight);
  }
  if (h <= 0) return DSN_ERR_TOO_BIG;
  const uint32_t stride = (static_cast<uint32_t>(w) + 7) / 8;
  const uint32_t need = 4 + stride * h;
  out[0] = static_cast<uint8_t>(w);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h);
  out[3] = static_cast<uint8_t>(h >> 8);
  memset(out + 4, 0, need - 4);
  static constexpr uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  int previousRow = -1;
  for (int y = 0; y < h; ++y) {
    const int sourceY = y * sourceHeight / h;
    if (sourceY != previousRow && bmp->readRowGray(sourceY, gray, row) != BmpReaderError::Ok) return DSN_ERR_IO;
    previousRow = sourceY;
    uint8_t* line = out + 4 + static_cast<uint32_t>(y) * stride;
    for (int x = 0; x < w; ++x)
      if (gray[x * sourceWidth / w] < bayer[y & 3][x & 3] * 16 + 8) line[x >> 3] |= 0x80 >> (x & 7);
  }
  return static_cast<int>(need);
}
#endif

// A book's cover as a dithered 1-bit bitmap scaled to `height`: writes
// {u16 w, u16 h} then the packed MSB-first rows (the dsn_bitmap BW format).
// Generates and caches the thumbnail on first use (the slow part Ã¢â‚¬” decoding the
// cover image), so apps request one per redraw. A book with no usable cover
// gets the firmware's placeholder on PSRAM devices. C3 only reads existing cached thumbnails.
int CrosspointServices::bookCover(const char* path, uint32_t height, uint8_t* out, uint32_t cap) {
#ifndef BOARD_HAS_PSRAM
  if (resolve(path, DSN_BOOK_PATH_MAX) || height < 16 || height > 512) return DSN_ERR_INVALID;
  if (!permissions_.allowsBook(path)) return DSN_ERR_UNAVAILABLE;
  return packCachedBookCover(cachedBookThumbnail(path, height), height, out, cap);
#else
  if (resolve(path, DSN_BOOK_PATH_MAX)) return DSN_ERR_INVALID;
  if (!permissions_.allowsBook(path)) return DSN_ERR_UNAVAILABLE;
  const std::string full(path);
  if (height < 16 || height > 512) return DSN_ERR_INVALID;
  const int ph = static_cast<int>(height), pw = ph * 2 / 3;  // placeholder size (book aspect)
  if (!FsHelpers::hasReflowableBookExtension(full)) return packPlaceholderCover(out, cap, pw, ph);
  Epub epub(full, "/.crosspoint");
  const std::string thumb = epub.getThumbBmpPath(static_cast<int>(height));
  if (!Storage.exists(thumb.c_str())) {  // decode + cache on first use (slow)
    const bool cacheLoaded = epub.load(false, true);
    if (!(cacheLoaded ? epub.generateThumbBmp(static_cast<int>(height))
                      : epub.generateThumbBmpFromSource(static_cast<int>(height))))
      return packPlaceholderCover(out, cap, pw, ph);  // no cover image in this book
  }
  HalFile file;
  if (!Storage.openFileForRead("DSN", thumb, file)) return packPlaceholderCover(out, cap, pw, ph);
  Bitmap bmp(file);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return packPlaceholderCover(out, cap, pw, ph);
  const int w = bmp.getWidth(), h = bmp.getHeight();
  if (w <= 0 || h <= 0) return packPlaceholderCover(out, cap, pw, ph);
  const uint32_t stride = (static_cast<uint32_t>(w) + 7) / 8;
  const uint32_t need = 4 + stride * static_cast<uint32_t>(h);
  if (need > cap) return DSN_ERR_TOO_BIG;
  out[0] = static_cast<uint8_t>(w);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h);
  out[3] = static_cast<uint8_t>(h >> 8);
  uint8_t* px = out + 4;
  memset(px, 0, stride * static_cast<uint32_t>(h));
  static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  std::vector<uint8_t> gray(w), rowbuf(static_cast<size_t>(w) * 4 + 16);
  for (int y = 0; y < h; ++y) {
    // A mid-image row failure would otherwise return a half-zeroed bitmap as
    // complete; fall back to the placeholder like every other failure here.
    if (bmp.readRowGray(y, gray.data(), rowbuf.data()) != BmpReaderError::Ok)
      return packPlaceholderCover(out, cap, pw, ph);
    uint8_t* line = px + static_cast<uint32_t>(y) * stride;
    for (int x = 0; x < w; ++x)
      if (gray[x] < BAYER[y & 3][x & 3] * 16 + 8) line[x >> 3] |= 0x80 >> (x & 7);  // dark -> black bit
  }
  return static_cast<int>(need);
#endif
}

// ---- crypto ----
int CrosspointServices::hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  if (algo == DSN_HASH_SHA1) {
    if (cap < 20) return DSN_ERR_TOO_BIG;
    crypto_->sha1(data, n, out);
    return 20;
  }
  if (algo == DSN_HASH_SHA256) {
    if (cap < 32) return DSN_ERR_TOO_BIG;
    crypto_->sha256(data, n, out);
    return 32;
  }
  return DSN_ERR_INVALID;
}

int CrosspointServices::aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out,
                               uint32_t cap) {
  const uint8_t* key = keyiv;
  const uint8_t* iv = keyiv + 16;
  const bool enc = flags & DSN_AES_ENCRYPT;
  const bool pad = !(flags & DSN_AES_NOPAD);
  if (enc) {
    // Build the (optionally padded) plaintext, then raw-encrypt in place.
    uint32_t len = n;
    if (pad) len = (n & ~15u) + 16;  // PKCS#7 always adds a full block when aligned
    if (len > cap) return DSN_ERR_TOO_BIG;
    if (out != data) memcpy(out, data, n);
    if (pad) memset(out + n, static_cast<uint8_t>(len - n), len - n);
    return crypto_->aes128CbcEncrypt(key, iv, out, len, out) ? static_cast<int>(len) : DSN_ERR_INVALID;
  }
  if (n > cap) return DSN_ERR_TOO_BIG;
  if (!crypto_->aes128CbcDecrypt(key, iv, data, n, out)) return DSN_ERR_INVALID;
  if (!pad) return static_cast<int>(n);
  const int stripped = pkcs7Strip(out, n);
  return stripped < 0 ? DSN_ERR_INVALID : stripped;
}

int CrosspointServices::rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out,
                                   uint32_t cap) {
  size_t outLen = 0;
  if (!crypto_->rsaPublicEncrypt(cert, cn, data, n, out, cap, &outLen)) return DSN_ERR_INVALID;
  return static_cast<int>(outLen);
}

int CrosspointServices::rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n,
                                   uint8_t* out, uint32_t cap) {
  uint8_t block[256];
  const int32_t blockLen = crypto_->rsaPrivateRaw(priv, pn, data, n, block, sizeof block);
  if (blockLen <= 0) return DSN_ERR_INVALID;
  if (flags & DSN_RSA_RAW) {
    if (static_cast<uint32_t>(blockLen) > cap) return DSN_ERR_TOO_BIG;
    memcpy(out, block, blockLen);
    return blockLen;
  }
  // Strip PKCS#1 v1.5 type-2: 00 02 <nonzero padding>=8 00 <message>.
  if (blockLen < 11 || block[0] != 0x00 || block[1] != 0x02) return DSN_ERR_INVALID;
  int sep = -1;
  for (int i = 2; i < blockLen; i++)
    if (block[i] == 0x00) {
      sep = i;
      break;
    }
  if (sep < 10) return DSN_ERR_INVALID;  // need >= 8 padding bytes
  const uint32_t msgLen = blockLen - sep - 1;
  if (msgLen > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, block + sep + 1, msgLen);
  return static_cast<int>(msgLen);
}

int CrosspointServices::rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn,
                                uint8_t* out, uint32_t cap) {
  // The firmware primitive signs a 20-byte (SHA-1) digest, PKCS#1 v1.5, no
  // DigestInfo, into a 128-byte (1024-bit) signature Ã¢â‚¬” exactly the one case the
  // first consumer needs. Other hash/DigestInfo combinations are not provided.
  if ((algo & 0x7f) != DSN_HASH_SHA1 || !(algo & DSN_SIGN_NODIGESTINFO)) return DSN_ERR_INVALID;
  if (dn != 20 || cap < 128) return DSN_ERR_TOO_BIG;
  return crypto_->rsaPrivateSignRaw(priv, pn, digest, out) ? 128 : DSN_ERR_INVALID;
}

int CrosspointServices::pkcs12Parse(const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) {
  std::vector<uint8_t> key, cert;
  if (!crypto_->pkcs12Extract(p12, n, pass, &key, &cert)) return DSN_ERR_INVALID;
  if (key.size() >= 0x10000) return DSN_ERR_INVALID;
  const size_t total = 2 + key.size() + cert.size();
  if (total > cap) return DSN_ERR_TOO_BIG;
  out[0] = (key.size() >> 8) & 0xff;
  out[1] = key.size() & 0xff;
  memcpy(out + 2, key.data(), key.size());
  memcpy(out + 2 + key.size(), cert.data(), cert.size());
  return static_cast<int>(total);
}

// ---- board ----
int CrosspointServices::backlightSet(int brightness, int warm) {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  if (brightness >= 0) Frontlight.setBrightness(static_cast<uint8_t>(brightness > 100 ? 100 : brightness));
  if (warm >= 0 && Frontlight.hasColorTemperature())
    Frontlight.setWarmth(static_cast<uint8_t>(warm > 100 ? 100 : warm));
  return 0;
}

int CrosspointServices::backlightGet() {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  return Frontlight.brightness() | (Frontlight.hasColorTemperature() ? Frontlight.warmth() : 0) << 8;
}

int CrosspointServices::battery() {
  const uint16_t pct = powerManager.getBatteryPercentage();
  if (pct > 100) return DSN_ERR_UNAVAILABLE;
  // Second byte: power state, from the same external-power signal the status bar
  // uses (board-aware: USB-detect pin, charger IC, or charging state). Some
  // boards read "not connected" at charge termination, so a full battery there
  // reports BATTERY Ã¢â‚¬” matching the status bar, which drops its bolt the same way.
  uint8_t state;
  if (!gpio.isUsbConnected())
    state = DSN_PWR_BATTERY;
  else if (pct >= 100)
    state = DSN_PWR_FULL;
  else
    state = DSN_PWR_CHARGING;
  return (pct & 0xff) | state << 8;
}

int CrosspointServices::haptic(int ms, int intensity) {
  (void)ms;  // HalHaptics plays a fixed short pattern; duration is advisory
  HalHaptics::longPress(true, static_cast<uint8_t>(intensity > 2 ? 2 : intensity < 0 ? 0 : intensity));
  return 0;
}

int CrosspointServices::accel(uint8_t*) {
  // The firmware tilt HAL exposes gyro rate, not acceleration in g. Until a
  // board surfaces raw accel, report "no such part".
  return DSN_ERR_UNAVAILABLE;
}

// Build a scannable QR matrix using the firmware's existing native QRCode
// library. Return bounded row-padded 1bpp pixels to the sandbox; this avoids
// expensive Reed-Solomon calculations inside the interpreted RV32 guest.
int CrosspointServices::qrBitmap(const char* link, uint32_t length, uint8_t* out, uint32_t cap) {
  constexpr uint32_t qrVersion = 4;
  constexpr uint32_t qrSize = 33;
  constexpr uint32_t stride = (qrSize + 7) / 8;
  constexpr uint32_t required = 4 + stride * qrSize;
  if (!link || length == 0 || length > 72) return DSN_ERR_INVALID;
  if (!out || cap < required) return DSN_ERR_TOO_BIG;
  if (std::any_of(link, link + length, [](unsigned char c) { return c < 32 || c > 126; })) {
    return DSN_ERR_INVALID;
  }
  // X3 render task stack has reached under 500 free bytes in serial logs.
  // Native QRCode's scratch must not consume 512 bytes of that stack.
  const uint32_t scratchSize = qrcode_getBufferSize(qrVersion);
  if (!scratchSize || scratchSize > 512) return DSN_ERR_IO;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(scratchSize);
  if (!scratch) return DSN_ERR_IO;
  const std::string url(link, length);
  QRCode qr{};
  if (qrcode_initText(&qr, scratch.get(), qrVersion, ECC_LOW, url.c_str()) != 0 || qr.size != qrSize) {
    return DSN_ERR_INVALID;
  }
  out[0] = qrSize;
  out[1] = 0;
  out[2] = qrSize;
  out[3] = 0;
  memset(out + 4, 0, required - 4);
  for (uint32_t row = 0; row < qrSize; row++) {
    for (uint32_t col = 0; col < qrSize; col++) {
      if (qrcode_getModule(&qr, col, row)) {
        out[4 + row * stride + col / 8] |= static_cast<uint8_t>(0x80u >> (col & 7));
      }
    }
  }
  return required;
}

// ---- async: queue on the render task, execute on the loop task ----
int CrosspointServices::wifiConnectStart() {
  if (!permissions_.allowsNetwork()) return DSN_ERR_UNAVAILABLE;
  readingStats_.reset();
#ifndef BOARD_HAS_PSRAM
  // Connection is deferred until the guest is unloaded by the request.
  if (!permissions_.allowsHandoff()) return DSN_ERR_UNAVAILABLE;
  return 0;
#endif
  networkCancelled_.store(false, std::memory_order_relaxed);
  pending_ = Op::WifiStart;
  done_ = false;
  return dessen::kPending;
}

int CrosspointServices::netStart(const char* head, const uint8_t* body, uint32_t bn, uint8_t* resp, uint32_t cap) {
  const std::string_view request(head ? head : "");
  const size_t space = request.find(' ');
  const size_t end = request.find_first_of("\r\n");
  if (space == request.npos ||
      !permissions_.allowsUrl(request.substr(space + 1, end == request.npos ? end : end - space - 1)))
    return DSN_ERR_UNAVAILABLE;
  if (bn > 8192 || cap > 32768) return DSN_ERR_TOO_BIG;
  readingStats_.reset();
  networkCancelled_.store(false, std::memory_order_relaxed);
  pending_ = Op::Net;
  done_ = false;
  head_ = head;
  body_ = body;
  bodySize_ = bn;
  out_ = resp;
  outCap_ = cap;
  return dessen::kPending;
}

int CrosspointServices::fetchStart(const char* url, const char* dest) {
  if (!url || !dest || !permissions_.allowsUrl(url) || !permissions_.allows(dest, DessenPermissions::Access::Write) ||
      !permissions_.allows(dest, DessenPermissions::Access::Delete) ||
      !permissions_.allows(std::string(dest) + ".part", DessenPermissions::Access::Write) ||
      !permissions_.allows(std::string(dest) + ".part", DessenPermissions::Access::Delete))
    return DSN_ERR_UNAVAILABLE;
  readingStats_.reset();
  const std::string temporary = std::string(dest) + ".part";
  HalFile marker;
  const bool saved = Storage.openFileForWrite("DSN", "/.crosspoint/dsn-download.tmp", marker) &&
                     marker.write(temporary.data(), temporary.size()) == temporary.size();
  const bool closed = marker && marker.close();
  if (!saved || !closed) {
    Storage.remove("/.crosspoint/dsn-download.tmp");
    return DSN_ERR_IO;
  }
  networkCancelled_.store(false, std::memory_order_relaxed);
  pending_ = Op::Fetch;
  done_ = false;
  url_ = url;
  dest_ = dest;
  return dessen::kPending;
}

int CrosspointServices::keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) {
  pending_ = Op::Keygen;
  done_ = false;
  bits_ = bits;
  out_ = pub;
  outCap_ = pubCap;
  priv_ = priv;
  privCap_ = privCap;
  return dessen::kPending;
}

int CrosspointServices::textInputStart(const char* prompt, char* out, uint32_t cap) {
  pending_ = Op::TextInput;
  done_ = false;
  prompt_ = prompt;
  out_ = reinterpret_cast<uint8_t*>(out);
  outCap_ = cap;
  return dessen::kPending;
}

void CrosspointServices::cleanupDownload() {
  if (pending_ != Op::Fetch) return;
  if (result_ < 0)
    Storage.remove((dest_ + ".part").c_str());
  else
    pluginhttp::notifyFileChanged(dest_);
  Storage.remove("/.crosspoint/dsn-download.tmp");
}

void CrosspointServices::failNetwork() {
  result_ = DSN_ERR_IO;
  done_ = true;
  cleanupDownload();
}

#ifndef BOARD_HAS_PSRAM
bool CrosspointServices::detachNetworkBuffers(const uint8_t* base, uint32_t size) {
  if (!permissions_.allowsHandoff() || !needsNetworkHeap() || networkDetached_ || !base) return false;
  if (pending_ == Op::Net) {
    // All guest ranges were validated by the dispatch. Check again before rebasing.
    auto valid = [base, size](const uint8_t* ptr, uint32_t len) {
      const uintptr_t start = reinterpret_cast<uintptr_t>(base);
      const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
      return addr >= start && addr - start <= size && len <= size - (addr - start);
    };
    if (!valid(out_, outCap_) || (bodySize_ && !valid(body_, bodySize_))) return false;
    if (bodySize_) {
      detachedBody_ = makeUniqueNoThrow<uint8_t[]>(bodySize_);
      if (!detachedBody_) return false;
      memcpy(detachedBody_.get(), body_, bodySize_);
    }
    outOffset_ = static_cast<uint32_t>(out_ - base);
    body_ = detachedBody_.get();
    out_ = nullptr;
  }
  networkDetached_ = true;
  return true;
}

void CrosspointServices::restoreNetworkBuffers(uint8_t* base) {
  if (!networkDetached_) return;
  if (pending_ == Op::Net) {
    out_ = base + outOffset_;
    if (detachedResponseSize_) memcpy(out_, detachedResponse_.get(), detachedResponseSize_);
    body_ = nullptr;
  }
  detachedBody_.reset();
  detachedResponse_.reset();
  detachedResponseSize_ = 0;
  networkDetached_ = false;
}

// Pointer-free, bounded request record. It is encrypted by the activity before storage.
bool CrosspointServices::exportNetworkRequest(std::unique_ptr<uint8_t[]>& data, uint32_t& size) const {
  if (!networkDetached_ || !permissions_.allowsHandoff()) return false;
  const bool net = pending_ == Op::Net;
  if (!net && pending_ != Op::Fetch) return false;
  const auto& first = net ? head_ : url_;
  const uint32_t a = first.size(), b = net ? bodySize_ : dest_.size();
  if (!a || a > 1024 || b > (net ? 8192u : 255u)) return false;
  const uint32_t fields[] = {net ? 1u : 2u, a, b, net ? outOffset_ : 0u, net ? outCap_ : 0u};
  size = sizeof(fields) + a + b;
  data = makeUniqueNoThrow<uint8_t[]>(size);
  if (!data) return false;
  memcpy(data.get(), fields, sizeof(fields));
  memcpy(data.get() + sizeof(fields), first.data(), a);
  if (b) memcpy(data.get() + sizeof(fields) + a, net ? body_ : reinterpret_cast<const uint8_t*>(dest_.data()), b);
  return true;
}

bool CrosspointServices::importNetworkRequest(const uint8_t* data, uint32_t size, uint32_t guestSize) {
  uint32_t fields[5];
  if (!data || size < sizeof(fields) || size > MAX_NETWORK_REQUEST || !permissions_.allowsHandoff()) return false;
  memcpy(fields, data, sizeof(fields));
  const uint32_t kind = fields[0], a = fields[1], b = fields[2], offset = fields[3], cap = fields[4];
  if ((kind != 1 && kind != 2) || !a || a > 1024 || b > (kind == 1 ? 8192u : 255u) || size != sizeof(fields) + a + b ||
      memchr(data + sizeof(fields), 0, a))
    return false;
  const std::string first(reinterpret_cast<const char*>(data + sizeof(fields)), a);
  const uint8_t* second = data + sizeof(fields) + a;
  if (kind == 1) {
    if (cap < 4 || cap > 32768 || offset > guestSize || cap > guestSize - offset) return false;
    if (b) {
      detachedBody_ = makeUniqueNoThrow<uint8_t[]>(b);
      if (!detachedBody_) return false;
      memcpy(detachedBody_.get(), second, b);
    }
    if (netStart(first.c_str(), detachedBody_.get(), b, nullptr, cap) != dessen::kPending) return false;
    outOffset_ = offset;
  } else {
    if (!b || offset || cap || memchr(second, 0, b)) return false;
    const std::string dest(reinterpret_cast<const char*>(second), b);
    if (fetchStart(first.c_str(), dest.c_str()) != dessen::kPending) return false;
  }
  networkDetached_ = true;
  return true;
}

bool CrosspointServices::startSuspendedWifi() {
  WIFI_STORE.loadFromFile();
  const auto last = WIFI_STORE.getLastConnectedSsid();
  auto cred = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    result_ = DSN_ERR_UNAVAILABLE;
    return false;
  }
  WiFi.persistent(false);
  radioOwned_ = true;
  LOG_INF("DSN", "Suspended Wi-Fi start: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  wifiStart_ = millis();
  LOG_INF("DSN", "Suspended Wi-Fi: entering station mode");
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("DSN", "Suspended Wi-Fi: station mode failed");
    result_ = DSN_ERR_IO;
    return false;
  }
  LOG_INF("DSN", "Suspended Wi-Fi: station mode ready; beginning connection");
  const auto status = WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  LOG_INF("DSN", "Suspended Wi-Fi: begin returned %d", static_cast<int>(status));
  reconnecting_ = true;
  return true;
}

void CrosspointServices::releaseNetworkRadio() {
  if (!radioOwned_) return;
  WiFi.disconnect();
  if (!WiFi.mode(WIFI_OFF)) {
    LOG_ERR("DSN", "Wi-Fi shutdown failed");
    return;
  }
  radioOwned_ = false;
  reconnecting_ = false;
  LOG_INF("DSN", "Wi-Fi released: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // Give idle/event tasks a bounded opportunity to finish deferred cleanup.
  delay(50);
  LOG_INF("DSN", "Wi-Fi cleanup +50ms: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  delay(150);
  LOG_INF("DSN", "Wi-Fi cleanup +200ms: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void CrosspointServices::completeSuspendedNetwork() {
  reconnecting_ = false;
  detachedBody_.reset();
  body_ = nullptr;
  out_ = nullptr;
  releaseNetworkRadio();
  done_ = true;
}

void CrosspointServices::cancelSuspendedNetwork() {
  reconnecting_ = false;
  releaseNetworkRadio();
}
#endif

int CrosspointServices::poll() {
#ifndef BOARD_HAS_PSRAM
  if (networkDetached_) return dessen::kPending;
#endif
  if (!done_) return dessen::kPending;
  pending_ = Op::None;
  return result_;
}

bool CrosspointServices::serviceAsyncWork() {
  if (done_) return true;
  if (pending_ == Op::ReadingStats) return serviceReadingStats();
  if (networkCancelled_.load(std::memory_order_relaxed)) {
    failNetwork();
#ifndef BOARD_HAS_PSRAM
    if (networkDetached_) completeSuspendedNetwork();
#endif
    return true;
  }
#ifndef BOARD_HAS_PSRAM
  // The guest has already released RAM. Reconnect only for an explicitly
  // authorized logical Wi-Fi session; preserve offline behavior otherwise.
  if (networkDetached_ && (pending_ == Op::WifiStart || pending_ == Op::Net || pending_ == Op::Fetch)) {
    if (WiFi.status() != WL_CONNECTED) {
      if (!reconnecting_) {
        if (startSuspendedWifi()) return false;
        completeSuspendedNetwork();
        return true;
      }
      if (millis() - wifiStart_ <= 10000) return false;
      LOG_ERR("DSN", "Suspended Wi-Fi: connection timeout, status=%d", static_cast<int>(WiFi.status()));
      result_ = DSN_ERR_IO;
      completeSuspendedNetwork();
      return true;
    }
    LOG_INF("DSN", "Suspended Wi-Fi connected: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    reconnecting_ = false;
    if (pending_ == Op::WifiStart) {
      result_ = 0;
      completeSuspendedNetwork();
      return true;
    }
  }
#endif
  switch (pending_) {
    case Op::Net: {
      const std::string_view request = head_;
      const size_t newline = request.find('\n');
      const auto first = request.substr(0, newline);
      const size_t space = first.find(' ');
      if (space == std::string_view::npos) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      const std::string method(first.substr(0, space));
      auto url = first.substr(space + 1);
      if (url.ends_with('\r')) url.remove_suffix(1);
      if ((method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") || url.empty() ||
          std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      if (outCap_ < 4) {
        result_ = DSN_ERR_TOO_BIG;
        break;
      }
      std::vector<HttpDownloader::Header> headers;
      headers.reserve(16);
      result_ = 0;
      for (size_t pos = newline; pos != std::string_view::npos && ++pos < request.size();) {
        const size_t end = request.find('\n', pos);
        auto line = request.substr(pos, end == std::string_view::npos ? end : end - pos);
        pos = end;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() && (end == std::string_view::npos || end + 1 == request.size())) break;
        const size_t colon = line.find(':');
        const auto name = line.substr(0, colon);
        if (colon == std::string_view::npos || name.empty() ||
            name.find_first_not_of("!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") !=
                std::string_view::npos) {
          result_ = DSN_ERR_INVALID;
          break;
        }
        auto value = line.substr(colon + 1);
        if (std::any_of(value.begin(), value.end(),
                        [](unsigned char c) { return (c < 32 && c != '\t') || c == 127; })) {
          result_ = DSN_ERR_INVALID;
          break;
        }
        value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
        if (headers.size() == 16) {
          result_ = DSN_ERR_TOO_BIG;
          break;
        }
        headers.emplace_back(name, value);
        const char* headerName = headers.back().first.c_str();
        if (strcasecmp(headerName, "Host") == 0 || strcasecmp(headerName, "Connection") == 0 ||
            strcasecmp(headerName, "Content-Length") == 0 || strcasecmp(headerName, "Transfer-Encoding") == 0 ||
            strcasecmp(headerName, "X-CrossPoint-Device-ID") == 0 ||
            strcasecmp(headerName, "X-CrossPoint-Device-Model") == 0 ||
            strcasecmp(headerName, "X-CrossPoint-Firmware") == 0) {
          result_ = DSN_ERR_INVALID;
          break;
        }
      }
      if (result_ < 0) break;

      // The installation manifest must explicitly allow physical identity
      // disclosure to its HTTPS origins. dsn_device_id stays app-scoped.
      if (permissions_.allowsDeviceIdentity(url)) {
        readerHardwareIdentity::Id id;
        if (readerHardwareIdentity::read(id)) {
          if (headers.size() > 13) {
            result_ = DSN_ERR_TOO_BIG;
            break;
          }
          headers.emplace_back("X-CrossPoint-Device-ID", id.data());
          headers.emplace_back("X-CrossPoint-Device-Model", devicehardware::model());
          headers.emplace_back("X-CrossPoint-Firmware", std::string(firmwareidentity::ID) + " " + CROSSPOINT_VERSION);
        }
      }

      // Use the same PSRAM-aware TLS reserve as CatalogActivity.
      if (url.size() >= 6 && strncasecmp(url.data(), "https:", 6) == 0 && !tlsHeapAvailable()) {
        LOG_ERR("DSN", "HTTP TLS heap guard: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        result_ = DSN_ERR_IO;
        break;
      }

      // DNS and sockets require an initialized, connected network stack.
      if (WiFi.status() != WL_CONNECTED) {
        result_ = DSN_ERR_IO;
        break;
      }
      LOG_INF("DSN", "HTTP start: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      BoundedOutput response(outCap_ - 4);
      const int status = HttpDownloader::request(
          method.c_str(), std::string(url), headers, body_, bodySize_,
          [&](const uint8_t* bytes, size_t len) { return response.append(bytes, len); }, &networkCancelled_);
      LOG_INF("DSN", "HTTP finished: status=%d output_error=%d", status, response.error);
      if (status == 0) {
        result_ = response.error ? response.error : DSN_ERR_IO;
        break;
      }
#ifndef BOARD_HAS_PSRAM
      if (networkDetached_) {
        detachedResponseSize_ = response.size() + 4;
        detachedResponse_ = makeUniqueNoThrow<uint8_t[]>(detachedResponseSize_);
        if (!detachedResponse_) {
          detachedResponseSize_ = 0;
          result_ = DSN_ERR_IO;
          break;
        }
        out_ = detachedResponse_.get();
      }
#endif
      out_[0] = (status >> 8) & 0xff;
      out_[1] = status & 0xff;
      out_[2] = 0;
      out_[3] = 0;
      result_ = response.copyTo(out_ + 4);
      break;
    }
    case Op::Fetch: {
      size_t bytes = 0;
      if (strncasecmp(url_.c_str(), "https:", 6) == 0 && !tlsHeapAvailable()) {
        LOG_ERR("DSN", "Fetch TLS heap guard: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        result_ = DSN_ERR_IO;
        break;
      }
      // DNS and sockets require an initialized, connected network stack.
      if (WiFi.status() != WL_CONNECTED) {
        result_ = DSN_ERR_IO;
        break;
      }
      const auto error = HttpDownloader::downloadRestricted(
          url_, dest_, &bytes, permissions_.isSharedCredential(dest_) ? MAX_SHARED_CREDENTIAL_SIZE : 16u * 1024 * 1024,
          &networkCancelled_);
      result_ = error == HttpDownloader::OK        ? static_cast<int>(bytes)
                : error == HttpDownloader::TOO_BIG ? DSN_ERR_TOO_BIG
                                                   : DSN_ERR_IO;
      break;
    }
    case Op::Keygen: {
      if (bits_ != 1024) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      RsaKeyPairDer pair;
      if (!crypto_->rsaGenerate(&pair)) {
        result_ = DSN_ERR_IO;
        break;
      }
      if (pair.spki.size() > outCap_ || pair.pkcs8.size() > privCap_) {
        result_ = DSN_ERR_TOO_BIG;
        break;
      }
      memcpy(out_, pair.spki.data(), pair.spki.size());
      memcpy(priv_, pair.pkcs8.data(), pair.pkcs8.size());
      result_ = static_cast<int>(pair.spki.size()) << 16 | static_cast<int>(pair.pkcs8.size());
      break;
    }
    case Op::WifiStart: {
      if (WiFi.status() == WL_CONNECTED) {
        result_ = 0;
        break;
      }
      // The launcher can be the first Wi-Fi user after boot.
      WIFI_STORE.loadFromFile();
      const auto last = WIFI_STORE.getLastConnectedSsid();
      auto cred = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
      if (!cred) cred = WIFI_STORE.getCredentialAt(0);
      if (!cred) {
        result_ = DSN_ERR_UNAVAILABLE;
        break;
      }
      WiFi.persistent(false);
      LOG_INF("DSN", "Wi-Fi start: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      if (!WiFi.mode(WIFI_STA)) {
        result_ = DSN_ERR_IO;
        break;
      }
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      wifiStart_ = millis();
      pending_ = Op::WifiConnecting;
      return false;
    }
    case Op::WifiConnecting:
      if (WiFi.status() == WL_CONNECTED) {
        result_ = 0;
      } else if (millis() - wifiStart_ > 20000) {
        result_ = DSN_ERR_IO;
      } else {
        return false;
      }
      break;
    case Op::None:
    case Op::TextInput:
    case Op::ReadingStats:
      return false;
  }
  cleanupDownload();
#ifndef BOARD_HAS_PSRAM
  if (networkDetached_) {
    completeSuspendedNetwork();
    return true;
  }
#endif
  done_ = true;
  return true;
}

void CrosspointServices::deliverText(const char* text) {
  const size_t n = strlen(text);
  if (n + 1 > outCap_) {
    result_ = DSN_ERR_TOO_BIG;
  } else {
    memcpy(out_, text, n + 1);
    result_ = static_cast<int>(n);
  }
  done_ = true;
}

void CrosspointServices::cancelText() {
  result_ = -1;  // text_input returns -1 on cancel
  done_ = true;
}

// The theme draws its hints upright (Portrait) along the bottom, and none on a
// touch board, so that is the only case with a band to report.
int CrosspointServices::buttonHintsHeight() {
  if (!hintRenderer_ || gpio.hasTouch() || hintRenderer_->getOrientation() != GfxRenderer::Orientation::Portrait) {
    return 0;
  }
  return UITheme::getInstance().getMetrics().buttonHintsHeight;
}

void CrosspointServices::drawButtonHints(freeink::ui::DrawTarget&, const freeink::ui::Rect&,
                                         const char* const labels[4]) {
  if (!hintRenderer_ || !hintInput_) return;
  // The same remapping every firmware screen uses: the app names actions,
  // the user's settings say which key each is on.
  const auto keys = hintInput_->mapLabels(labels[0], labels[1], labels[2], labels[3]);
  GUI.drawButtonHints(*hintRenderer_, keys.btn1, keys.btn2, keys.btn3, keys.btn4);
}

#endif  // CROSSPOINT_DESSEN_APPS
