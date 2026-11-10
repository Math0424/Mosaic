#include "FeedEngine.h"

#include "FeedRecommender.h"
#include "FeedWarmStart.h"
#include "VisualFeatures.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"

#include <QDebug>
#include <QMutexLocker>
#include <QtConcurrentRun>

namespace Mosaic {

namespace {

constexpr qint64 kSkipMs = 1500;
constexpr qint64 kFullWatchMs = 8000;
// A minute-long "watch" is absence, not engagement.
constexpr qint64 kAbandonMs = 60000;

// Clamp band for the adaptive skip threshold.
constexpr qint64 kSkipFloorMs = 1000;
constexpr qint64 kSkipCeilingMs = 4000;
// The skip cutoff never falls below this fraction of the user's average
// watch time - that's what lets the threshold rise with their pace.
constexpr double kSkipShareOfWatchMs = 0.35;

}  // namespace

FeedEngine::FeedEngine(SourceManager& sources, const QStringList& feedSourceIds)
    : m_sources(sources) {
    m_db = std::make_unique<FeedDatabase>();
    QString dbError;
    if (!m_db->open(&dbError))
        qWarning().noquote() << "FeedEngine: feed database failed to open, feed disabled:"
                             << dbError;
    m_visual = std::make_unique<VisualFeatures>(*m_db, m_sources);
    m_recommender =
        std::make_unique<FeedRecommender>(*m_db, m_sources, feedSourceIds, m_visual.get());
}

QList<SourceImage> FeedEngine::nextBatch(int count) {
    QMutexLocker lock(&m_batchMutex);
    return m_recommender->nextBatch(count);
}

FeedEngine::~FeedEngine() {
    shutdown();
    // The warm start holds raw pointers to the members deleted below.
    m_warmStart.waitForFinished();
}

void FeedEngine::startWarmStart() {
    if (m_warmStart.isRunning() || !isOpen())
        return;
    m_cancelWarmStart = false;
    FeedDatabase* db = m_db.get();
    SourceManager* sources = &m_sources;
    VisualFeatures* visual = m_visual.get();
    std::atomic<bool>* cancel = &m_cancelWarmStart;
    m_warmStart = QtConcurrent::run(
        [db, sources, visual, cancel]() { FeedWarmStart(*db, *sources, *visual).run(*cancel); });
}

void FeedEngine::stopWarmStart() {
    m_cancelWarmStart = true;
    m_warmStart.waitForFinished();
}

void FeedEngine::shutdown() {
    m_cancelWarmStart = true;
    if (m_db)
        m_db->close();
}

QStringList FeedEngine::tagNames(const SourceImage& image) {
    QStringList tags;
    tags.reserve(image.tags.size());
    for (const SourceTag& t : image.tags) tags << t.name;
    return tags;
}

void FeedEngine::recordDwell(const SourceImage& image, qint64 dwellMs) {
    if (dwellMs <= 0 || dwellMs >= kAbandonMs)
        return;
    // Adaptive thresholds from this user's linger history.
    const QHash<QString, qint64> lingers = m_db->averageLingers();
    const qint64 avgWatchedRaw = lingers.value(QStringLiteral("watched"), kFullWatchMs);
    const qint64 skippedAvg = lingers.value(QStringLiteral("skipped"), kSkipMs);
    const qint64 paceFloor = static_cast<qint64>(kSkipShareOfWatchMs * avgWatchedRaw);
    const qint64 skipMs = qBound(kSkipFloorMs, qMax(skippedAvg, paceFloor), kSkipCeilingMs);

    const QStringList tags = tagNames(image);
    const double dwellSecs = dwellMs / 1000.0;
    if (dwellMs < skipMs) {
        // A skip is a weak negative, not a dislike.
        m_db->recordEvent(FeedDatabase::Event::Skip, image.sourceId, image.md5, tags, dwellSecs);
        m_db->recordLinger(QStringLiteral("skipped"), dwellMs);
    } else {
        m_db->recordEvent(FeedDatabase::Event::View, image.sourceId, image.md5, tags, dwellSecs);
        m_db->recordLinger(QStringLiteral("watched"), dwellMs);
    }
}

void FeedEngine::recordReaction(FeedDatabase::Event event, const SourceImage& image,
                                qint64 dwellMs) {
    // A dislike carries no watch credit: the dwell before it is not interest.
    const double watchSecs = event == FeedDatabase::Event::Dislike ? 0.0 : dwellMs / 1000.0;
    m_db->recordEvent(event, image.sourceId, image.md5, tagNames(image), watchSecs, -1);
    if (dwellMs > 0 && dwellMs < kAbandonMs) {
        m_db->recordLinger(event == FeedDatabase::Event::Dislike ? QStringLiteral("disliked")
                                                                 : QStringLiteral("liked"),
                           dwellMs);
    }
}

ImportResult FeedEngine::saveToLibrary(const SourceImage& image) {
    // Download from the post's REMOTE source - LibrarySource only reads
    // files already in the library, so fetching through it would fail.
    QString sourceId = image.sourceId;
    if (sourceId.isEmpty()) {
        const QList<Source*> remotes = m_sources.remoteSources();
        if (!remotes.isEmpty())
            sourceId = remotes.first()->id();
    }
    Source* downloadSource = m_sources.source(sourceId);

    LibrarySource library;
    SourceImage post = image;
    post.sourceId = sourceId;

    ImportResult result;
    if (library.hasImage(post)) {
        // Already saved (same md5, or same source + post id): skip the download.
        result.ok = true;
        result.alreadyExisted = true;
        return result;
    }
    if (!downloadSource) {
        result.error = QStringLiteral("source not available");
        return result;
    }
    QString error;
    const QByteArray bytes = downloadSource->fetch(image, FileKind::Full, &error);
    if (bytes.isEmpty()) {
        result.error = error;
        return result;
    }
    return library.importImage(post, bytes);
}

}  // namespace Mosaic
