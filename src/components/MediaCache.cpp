#include "MediaCache.h"

#include "../sources/local/LibrarySource.h"

#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

namespace Mosaic {

namespace {

constexpr int kPreviewMaxBytes = 24 * 1024 * 1024;  // sanity cap for decode attempts

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
    m_inFlight.insert(key);

    QPointer<MediaCache> self(this);
    const QString md5 = image.md5;
    QThreadPool::globalInstance()->start([self, source, image, kind, key, md5, onReady]() {
        // Library posts are read from disk - never re-downloaded.
        QByteArray bytes;
        LibrarySource library;
        if (!md5.isEmpty() && library.hasImage(md5)) {
            SourceImage local;
            local.md5 = md5;
            local.format = image.format;
            bytes = library.fetch(local, kind);
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
                if (!bytes.isEmpty()) {
                    if (!self->m_cache.contains(key))
                        self->m_cacheOrder.append(key);
                    self->m_cache.insert(key, bytes);
                    self->evict();
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

    QPointer<MediaCache> self(this);
    m_inFlight.insert(cacheKey(md5, FileKind::Full));
    QThreadPool::globalInstance()->start([self, source, image, md5, onFull]() {
        // Library first, then remote.
        QString error;
        QByteArray bytes;
        LibrarySource library;
        if (!md5.isEmpty() && library.hasImage(md5)) {
            SourceImage local;
            local.md5 = md5;
            local.format = image.format;
            bytes = library.fetch(local, FileKind::Full, &error);
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
                if (bytes.isEmpty()) {
                    self->m_cache.remove(key);
                    const auto subs = self->m_subscribers.take(key);
                    for (const auto& sub : subs) sub(bytes);
                    return;
                }
                if (!self->m_cache.contains(key))
                    self->m_cacheOrder.append(key);
                self->m_cache.insert(key, bytes);
                self->evict();
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
        m_cache.remove(key);
        m_inFlight.remove(key);
        m_cacheOrder.removeAll(key);
    }
}

void MediaCache::evict() {
    while (m_cacheOrder.size() > m_maxEntries) {
        const QString key = m_cacheOrder.takeFirst();
        m_cache.remove(key);
    }
}

void MediaCache::evictDistant() { evict(); }

}  // namespace Mosaic
