#pragma once

#include "../Source.h"

#include <functional>

namespace Mosaic {

/** Result of importing a remote image into the local library. */
struct ImportResult {
    bool ok = false;
    QString filePath;
    QString error;
    bool alreadyExisted = false;
};

// Data-access layer for the library catalog (DuckDB images table) and the
// flat md5-named library folder. Synchronous and thread-safe.
class LibraryCatalog {
   public:
    // Orders library posts for the "feed:NN" search modifier: returns the
    // md5s best-first. `exploration` is in [0, 1] (0 = pure taste, 1 =
    // pure exploration). Posts carry md5, tags, format and createdAt only.
    // Called on search worker threads; `progress` may be empty.
    using FeedRanker = std::function<QStringList(
        const QList<SourceImage>& posts, double exploration, const SearchProgress& progress)>;
    // Installed by the app once the feed recommender is available; an
    // empty function removes it ("feed:" then falls back to newest first).
    static void setFeedRanker(FeedRanker ranker);

    // queries

    SourceResult search(const SourceQuery& query);

    // Cheaper than search("md5:...") for single lookups.
    bool lookupByMd5(const QString& md5, SourceImage* out) const;

    bool hasImage(const QString& md5) const;

    // md5 of the cataloged image saved from this source's post, or empty
    // when that post is not in the library.
    QString md5ForPost(const QString& sourceId, qint64 remoteId) const;

    // All cataloged images carrying the given tag (exact name, optional
    // category); used for the startup notags refresh pass.
    QList<SourceImage> imagesWithTag(const QString& tagName,
                                     const QString& category = QString()) const;

    // files

    static QString filePathFor(const QString& md5, const QString& format);

    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) const;

    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) const;

    // relationships

    SourceImage parentOf(const SourceImage& image, QString* error = nullptr);
    QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr);

    // True when the two images share a parent/child chain.
    bool inParentChildChain(const QString& md5A, const QString& md5B);

    // mutation

    // Content-addressed store: identical files are stored once; the
    // original creation timestamp is preserved.
    ImportResult importImage(const SourceImage& meta, const QByteArray& data);

    bool removeImage(const QString& md5, QString* error = nullptr);

    // favorites

    bool setFavorite(const QString& md5, bool favorite, QString* error = nullptr);

    bool isFavorite(const QString& md5) const;
};

}  // namespace Mosaic
