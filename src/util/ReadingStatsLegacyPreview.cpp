#include "ReadingStatsLegacyPreview.h"

#include <HalStorage.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include <cstring>
#include <string>

namespace readingstats::legacy {
namespace {
constexpr char kCrossInk[] = "/.crosspoint/global_stats.bin";
constexpr char kVCodex[] = "/.crosspoint/reading_stats.json";
constexpr uint32_t kMaxBooks = 2048;
constexpr size_t kMaxSourceBytes = 4 * 1024 * 1024;

class BufferedReader {
 public:
  explicit BufferedReader(HalFile& file) : file_(file) {}
  int next() {
    if (pushed_ >= 0) {
      const int c = pushed_;
      pushed_ = -1;
      return c;
    }
    if (pos_ == end_) {
#if defined(ARDUINO)
      if ((++reads_ & 7u) == 0) yield();
#endif
      const int count = file_.read(bytes_, sizeof(bytes_));
      if (count <= 0) return -1;
      pos_ = 0;
      end_ = static_cast<size_t>(count);
    }
    return bytes_[pos_++];
  }
  void putBack(int c) { pushed_ = c; }

 private:
  HalFile& file_;
  uint8_t bytes_[512] = {};
  size_t pos_ = 0;
  size_t end_ = 0;
  int pushed_ = -1;
#if defined(ARDUINO)
  uint32_t reads_ = 0;
#endif
};

bool space(int c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
int nonspace(BufferedReader& reader) {
  int c = -1;
  do {
    c = reader.next();
  } while (space(c));
  return c;
}

// A bounded JSON string token; enough for path/field names, without a
// dynamically expanding allocation for readingDays or session history.
bool stringToken(BufferedReader& reader, std::string& result, const size_t limit = 256) {
  result.clear();
  bool escape = false;
  for (;;) {
    const int c = reader.next();
    if (c < 0) return false;
    if (!escape && c == '"') return true;
    if (!escape && c == '\\') {
      escape = true;
      continue;
    }
    if (result.size() >= limit) return false;
    result.push_back(static_cast<char>(c));
    escape = false;
  }
}

bool skipValue(BufferedReader& reader) {
  const int first = nonspace(reader);
  if (first < 0) return false;
  if (first == '"') {
    std::string discard;
    return stringToken(reader, discard, 512);
  }
  if (first == '{' || first == '[') {
    int depth = 1;
    bool quoted = false;
    bool escaped = false;
    for (;;) {
      const int c = reader.next();
      if (c < 0) return false;
      if (quoted) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = false;
      } else if (c == '"')
        quoted = true;
      else if (c == '{' || c == '[')
        ++depth;
      else if (c == '}' || c == ']')
        --depth;
      if (depth == 0) return true;
    }
  }
  int c = first;
  while (c >= 0 && c != ',' && c != '}' && c != ']') c = reader.next();
  if (c < 0) return false;
  reader.putBack(c);
  return true;
}

bool readUnsigned(BufferedReader& reader, uint64_t& result) {
  int c = nonspace(reader);
  if (c < '0' || c > '9') return false;
  result = 0;
  do {
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
    c = reader.next();
  } while (c >= '0' && c <= '9');
  if (c != ',' && c != '}' && !space(c)) return false;
  reader.putBack(c);
  return true;
}

bool locateBooks(BufferedReader& reader) {
  bool isFormatV6 = false;
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  while (true) {
    const int c = reader.next();
    if (c < 0) return false;
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
      continue;
    }
    if (c == '"' && depth == 1) {
      std::string key;
      if (!stringToken(reader, key, 64)) return false;
      if (key == "formatVersion") {
        uint64_t version = 0;
        if (nonspace(reader) != ':' || !readUnsigned(reader, version) || version != 6 || nonspace(reader) != ',')
          return false;
        isFormatV6 = true;
        continue;
      }
      if (key == "books") return isFormatV6 && nonspace(reader) == ':' && nonspace(reader) == '[';
      continue;
    }
    if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[')
      ++depth;
    else if (c == '}' || c == ']')
      --depth;
    if (depth < 0) return false;
  }
}

bool readBookDays(BufferedReader& reader, Snapshot& destination) {
  if (nonspace(reader) != '[') return false;
  for (;;) {
    const int first = nonspace(reader);
    if (first == ']') return true;
    if (first != '{') return false;
    uint64_t rawDay = 0;
    uint64_t milliseconds = 0;
    bool hasDay = false, hasTime = false;
    for (;;) {
      const int c = nonspace(reader);
      if (c != '"') return false;
      std::string key;
      if (!stringToken(reader, key, 64) || nonspace(reader) != ':') return false;
      if (key == "dayOrdinal") {
        if (!readUnsigned(reader, rawDay) || rawDay > UINT32_MAX) return false;
        hasDay = true;
      } else if (key == "readingMs") {
        if (!readUnsigned(reader, milliseconds)) return false;
        hasTime = true;
      } else if (!skipValue(reader)) {
        return false;
      }
      const int separator = nonspace(reader);
      if (separator == '}') break;
      if (separator != ',') return false;
    }
    if (!hasDay || !hasTime) return false;
    uint32_t day = 0;
    if (!vCodexDay(static_cast<uint32_t>(rawDay), day)) return false;
    // Snapshot stores seconds, not milliseconds. This deliberate rounding
    // retains the exact all-time milliseconds/1000 separately below.
    const uint64_t seconds = milliseconds / 1000;
    if (seconds > UINT32_MAX) return false;
    destination.addSession(static_cast<uint32_t>(seconds), 0, day);
    const int separator = nonspace(reader);
    if (separator == ']') return true;
    if (separator != ',') return false;
  }
}

// Source stream is already immediately after '{'. Only these three scalar
// fields are needed for preview; no whole-document or whole-book JSON tree.
bool readBook(BufferedReader& reader, uint64_t& milliseconds, uint32_t& sessions, BookRecord* book = nullptr) {
  bool hasPath = false, hasTime = false, hasSessions = false;
  milliseconds = 0;
  sessions = 0;
  if (book) *book = BookRecord{};
  for (;;) {
    const int c = nonspace(reader);
    if (c == '}') return hasPath && hasTime && hasSessions;
    if (c != '"') return false;
    std::string key;
    if (!stringToken(reader, key, 64) || nonspace(reader) != ':') return false;
    if (key == "path") {
      if (nonspace(reader) != '"') return false;
      std::string path;
      if (!stringToken(reader, path) || path.empty() || path[0] != '/' || path.find("..") != std::string::npos ||
          path.find('\\') != std::string::npos)
        return false;
      if (path.size() >= 176) return false;  // Native book index must retain full path.
      if (book) book->path = std::move(path);
      hasPath = true;
    } else if (book && (key == "bookId" || key == "title" || key == "author")) {
      if (nonspace(reader) != '"') return false;
      std::string value;
      if (!stringToken(reader, value, 192)) return false;
      if (key == "bookId")
        book->key = std::move(value);
      else if (key == "title")
        book->title = std::move(value);
      else
        book->author = std::move(value);
    } else if (book && key == "readingDays") {
      if (!readBookDays(reader, book->delta)) return false;
    } else if (key == "totalReadingMs") {
      if (!readUnsigned(reader, milliseconds)) return false;
      hasTime = true;
    } else if (key == "sessions") {
      uint64_t count = 0;
      if (!readUnsigned(reader, count) || count > UINT32_MAX) return false;
      sessions = static_cast<uint32_t>(count);
      hasSessions = true;
    } else if (!skipValue(reader)) {
      return false;
    }
    const int separator = nonspace(reader);
    if (separator == '}') return hasPath && hasTime && hasSessions;
    if (separator != ',') return false;
  }
}

Preview inspectVCodex() {
  Preview preview;
  preview.present = Storage.exists(kVCodex);
  if (!preview.present) return preview;
  HalFile file;
  if (!Storage.openFileForRead("RST", kVCodex, file)) return preview;
  if (file.fileSize() > kMaxSourceBytes) {
    preview.truncated = true;
    file.close();
    return preview;
  }
  BufferedReader reader(file);
  if (!locateBooks(reader)) {
    file.close();
    return preview;
  }

  bool valid = true;
  uint64_t milliseconds = 0;
  uint64_t totalMilliseconds = 0;
  for (;;) {
    const int c = nonspace(reader);
    if (c == ']') break;
    if (c != '{' || preview.books >= kMaxBooks) {
      valid = false;
      preview.truncated = true;
      break;
    }
    uint32_t sessions = 0;
    if (!readBook(reader, milliseconds, sessions) || milliseconds > UINT64_MAX - totalMilliseconds) {
      valid = false;
      break;
    }
    totalMilliseconds += milliseconds;
    preview.totals.sessions =
        sessions > UINT32_MAX - preview.totals.sessions ? UINT32_MAX : preview.totals.sessions + sessions;
    ++preview.books;
    const int delimiter = nonspace(reader);
    if (delimiter == ']') break;
    if (delimiter != ',') {
      valid = false;
      break;
    }
  }
  if (valid) valid = nonspace(reader) == '}' && nonspace(reader) == -1;
  file.close();
  preview.supported = valid;
  if (valid) preview.totals.totalSeconds = totalMilliseconds / 1000;
  return preview;
}

Preview inspectCrossInk() {
  Preview preview;
  preview.present = Storage.exists(kCrossInk);
  if (!preview.present) return preview;
  HalFile file;
  if (!Storage.openFileForRead("RST", kCrossInk, file)) return preview;
  uint8_t bytes[159] = {};
  const size_t size = file.fileSize();
  const int count = size <= sizeof(bytes) ? file.read(bytes, size) : -1;
  file.close();
  preview.supported = count == static_cast<int>(size) && crossInkGlobal(bytes, size, preview.totals);
  return preview;
}
}  // namespace

bool scanVCodexBooks(BookVisitor visitor, void* user, uint32_t& seen) {
  seen = 0;
  if (!visitor || !inspectVCodex().supported) return false;
  HalFile file;
  if (!Storage.openFileForRead("RST", kVCodex, file)) return false;
  BufferedReader reader(file);
  if (!locateBooks(reader)) {
    file.close();
    return false;
  }
  bool ok = true;
  for (;;) {
    const int first = nonspace(reader);
    if (first == ']') break;
    if (first != '{' || seen >= kMaxBooks) {
      ok = false;
      break;
    }
    BookRecord book{};
    uint64_t milliseconds = 0;
    uint32_t sessions = 0;
    if (!readBook(reader, milliseconds, sessions, &book) || book.path.empty()) {
      ok = false;
      break;
    }
    if (book.key.empty()) book.key = book.path;
    book.delta.totalSecondsLo = static_cast<uint32_t>(milliseconds / 1000);
    book.delta.totalSecondsHi = static_cast<uint32_t>((milliseconds / 1000) >> 32);
    book.delta.sessions = sessions;
    book.delta.pageTurns = 0;  // vCodex doesn't store qualified turns.
    book.delta.seal();
    ++seen;
    if (!visitor(book, user)) {
      ok = false;
      break;
    }
    const int delimiter = nonspace(reader);
    if (delimiter == ']') break;
    if (delimiter != ',') {
      ok = false;
      break;
    }
  }
  if (ok) ok = nonspace(reader) == '}' && nonspace(reader) == -1;
  file.close();
  return ok;
}

Preview inspect(Source source) { return source == Source::CrossInk ? inspectCrossInk() : inspectVCodex(); }
}  // namespace readingstats::legacy
