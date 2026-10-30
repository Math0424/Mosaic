#include "MediaCache.h"

#include "../core/ThreadPools.h"
#include "../sources/local/LibrarySource.h"

#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

namespace Mosaic {

namespace {

constexpr int kPreviewMaxBytes = 24 * 1024 * 1024;  // sanity cap for decode attempts
constexpr qint64 kMaxEntryBytes = 128LL * 1024 * 1024;  // larger blobs are not cached
constexpr qint64 kFailureTtlMs = 30000;  // short: failures are often rate limits

}  // namespace

MediaCache::MediaCache(QObject* parent) : QObject(parent) {}

QString MediaCache::cacheKey(const QString& md5, FileKind kind) {
    return md5 + QLatin1Char(':') + QString::number(static_cast<int>(kind));
}

QByteArray MediaCache::cached(const QString& md5, FileKind kind) const {
    return m_cache.value(cacheKey(md5, kind));
}

QImage MediaCache::cachedPreviewImage(const QString& md5) const {
    for (FileKind kind : {FileKind::Sample, FileKind::Preview}) {
        const QByteArray bytes = m_cache.value(cacheKey(md5, kind));
        if (bytes.isEmpty() || bytes.size() > kPreviewMaxBytes)
            continue;
        QImage image;
        if (image.loadFromData(bytes))
            return image;
    }
    return {};
}

bool MediaCache::recentlyFailed(const QString& key) const {
    const auto it = m_failedAt.constFind(key);
    return it != m_failedAt.constEnd() &&
           QDateTime::currentMSecsSinceEpoch() - it.value() < kFailureTtlMs;
}

void MediaCache::markFetched(const QString& key, bool ok) {
    if (ok)
        m_failedAt.remove(key);
    else
        m_failedAt.insert(key, QDateTime::currentMSecsSinceEpoch());
}

bool MediaCache::inFlight(const QString& md5, FileKind kind) const {
    return m_inFlight.contains(cacheKey(md5, kind));
}

bool MediaCache::subscribe(const QString& md5, FileKind kind,
                           const std::function<void(const QByteArray&)>& callback) {
    const QString key = cacheKey(md5, kind);
    if (!m_inFlight.contains(key))
        return false;  // nothing running - caller must start a fetch
    m_subscribers[key].append(callback);
    return true;
}

void MediaCache::fetchIntoCache(Source* source, const SourceImage& image, FileKind kind,
                                std::function<void()> onReady) {
    if (!source || image.md5.isEmpty()) {
        qWarning().noquote() << "MediaCache: fetch requested with no source or empty md5";
        return;
    }
    const QString key = cacheKey(image.md5, kind);
    if (m_cache.contains(key) || m_inFlight.contains(key))
        return;  // cached or already on its way
    if (recentlyFailed(key)) {
        if (onReady)
            onReady();  // nothing cached; callers check cached() and see empty
        return;
    }
    m_inFlight.insert(key);

    QPointer<MediaCache> self(this);
    const QString md5 = image.md5;
    ThreadPools::network()->start([self, source, image, kind, key, md5, onReady]() {
        // Library posts are read from disk - never re-downloaded.
        QByteArray bytes;
        LibrarySource library;
        if (!md5.isEmpty() && library.hasImage(md5)) {
            bytes = library.fetch(image.withMd5(md5), kind);
        }
        if (bytes.isEmpty())
            bytes = source->fetch(image, kind);

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, key, bytes, onReady, md5]() {
                if (!self)
                    return;
                self->m_inFlight.remove(key);
                self->markFetched(key, !bytes.isEmpty());
                if (!bytes.isEmpty()) {
                    self->store(key, bytes);
                } else {
                    qDebug().noquote() << "MediaCache: fetch returned no data for" << md5;
                }
                // Fan out to everyone who asked while the fetch was running.
                const auto subs = self->m_subscribers.take(key);
                for (const auto& sub : subs) sub(bytes);
                if (onReady)
                    onReady();
            },
            Qt::QueuedConnection);
    });
}

bool MediaCache::request(Source* source, const SourceImage& image,
                         const std::function<void(const QByteArray&)>& onPreview,
                         const std::function<void(const QByteArray&)>& onFull) {
    const QString md5 = image.md5;

    // Full media already cached: deliver instantly.
    const QByteArray full = cached(md5, FileKind::Full);
    if (!full.isEmpty()) {
        if (onFull)
            onFull(full);
        return true;
    }

    // Preview placeholder first, full fetch in background.
    QImage placeholder = cachedPreviewImage(md5);
    if (placeholder.isNull()) {
        // Subscribe to a running preview prefetch instead of duplicating it.
        if (!subscribe(md5, FileKind::Preview, onPreview))
            fetchIntoCache(source, image, FileKind::Preview, [this, md5, onPreview]() {
                const QByteArray bytes = cached(md5, FileKind::Preview);
                if (!bytes.isNull() && onPreview)
                    onPreview(bytes);
            });
    } else if (onPreview) {
        onPreview(cached(md5, FileKind::Sample).isEmpty() ? cached(md5, FileKind::Preview)
                                                          : cached(md5, FileKind::Sample));
    }

    // The real media. If a fetch is already in flight (preload race),
    // subscribe - waiting alone would hang forever.
    if (inFlight(md5, FileKind::Full)) {
        subscribe(md5, FileKind::Full, onFull);
        return false;
    }

    // Recently failed: start nothing. Callers see neither variant in flight
    // and treat the post as unavailable (FeedPage skips it).
    if (recentlyFailed(cacheKey(md5, FileKind::Full)))
        return false;

    QPointer<MediaCache> self(this);
    m_inFlight.insert(cacheKey(md5, FileKind::Full));
    ThreadPools::network()->start([self, source, image, md5, onFull]() {
        // Library first, then remote.
        QString error;
        QByteArray bytes;
        LibrarySource library;
        if (!md5.isEmpty() && library.hasImage(md5)) {
            bytes = library.fetch(image.withMd5(md5), FileKind::Full, &error);
        }
        if (bytes.isEmpty())
            bytes = source->fetch(image, FileKind::Full, &error);

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, bytes, onFull]() {
                if (!self)
                    return;
                const QString key = cacheKey(md5, FileKind::Full);
                self->m_inFlight.remove(key);
                self->markFetched(key, !bytes.isEmpty());
                if (bytes.isEmpty()) {
                    self->drop(key);
                    const auto subs = self->m_subscribers.take(key);
                    for (const auto& sub : subs) sub(bytes);
                    // The initiator hears about the failure too, or its UI
                    // stays on "loading" forever.
                    if (onFull)
                        onFull(bytes);
                    return;
                }
                self->store(key, bytes);
                const auto subs = self->m_subscribers.take(key);
                for (const auto& sub : subs) sub(bytes);
                if (onFull)
                    onFull(bytes);
            },
            Qt::QueuedConnection);
    });
    return false;
}

void MediaCache::invalidate(const QString& md5) {
    for (int i = 0; i < static_cast<int>(FileKind::Full) + 1; ++i) {
        const QString key = cacheKey(md5, static_cast<FileKind>(i));
        drop(key);
        m_inFlight.remove(key);
        m_failedAt.remove(key);
    }
}

void MediaCache::store(const QString& key, const QByteArray& bytes) {
    drop(key);
    if (bytes.size() > kMaxEntryBytes)
        return;
    m_cache.insert(key, bytes);
    m_cacheOrder.append(key);
    m_totalBytes += bytes.size();
    evict();
}

void MediaCache::drop(const QString& key) {
    const auto it = m_cache.constFind(key);
    if (it == m_cache.constEnd())
        return;
    m_totalBytes -= it->size();
    m_cache.erase(it);
    m_cacheOrder.removeAll(key);
}

void MediaCache::evict() {
    // Oldest first; the newest entry always stays.
    while (m_cacheOrder.size() > 1 &&
           (m_cacheOrder.size() > m_maxEntries || m_totalBytes > m_maxBytes)) {
        const QString key = m_cacheOrder.takeFirst();
        m_totalBytes -= m_cache.value(key).size();
        m_cache.remove(key);
    }
}

void MediaCache::evictDistant() { evict(); }

}  // namespace Mosaic
