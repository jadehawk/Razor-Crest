#pragma once

#include <KOReaderSyncClient.h>

#include <optional>
#include <string>

class Epub;
struct CrossPointPosition;
struct SavedProgressPosition;

// One sync of one book with the configured server.
//
// Reading progress is the standard KOSync exchange (KOReaderSyncClient) and is
// all that a plain KOSync server ever receives. Servers that offer the
// CrossPoint Sync extensions also get clippings (when enabled), bookmarks, and
// reading sessions.
namespace syncEngine {

// The document id for the configured match method; empty when the book cannot be read.
std::string documentId(const std::string& bookPath);

// What the server holds at the start of a sync.
struct Pull {
  std::string documentHash;      // empty: the document id could not be calculated
  bool extensionsSynced = true;  // false: Sync Clippings is enabled and failed; progress was not fetched
  KOReaderSyncClient::Error result = KOReaderSyncClient::OK;  // NOT_FOUND: the server has no progress
  KOReaderProgress remote;
  // Smart sync only: the record stored under the other match method's id, for
  // the caller to compare once both positions can be mapped into the book.
  std::optional<KOReaderProgress> alternate;
};

// Syncs the extensions, then fetches remote progress. Bookmarks and reading
// sessions are best effort; what was not sent stays queued on the SD card.
// C3 callers hold a FrameBufferLoan with rendering locked until this returns.
Pull pull(const std::string& bookPath, bool smartSync, bool uploadOnly = false);

// Uploads the position the user or smart sync chose.
KOReaderSyncClient::Error push(const KOReaderProgress& progress);

// Builds the upload for a local position: the rich position and book metadata
// are included when the server type and settings allow them. epub may be null;
// the metadata then carries the filename only.
KOReaderProgress localProgress(const std::string& documentHash, const std::string& bookPath, Epub* epub,
                               const CrossPointPosition& position, const SavedProgressPosition& saved);

}  // namespace syncEngine
