#pragma once

#include <Print.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // Persistent ZIP handle. Constructing a temporary ZipFile per item read
  // re-ran loadZipDetails() (~130 ms EOCD scan) and reset the central
  // directory cursor on every single lookup — measured at ~340 ms per comic
  // image, which is most of the fixed cost of extracting one.
  mutable std::unique_ptr<ZipFile> zipHandle;
  ZipFile& zip() const;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Optional load-progress hook (plain function pointer: no std::function in
  // the parse path). percent is 0-100 within load().
  void (*progressFn)(int percent, void* ctx) = nullptr;
  void* progressCtx = nullptr;
  // Optional cancellation flag polled at stream-chunk / build-loop / stage
  // boundaries inside load(). Set only by the background pre-indexer; the
  // foreground reader leaves it null so every check compiles to one branch.
  const std::atomic<bool>* cancelFlag = nullptr;

  void reportProgress(int percent) const {
    if (progressFn) {
      progressFn(percent, progressCtx);
    }
  }

  bool cancelled() const { return cancelFlag && cancelFlag->load(std::memory_order_relaxed); }

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

 public:
  // Out of line in Epub.cpp: ZipFile is incomplete here, and unique_ptr's
  // deleter is instantiated from the constructor's cleanup path.
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub();
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  // Install a progress callback invoked at stage boundaries inside load().
  // Pass nullptr to remove it (also done by the reader once load returns).
  void setProgress(void (*fn)(int percent, void* ctx), void* ctx) {
    progressFn = fn;
    progressCtx = ctx;
  }
  // Install a cancellation flag polled inside load() at stream-chunk, build-
  // loop and stage boundaries, so a background index run stops within ~ms of
  // a reader taking over. Pass nullptr to remove (default: unset).
  void setCancel(const std::atomic<bool>* flag) { cancelFlag = flag; }
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
