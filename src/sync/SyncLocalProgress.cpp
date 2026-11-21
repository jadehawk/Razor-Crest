#include <CrossPointPosition.h>
#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <ProgressMapper.h>

#include <utility>

#include "SyncEngine.h"

KOReaderProgress syncEngine::localProgress(const std::string& documentHash, const std::string& bookPath, Epub* epub,
                                           const CrossPointPosition& position, const SavedProgressPosition& saved) {
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = saved.xpath;
  progress.percentage = saved.percentage;

  // Rich position for server profiles that explicitly support the CrossPoint extension.
  // The HTTP client enforces the same boundary before serializing the extension.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = saved.percentage < 0.0f ? 0.0f : saved.percentage > 1.0f ? 1.0f : saved.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(position.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(position.pageNumber);
    pos.totalPages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
    if (position.hasParagraphIndex) {
      pos.paragraphIndex = position.paragraphIndex;
    }
    pos.xpath = saved.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto lastSlash = bookPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? bookPath.substr(lastSlash + 1) : bookPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (epub->loadSyncMetadata(syncMetadata)) {
          meta.isbn = std::move(syncMetadata.isbn);
          meta.asin = std::move(syncMetadata.asin);
          meta.series = std::move(syncMetadata.series);
          meta.seriesIndex = syncMetadata.seriesIndex;
        } else {
          LOG_DBG("SYNC", "Could not read extended EPUB metadata; sending core metadata only");
        }
      }
    } else {
      LOG_ERR("SYNC", "Epub unavailable for metadata; sending filename only");
    }
    // Plugin sidecar fields ("<book>.meta.json", written at download time via
    // the catalog sidecar mechanism or /api/plugin-fs) ride along so a custom
    // sync server can route progress by a service book id.
    // A missing sidecar (the common case) leaves extraJson empty.
    Storage.readFileToString("SYNC", bookPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }
  return progress;
}
