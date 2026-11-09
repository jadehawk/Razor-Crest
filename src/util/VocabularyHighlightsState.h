#pragma once

#include <ArduinoJson.h>
#include <BoardConfig.h>

#include <cstdint>
#include <string>

// Shared on-SD state with the browser WebUI. Never edits firmware export files.
// Conservative read cap protects the no-PSRAM X3 from oversized JSON allocations.
class VocabularyHighlightsState {
 public:
  enum class Status : uint8_t { New, Learning, Mastered };

  bool load();
  bool writable() const { return valid_; }
  const std::string& error() const { return error_; }
  Status status(const std::string& id) const;
  bool hidden(const std::string& id) const;
  bool setStatus(const std::string& id, Status value);
  bool setHidden(const std::string& id, bool value);

 private:
#if FREEINK_DEVICE_X3
  static constexpr size_t kMaxStateBytes = 8192;
#else
  static constexpr size_t kMaxStateBytes = 24576;
#endif
  ArduinoJson::JsonDocument doc_;
  bool valid_ = false;
  std::string error_;

  bool save();
  void fail(const char* reason);
  static bool validVocabularyId(const char* key);
  static bool validHiddenId(const char* key);
  static bool validate(const ArduinoJson::JsonDocument& doc);
};
