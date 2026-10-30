#include "FeedWarmStart.h"

#include "FeedDatabase.h"
#include "VisualFeatures.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibraryCatalog.h"
#include "../sources/local/LibrarySource.h"
#include "../sources/web/HttpSource.h"

#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QThread>

#include <algorithm>

namespace Mosaic {

namespace {

// Remote md5 lookups: one request per site per this many ms. Slow on
// purpose - rule34.xxx allows about one page a second for the whole app,
// and the feed needs most of it.
constexpr qint64 kRemoteIntervalMs = 2500;
// Longest wait for a request slot before the lookup counts as throttled.
constexpr int kRemoteMaxWaitMs = 5000;
// After a rate-limited lookup: pause 30s, doubling up to 5 min.
constexpr qint64 kRateLimitCooldownMs = 30000;
constexpr qint64 kMaxRateLimitCooldownMs = 300000;
// A post that keeps getting rate limited is given up after this many tries.
constexpr int kMaxRateLimitedAttempts = 3;

constexpr int kMaxFavorites = 500;
constexpr int kMaxLongViews = 400;
// Skips are capped relative to the explicit positives so replayed
// negatives neither drown nor vanish.
constexpr int kMinSkips = 300;
constexpr int kMaxSkips = 1500;

QStringList tagNames(const SourceImage& img) {
    QStringList tags;
    for (const SourceTag& t : img.tags) {
        if (t.category != QLatin1String("usergroup"))
            tags << t.name;
    }
    return tags;
}

}  // namespace

FeedWarmStart::FeedWarmStart(FeedDatabase& db, SourceManager& sources, VisualFeatures& visual)
    : m_db(db), m_sources(sources), m_visual(visual) {}

void FeedWarmStart::run(const std::atomic<bool>& cancel) {
    if (!m_db.isOpen() || m_db.warmStartDone())
        return;

    qInfo().noquote() << "FeedWarmStart: starting";
    if (m_db.metaValue(QStringLiteral("warmstart_priors")) < 0.5) {
        m_db.seedModelFromTagPreferences();
        m_db.setMetaValue(QStringLiteral("warmstart_priors"), 1.0);
    }
    if (cancel)
        return;
    if (m_db.metaValue(QStringLiteral("warmstart_favorites")) < 0.5) {
        runFavorites(cancel);
        if (cancel)
            return;
        m_db.setMetaValue(QStringLiteral("warmstart_favorites"), 1.0);
    }
    runHistory(cancel);
    if (cancel || !m_db.isOpen())
        return;
    m_db.setWarmStartDone(true);
    qInfo().noquote() << "FeedWarmStart: done";
}

void FeedWarmStart::runFavorites(const std::atomic<bool>& cancel) {
    LibrarySource library;
    SourceQuery favQuery;
    favQuery.tags = QStringLiteral("usergroup:favorite");
    favQuery.limit = kMaxFavorites;
    const SourceResult favs = library.search(favQuery);
    if (!favs.ok)
        return;

    int trained = 0;
    for (const SourceImage& img : favs.images) {
        if (cancel || !m_db.isOpen())
            return;
        if (img.md5.isEmpty())
            continue;
        // Local thumbnail: the look of a favorite is free to learn.
        m_visual.ensure(img);
        m_db.trainFromHistory(img.md5, tagNames(img), FeedDatabase::Event::Favorite, 0.0,
                              /*updateBias=*/false);
        m_db.markWarmStartProcessed(img.md5);
        ++trained;
    }
    qInfo().noquote() << "FeedWarmStart: replayed" << trained << "library favorites";
}

void FeedWarmStart::runHistory(const std::atomic<bool>& cancel) {
    QList<FeedDatabase::HistoryEvent> events = m_db.warmStartHistory(kMaxLongViews, kMaxSkips);

    int positives = 0;
    QList<FeedDatabase::HistoryEvent> skips;
    QList<FeedDatabase::HistoryEvent> rest;
    for (const FeedDatabase::HistoryEvent& e : events) {
        if (e.kind == FeedDatabase::Event::Skip) {
            skips.append(e);
            continue;
        }
        if (e.kind != FeedDatabase::Event::Dislike)
            ++positives;
        rest.append(e);
    }
    // Skips come back already randomly ordered; keep the first N.
    const int skipBudget = qBound(kMinSkips, 2 * positives, kMaxSkips);
    rest += skips.mid(0, skipBudget);
    // Online learning is order-sensitive: interleave positives and
    // negatives instead of replaying them in blocks.
    std::shuffle(rest.begin(), rest.end(), *QRandomGenerator::global());

    const double already = m_db.metaValue(QStringLiteral("warmstart_progress"));
    const double total = already + rest.size();
    m_db.setMetaValue(QStringLiteral("warmstart_total"), total);
    qInfo().noquote() << "FeedWarmStart: replaying" << rest.size() << "history events ("
                      << positives << "positives )";

    // Background work must never compete with the feed for a site's quota:
    // no retry ladder, and a rate-limited lookup is re-queued for later
    // instead of being thrown away.
    HttpSource::ImpatientScope impatient(kRemoteMaxWaitMs);
    LibraryCatalog catalog;
    QHash<QString, int> attempts;
    qint64 cooldownMs = kRateLimitCooldownMs;
    int done = 0, resolved = 0;
    for (int i = 0; i < rest.size(); ++i) {
        if (cancel || !m_db.isOpen())
            return;
        const FeedDatabase::HistoryEvent e = rest.at(i);

        QStringList tags = m_db.postTags(e.md5);
        SourceImage img;
        bool local = false;
        if (tags.isEmpty()) {
            if (catalog.lookupByMd5(e.md5, &img)) {
                local = true;
            } else {
                if (!waitForRemoteSlot(e.sourceId, cancel))
                    return;
                const bool found = resolvePost(m_sources, e.md5, e.sourceId, &img, 1);
                m_lastRemoteMs.insert(e.sourceId, QDateTime::currentMSecsSinceEpoch());
                if (!found && HttpSource::lastRequestRateLimited()) {
                    if (++attempts[e.md5] <= kMaxRateLimitedAttempts)
                        rest.append(e);  // try again once the site calms down
                    qInfo().noquote()
                        << "FeedWarmStart: rate limited, pausing" << cooldownMs / 1000 << "s";
                    if (!sleepCancellable(cooldownMs, cancel))
                        return;
                    cooldownMs = qMin(cooldownMs * 2, kMaxRateLimitCooldownMs);
                    if (attempts.value(e.md5) <= kMaxRateLimitedAttempts)
                        continue;
                } else {
                    cooldownMs = kRateLimitCooldownMs;
                }
                if (!found)
                    img = SourceImage{};
            }
            tags = tagNames(img);
        }

        if (!tags.isEmpty()) {
            if (local)
                m_visual.ensure(img);
            m_db.trainFromHistory(e.md5, tags, e.kind, e.watchSeconds);
            ++resolved;
        }
        // Unresolvable posts are marked too, so a resume never retries them.
        m_db.markWarmStartProcessed(e.md5);
        if (++done % 10 == 0)
            m_db.setMetaValue(QStringLiteral("warmstart_progress"), already + done);
    }
    m_db.setMetaValue(QStringLiteral("warmstart_progress"), already + done);
    qInfo().noquote() << "FeedWarmStart: history replayed |" << resolved << "of" << done
                      << "resolved";
}

bool FeedWarmStart::sleepCancellable(qint64 ms, const std::atomic<bool>& cancel) {
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + ms;
    while (!cancel) {
        const qint64 wait = until - QDateTime::currentMSecsSinceEpoch();
        if (wait <= 0)
            return true;
        QThread::msleep(static_cast<unsigned long>(qMin<qint64>(wait, 50)));
    }
    return false;
}

bool FeedWarmStart::waitForRemoteSlot(const QString& sourceId, const std::atomic<bool>& cancel) {
    while (!cancel) {
        const qint64 wait = m_lastRemoteMs.value(sourceId) + kRemoteIntervalMs -
                            QDateTime::currentMSecsSinceEpoch();
        if (wait <= 0)
            return true;
        QThread::msleep(static_cast<unsigned long>(qMin<qint64>(wait, 50)));
    }
    return false;
}

bool FeedWarmStart::resolvePost(SourceManager& sources, const QString& md5, const QString& sourceId,
                                SourceImage* out, int maxRemoteQueries) {
    LibraryCatalog catalog;
    if (catalog.lookupByMd5(md5, out))
        return true;

    // The site the post came from first, then the others.
    QList<Source*> candidates;
    if (Source* s = sources.source(sourceId); s && !s->isLocal())
        candidates.append(s);
    for (Source* s : sources.remoteSources()) {
        if (!candidates.contains(s))
            candidates.append(s);
    }
    int queries = 0;
    for (Source* s : candidates) {
        if (maxRemoteQueries > 0 && queries >= maxRemoteQueries)
            break;
        ++queries;
        SourceQuery q;
        q.tags = QStringLiteral("md5:%1").arg(md5);
        q.limit = 1;
        const SourceResult result = s->search(q);
        if (!result.ok || result.images.isEmpty())
            continue;
        *out = result.images.first();
        out->md5 = md5;
        if (out->sourceId.isEmpty())
            out->sourceId = s->id();
        return true;
    }
    return false;
}

}  // namespace Mosaic
