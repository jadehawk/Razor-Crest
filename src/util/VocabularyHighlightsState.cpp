#include "VocabularyHighlightsState.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <utility>

namespace {
constexpr char kDir[] = "/.crosspoint/vocabulary-highlights";
constexpr char kState[] = "/.crosspoint/vocabulary-highlights/state-v1.json";
constexpr char kTemporary[] = "/.crosspoint/vocabulary-highlights/state-v1.json.tmp";
constexpr char kLegacy[] = "/.crosspoint/vocabulary-builder/state-v1.json";

bool hex16or32(const char* text, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
}  // namespace

void VocabularyHighlightsState::fail(const char* reason) {
  valid_ = false;
  error_ = reason;
  LOG_ERR("VOCAB", "%s", reason);
}

bool VocabularyHighlightsState::validVocabularyId(const char* key) {
  return key && strlen(key) == 38 && strncmp(key, "vocab:", 6) == 0 && hex16or32(key + 6, 32);
}

bool VocabularyHighlightsState::validHiddenId(const char* key) {
  if (validVocabularyId(key)) return true;
  if (!key || strncmp(key, "clip:", 5) != 0) return false;
  if (strlen(key) < 23 || !hex16or32(key + 5, 16) || key[21] != ':' || key[22] < '1' || key[22] > '9') return false;
  for (const char* cursor = key + 23; *cursor; ++cursor)
    if (*cursor < '0' || *cursor > '9') return false;
  return true;
}

bool VocabularyHighlightsState::validate(const ArduinoJson::JsonDocument& doc) {
  if (!doc["version"].is<int>() || doc["version"].as<int>() != 1) return false;
  const ArduinoJson::JsonObjectConst statuses = doc["statuses"].as<ArduinoJson::JsonObjectConst>();
  const ArduinoJson::JsonObjectConst hidden = doc["hidden"].as<ArduinoJson::JsonObjectConst>();
  if (statuses.isNull() || hidden.isNull()) return false;
  for (ArduinoJson::JsonPairConst item : statuses) {
    const char* id = item.key().c_str();
    if (!validVocabularyId(id) || !item.value().is<const char*>()) return false;
    const char* value = item.value().as<const char*>();
    if (strcmp(value, "learning") != 0 && strcmp(value, "mastered") != 0) return false;
  }
  for (ArduinoJson::JsonPairConst item : hidden)
    if (!validHiddenId(item.key().c_str()) || !item.value().is<bool>() || !item.value().as<bool>()) return false;
  return true;
}

bool VocabularyHighlightsState::load() {
  doc_.clear();
  valid_ = false;
  error_.clear();
  const char* path = Storage.exists(kState) ? kState : Storage.exists(kLegacy) ? kLegacy : nullptr;
  if (!path) {
    doc_["version"] = 1;
    doc_["statuses"].to<ArduinoJson::JsonObject>();
    doc_["hidden"].to<ArduinoJson::JsonObject>();
  } else {
    std::string raw;
    // Failure here must not look like an empty document: never overwrite
    // damaged, inaccessible, or oversized browser-managed progress.
    if (!Storage.readFileToString("VOCAB", path, kMaxStateBytes, raw)) {
      fail("Saved progress exceeds native memory limit or cannot be read");
      return false;
    }
    if (deserializeJson(doc_, raw) != ArduinoJson::DeserializationError::Ok) {
      fail("Saved progress JSON is damaged");
      return false;
    }
  }
  if (doc_.overflowed() || !validate(doc_)) {
    fail("Unsupported saved progress; changes are disabled");
    return false;
  }
  valid_ = true;
  return true;
}

VocabularyHighlightsState::Status VocabularyHighlightsState::status(const std::string& id) const {
  if (!valid_) return Status::New;
  const char* value = doc_["statuses"][id.c_str()] | "";
  if (strcmp(value, "learning") == 0) return Status::Learning;
  if (strcmp(value, "mastered") == 0) return Status::Mastered;
  return Status::New;
}

bool VocabularyHighlightsState::hidden(const std::string& id) const {
  return valid_ && (doc_["hidden"][id.c_str()] | false);
}

bool VocabularyHighlightsState::setStatus(const std::string& id, const Status statusValue) {
  if (!validVocabularyId(id.c_str()) || !load()) return false;
  ArduinoJson::JsonObject statuses = doc_["statuses"].as<ArduinoJson::JsonObject>();
  switch (statusValue) {
    case Status::New:
      statuses.remove(id.c_str());
      break;
    case Status::Learning:
      statuses[id.c_str()] = "learning";
      break;
    case Status::Mastered:
      statuses[id.c_str()] = "mastered";
      break;
  }
  return save();
}

bool VocabularyHighlightsState::setHidden(const std::string& id, const bool value) {
  if (!validHiddenId(id.c_str()) || !load()) return false;
  ArduinoJson::JsonObject hidden = doc_["hidden"].as<ArduinoJson::JsonObject>();
  if (value)
    hidden[id.c_str()] = true;
  else
    hidden.remove(id.c_str());
  return save();
}

bool VocabularyHighlightsState::save() {
  // A full round trip preserves unrelated statuses, clipping tombstones and
  // future non-conflicting metadata. No firmware export is ever rewritten.
  if (doc_.overflowed() || !validate(doc_)) {
    fail("Saved progress cannot be serialized safely");
    return false;
  }
  const size_t measured = measureJson(doc_);
  if (measured == 0 || measured > kMaxStateBytes) {
    fail("Saved progress has reached the native size limit");
    return false;
  }
  String json;
  if (!json.reserve(measured) || serializeJson(doc_, json) != measured) {
    fail("Insufficient memory to save vocabulary progress");
    return false;
  }
  if (!Storage.ensureDirectoryExists(kDir)) {
    fail("Could not create vocabulary progress directory");
    return false;
  }
  HalFile file = Storage.open(kTemporary, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    fail("Could not open vocabulary progress temp file");
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
  file.flush();
  const bool closed = file.close();
  if (written != json.length() || !closed) {
    Storage.remove(kTemporary);
    fail("Could not finish vocabulary progress temp file");
    return false;
  }
  if (!Storage.replaceFile(kTemporary, kState)) {
    Storage.remove(kTemporary);
    fail("Could not atomically save vocabulary progress");
    return false;
  }
  // The original legacy path remains untouched as a migration backup.
  return load();
}
