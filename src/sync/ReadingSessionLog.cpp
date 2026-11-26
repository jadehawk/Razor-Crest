#include "ReadingSessionLog.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <ReaderHardwareIdentity.h>
#include <TrustedTime.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "SyncRequest.h"

namespace {
constexpr char LOG_PATH[] = "/.crosspoint/reading_sessions.jsonl";
// Upload position in the log, kept per account so a new account receives the whole history.
constexpr char STATE_PATH[] = "/.crosspoint/reading_sessions_sync.json";

constexpr size_t BATCH_SIZE = 20;  // server limit per request
// Bounds one manual sync to 200 sessions when a long history is first uploaded.
constexpr size_t MAX_BATCHES = 10;
constexpr size_t MAX_LINE = 512;
constexpr int MAX_TITLE_BYTES = 80;

// Server limits; a session outside them would make the server reject its whole batch.
constexpr int64_t MIN_START = 946684800;  // 2000-01-01
constexpr uint32_t MIN_SECONDS = 10;
constexpr uint32_t MAX_SECONDS = 7 * 86400;
constexpr int64_t MAX_FUTURE_SECONDS = 300;

int utcOffsetMinutes(const time_t at) {
  struct tm local;
  struct tm utc;
  localtime_r(&at, &local);
  gmtime_r(&at, &utc);
  // Reading the UTC fields as local time shifts the result by exactly the zone offset.
  utc.tm_isdst = local.tm_isdst;
  return std::clamp(static_cast<int>((at - mktime(&utc)) / 60), -720, 840);
}

// Reads the line that starts at offset, without its newline, and moves offset past it.
// An overlong line comes back empty. Returns false, leaving offset alone, when no
// complete line is left: the end of the log, an unfinished last line, or a read error.
bool readLine(HalFile& file, size_t& offset, char* line, const size_t size) {
  size_t next = offset;
  bool overlong = false;
  while (file.seek(next)) {
    const int got = file.read(line, size - 1);
    if (got <= 0) return false;
    if (const void* newline = memchr(line, '\n', got)) {
      const size_t length = static_cast<const char*>(newline) - line;
      line[overlong ? 0 : length] = '\0';
      offset = next + length + 1;
      return true;
    }
    if (static_cast<size_t>(got) < size - 1) return false;
    overlong = true;
    next += got;
  }
  return false;
}

bool isDocumentId(const char* document) {
  return strlen(document) == 32 && strspn(document, "0123456789abcdefABCDEF") == 32;
}

// Unique per device: every CrossPoint reader reports the same device id to the server.
void formatSessionId(char* out, const size_t size, const int64_t start, const char* mac) {
  snprintf(out, size, "%lld-%s", static_cast<long long>(start), mac);
}

bool saveOffset(const std::string& account, const size_t offset) {
  JsonDocument state;
  state["account"] = account;
  state["offset"] = offset;
  return PersistableStoreBase::writeDocToFile(STATE_PATH, state);
}
}  // namespace

bool readingSessionLog::append(const std::string& document, const std::string& bookPath, const std::string& title,
                               const int64_t startTime, const uint32_t seconds, const uint16_t startProgressBp,
                               const uint16_t endProgressBp, const LocalDetails* local) {
  if (!isDocumentId(document.c_str())) return false;
  if (local && (local->trackedSeconds < MIN_SECONDS || local->trackedSeconds > MAX_SECONDS ||
                seconds > local->trackedSeconds || seconds < MIN_SECONDS))
    return false;
  JsonDocument doc;
  doc["doc"] = document;
  doc["start"] = startTime;
  doc["secs"] = seconds;
  if (startTime >= MIN_START) doc["tz"] = utcOffsetMinutes(static_cast<time_t>(startTime));
  doc["from"] = startProgressBp;
  doc["to"] = endProgressBp;
  doc["title"] = title.substr(0, utf8SafeTruncateBuffer(title.data(), std::min<int>(title.size(), MAX_TITLE_BYTES)));
  doc["path"] = bookPath;
  readerHardwareIdentity::Id hardware;
  if (readerHardwareIdentity::read(hardware)) {
    doc["device"] = readerHardwareIdentity::compact(hardware);
  }
  if (local) {
    doc["v"] = 2;
    doc["tracked_secs"] = local->trackedSeconds;
    doc["turns"] = local->turns;
    auto metadata = [&](const char* key, const std::string& value, int limit) {
      if (!value.empty())
        doc[key] = value.substr(0, utf8SafeTruncateBuffer(value.data(), std::min<int>(value.size(), limit)));
    };
    metadata("author", local->author, 80);
    metadata("series", local->series, 80);
    metadata("series_index", local->seriesIndex, 12);
  }
  // One spare byte in front for a newline that ends an unfinished line already in the log.
  const auto buffer = makeUniqueNoThrow<char[]>(MAX_LINE + 2);
  if (!buffer) return false;
  char* line = buffer.get() + 1;
  size_t length = SyncRequest::serialize(doc, line, MAX_LINE);
  if (length == 0) {
    // upload() skips lines longer than its buffer, so a very long path is left out.
    doc.remove("path");
    length = SyncRequest::serialize(doc, line, MAX_LINE);
  }
  // Escape expansion can exceed the limit even after byte truncation.
  // Preserve the numerical data and identity; shrink optional strings only.
  if (!length && local) {
    for (const char* key : {"series", "author", "title"}) {
      if (length) break;
      const auto value = doc[key].as<std::string>();
      for (int cap = 48; cap >= 0 && !length; cap -= 16) {
        doc[key] = value.substr(0, utf8SafeTruncateBuffer(value.data(), std::min<int>(value.size(), cap)));
        length = SyncRequest::serialize(doc, line, MAX_LINE);
      }
    }
    if (!length) {
      doc.remove("series_index");
      length = SyncRequest::serialize(doc, line, MAX_LINE);
    }
  }
  if (length == 0) return false;
  line[length++] = '\n';

  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(LOG_PATH, O_RDWR | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("RSL", "Failed to open the reading session log");
    return false;
  }
  const size_t sizeBefore = file.size();
  // A power cut can leave an unfinished line, which would swallow this record.
  char last = '\n';
  if (sizeBefore > 0 && file.seek(sizeBefore - 1)) file.read(&last, 1);
  if (last != '\n') {
    *--line = '\n';
    ++length;
  }
  const bool written = file.write(line, length) == length;
  // A partial line would swallow the next session appended after it.
  if (!written) file.truncate(sizeBefore);
  const bool closed = file.close();
  if (!written || !closed) LOG_ERR("RSL", "Failed to persist a reading session");
  return written && closed;
}

bool readingSessionLog::upload(SyncRequest& sync) {
  if (!Storage.exists(LOG_PATH)) return true;
  HalFile file = Storage.open(LOG_PATH);
  if (!file) return false;

  const std::string account = SyncRequest::account();
  size_t offset = 0;
  {
    JsonDocument state;
    if (PersistableStoreBase::readDocFromFile(STATE_PATH, state) && account == (state["account"] | "")) {
      offset = state["offset"] | static_cast<size_t>(0);
    }
  }
  // A log that shrank was replaced; start over. The server ignores sessions it already holds.
  if (offset > file.size()) offset = 0;

  readerHardwareIdentity::Id hardware;
  const bool haveHardware = readerHardwareIdentity::read(hardware);
  if (!haveHardware) return false;
  const int64_t now = trustedtime::trustedNow();
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/sessions";
  bool ok = true;
  size_t consumed = offset;  // end of the last complete line read
  JsonDocument doc;
  // On the heap: the loop task's stack is small and TLS runs below this frame.
  const auto lineBuffer = makeUniqueNoThrow<char[]>(MAX_LINE + 1);
  if (!lineBuffer) return false;
  char* const line = lineBuffer.get();
  // Store the actual outgoing IDs for acknowledgement, including sessions
  // recorded before an SD card was moved to another physical device.
  const auto ids = makeUniqueNoThrow<std::array<char, 40>[]>(BATCH_SIZE);
  if (!ids) return false;
  char sessionId[40];

  for (size_t batch = 0; ok && batch < MAX_BATCHES; ++batch) {
    // Written straight into the request buffer: every value is a number or checked hex, so nothing needs escaping.
    char* const body = sync.body();
    size_t length =
        snprintf(body, SyncRequest::BUFFER_SIZE, "{\"device_id\":\"%s\",\"items\":[", KOReaderSyncClient::DEVICE_ID);
    size_t count = 0;
    bool endOfLog = false;
    while (count < BATCH_SIZE) {
      if (!readLine(file, consumed, line, MAX_LINE + 1)) {
        endOfLog = true;
        break;
      }
      JsonDocument entry;
      if (!SyncRequest::parse(entry, line, strlen(line))) continue;
      // Historical checkpoint rows are local aggregates, not measured sessions.
      // Never send them to the stock session endpoint, even if future metadata
      // includes a field resembling an ordinary session.
      const auto type = entry["type"];
      if (!type.isNull() && (!type.is<const char*>() || strcmp(type.as<const char*>(), "session") != 0)) continue;
      const char* document = entry["doc"] | "";
      const int64_t start = entry["start"] | static_cast<int64_t>(0);
      const uint32_t seconds = entry["secs"] | static_cast<uint32_t>(0);
      // A session dated in the future was measured with a wrong clock; it stays in the log only.
      if (!isDocumentId(document) || start < MIN_START || seconds < MIN_SECONDS || seconds > MAX_SECONDS ||
          (now > 0 && start + seconds > now + MAX_FUTURE_SECONDS)) {
        continue;
      }
      const char* recorded = entry["device"] | "";
      const char* mac =
          readerHardwareIdentity::validMac(recorded) ? recorded : readerHardwareIdentity::compact(hardware);
      formatSessionId(sessionId, sizeof(sessionId), start, mac);
      if (std::any_of(ids.get(), ids.get() + count,
                      [&](const auto& id) { return std::strcmp(id.data(), sessionId) == 0; }))
        continue;
      length += snprintf(body + length, SyncRequest::BUFFER_SIZE - length,
                         "%s{\"session_id\":\"%s\",\"document\":\"%s\",\"started_at\":%lld,\"ended_at\":%lld,"
                         "\"seconds\":%lu,\"utc_offset_minutes\":%d}",
                         count ? "," : "", sessionId, document, static_cast<long long>(start),
                         static_cast<long long>(start + seconds), static_cast<unsigned long>(seconds),
                         std::clamp(entry["tz"] | 0, -720, 840));
      std::strcpy(ids[count++].data(), sessionId);
    }

    if (count > 0) {
      length += snprintf(body + length, SyncRequest::BUFFER_SIZE - length, "]}");
      if (!sync.sendBody(url, length, doc)) {
        ok = false;
        break;
      }
      // The server lists what it stored; an older server answers 200 without storing anything.
      const JsonArrayConst accepted = doc["accepted_session_ids"].as<JsonArrayConst>();
      for (size_t i = 0; i < count && ok; ++i) {
        ok = false;
        for (JsonVariantConst id : accepted) {
          if (strcmp(id | "", ids[i].data()) == 0) ok = true;
        }
      }
      if (!ok) {
        LOG_ERR("RSL", "Server did not acknowledge reading sessions");
        break;
      }
    }
    if (consumed != offset) {
      offset = consumed;
      ok = saveOffset(account, offset);
    }
    if (endOfLog) break;
  }
  file.close();
  return ok;
}
