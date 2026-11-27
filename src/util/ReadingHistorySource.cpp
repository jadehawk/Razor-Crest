#include "ReadingHistorySource.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ReaderHardwareIdentity.h>
#include <esp_random.h>
#include <sdk/dessen_abi.h>

#include <cstring>

#include "ReadingHistoryProvider.h"

namespace readingHistory {
namespace {
constexpr char PATH[] = "/.crosspoint/reading_sessions.jsonl";

void put(uint8_t* out, uint64_t value, unsigned n) {
  for (unsigned i = 0; i < n; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
}  // namespace
int readSourceIdentity(const Provider& provider, uint32_t mode, uint8_t* out, uint32_t cap) {
  const auto* s = provider.summary();
  if (!s || mode > 1) return DSN_ERR_INVALID;
  if (s->rejected || s->incompleteTail) {
    LOG_ERR("RHIST", "Source identity rejected: invalid or incomplete history");
    return DSN_ERR_INVALID;
  }
  if (s->sourceConflict) {
    LOG_ERR("RHIST", "Source identity rejected: mixed history ownership");
    return DSN_ERR_DENIED;
  }
  readerHardwareIdentity::Id reader;
  if (!readerHardwareIdentity::read(reader)) return DSN_ERR_IO;
  if (s->sourceMac[0] && strcmp(s->sourceMac.data(), readerHardwareIdentity::compact(reader))) {
    LOG_ERR("RHIST", "Source identity rejected: different physical reader");
    return DSN_ERR_DENIED;
  }
  const bool owned = s->sourceId != DocumentId{};
  if (!mode) {
    if (!out || cap < 40) return DSN_ERR_TOO_BIG;
    put(out, 1, 4);
    put(out + 4, owned ? 1 : 0, 4);
    memcpy(out + 8, s->sourceId.data(), 16);
    put(out + 24, s->bytes, 8);
    put(out + 32, provider.verifiedSourceHash(), 8);
    return 40;
  }
  if (owned || !s->bytes || (!s->overall.sessions && !s->overall.seconds) || cap != 0) {
    LOG_ERR("RHIST", "Cannot claim history: owned=%u bytes=%llu sessions=%llu seconds=%llu",
            static_cast<unsigned>(owned), static_cast<unsigned long long>(s->bytes),
            static_cast<unsigned long long>(s->overall.sessions), static_cast<unsigned long long>(s->overall.seconds));
    return DSN_ERR_INVALID;
  }
  DocumentId id{};
  esp_fill_random(id.data(), id.size());
  if (id == DocumentId{}) return DSN_ERR_IO;
  constexpr char hex[] = "0123456789abcdef";
  char text[33]{};
  for (size_t i = 0; i < id.size(); ++i) {
    text[2 * i] = hex[id[i] >> 4];
    text[2 * i + 1] = hex[id[i] & 15];
  }
  char line[160];
  const int n =
      snprintf(line, sizeof(line), "{\"type\":\"rc-history-owner\",\"v\":1,\"history_id\":\"%s\",\"device\":\"%s\"}\n",
               text, readerHardwareIdentity::compact(reader));
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return -1;
  auto file = Storage.open(PATH, O_RDWR | O_APPEND);
  if (!file || file.fileSize64() != s->bytes) return DSN_ERR_IO;
  const bool written = file.write(line, n) == static_cast<size_t>(n);
  if (!written) file.truncate(s->bytes);
  if (!file.close() || !written) return DSN_ERR_IO;
  LOG_INF("RHIST", "Local history identity bound");
  return 0;
}
}  // namespace readingHistory
