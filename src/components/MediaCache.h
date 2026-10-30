#pragma once

#include "../sources/Source.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

class QPointerBase;

namespace Mosaic {

// Shared media byte-cache + prefetch pipeline. Caches raw bytes per
// "<md5>:<kind>", prefetches neighbours off-thread, delivers via
// std::function callbacks on the GUI thread.
class MediaCache : public QObject {
    Q_OBJECT

   public:
    explicit MediaCache(QObject* parent = nullptr);

    static QString cacheKey(const QString& md5, FileKind kind);

    // Cached raw bytes for one variant; empty when not cached.
    QByteArray cached(const QString& md5, FileKind kind) const;

    QImage cachedPreviewImage(const QString& md5) const;

    bool inFlight(const QString& md5, FileKind kind) const;

    // Fetches into the cache unless already cached or in flight; onReady
    // fires on the GUI thread afterwards.
    void fetchIntoCache(Source* source, const SourceImage& image, FileKind kind,
                        std::function<void()> onReady = {});

    // Progressive load: cached Full bytes instantly, otherwise preview
    // first and full bytes when they arrive. Returns true when the full
    // media was delivered synchronously. A failed full fetch calls onFull
    // with empty bytes; within kFailureTtlMs of a failure nothing is started
    // (inFlight() stays false for the Full variant).
    bool request(Source* source, const SourceImage& image,
                 const std::function<void(const QByteArray&)>& onPreview,
                 const std::function<void(const QByteArray&)>& onFull);

    void invalidate(const QString& md5);

    void setMaxEntries(int entries) { m_maxEntries = qMax(2, entries); }
    void setMaxBytes(qint64 bytes) { m_maxBytes = qMax<qint64>(1, bytes); }
    void setKeepRadius(int radius) { m_keepRadius = qMax(0, radius); }

   private:
    // Inserts (or replaces) a blob and evicts down to the limits. Blobs
    // larger than kMaxEntryBytes are not kept: they are delivered to the
    // callbacks but would push everything else out of the cache.
    void store(const QString& key, const QByteArray& bytes);
    void drop(const QString& key);
    void evict();
    // Drops blobs far from the current position.
    void evictDistant();

    QHash<QString, QByteArray> m_cache;
    QStringList m_cacheOrder;
    QSet<QString> m_inFlight;
    // key -> ms timestamp of the last failed fetch. A dead URL is not
    // retried on every scroll/navigation, only after kFailureTtlMs.
    QHash<QString, qint64> m_failedAt;
    bool recentlyFailed(const QString& key) const;
    void markFetched(const QString& key, bool ok);

    int m_maxEntries = 8;
    // Entry counts alone are unbounded in bytes (full videos run to
    // hundreds of MB each), so the total is capped as well.
    qint64 m_maxBytes = 512LL * 1024 * 1024;
    qint64 m_totalBytes = 0;
    int m_keepRadius = 4;

    /// Subscribers waiting for an in-flight fetch to finish (key -> list).
    /// Without this, a post prefetched WITHOUT a callback (preloadNext) would
    /// leave a later showPost waiting forever.
    QHash<QString, QList<std::function<void(const QByteArray&)>>> m_subscribers;

    // Runs `callback` when the fetch for `key` completes (immediately if
    // nothing is in flight). Returns true when subscribed/waiting.
    bool subscribe(const QString& md5, FileKind kind,
                   const std::function<void(const QByteArray&)>& callback);
};

}  // namespace Mosaic
