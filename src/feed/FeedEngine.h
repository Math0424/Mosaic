#pragma once

#include "FeedDatabase.h"

#include "../sources/Source.h"
#include "../sources/local/LibraryCatalog.h"

#include <QFuture>
#include <QMutex>
#include <QStringList>

#include <atomic>
#include <memory>

namespace Mosaic {

class FeedRecommender;
class SourceManager;
class VisualFeatures;

// Shared feed state: the one feed.duckdb connection, the visual descriptor
// store, the recommender and the background warm start. DuckDB allows a
// single open per file, and it is the same person on every screen, so every
// feed front end (the desktop FeedPage and the web server) shares this
// object - including one recommender, whose batches are serialized.
//
// Also holds the reaction rules both front ends must agree on (what dwell
// time counts as a skip, what a save does), so the web feed trains exactly
// like the desktop one.
class FeedEngine {
   public:
    FeedEngine(SourceManager& sources, const QStringList& feedSourceIds);
    ~FeedEngine();

    FeedEngine(const FeedEngine&) = delete;
    FeedEngine& operator=(const FeedEngine&) = delete;

    FeedDatabase& db() { return *m_db; }
    VisualFeatures& visual() { return *m_visual; }
    SourceManager& sources() { return m_sources; }
    bool isOpen() const { return m_db && m_db->isOpen(); }

    // The one recommender. Stats readers (debug panel) may use it directly;
    // batches go through nextBatch() so front ends never search at once.
    FeedRecommender& recommender() { return *m_recommender; }

    // Next recommended posts. One batch at a time across all front ends (a
    // caller waits for a running batch rather than doubling the site load).
    // Blocking - worker threads only.
    QList<SourceImage> nextBatch(int count);

    // Runs the learned model's warm start in the background (no-op while
    // one runs, or once it has completed).
    void startWarmStart();
    // Cancels a running warm start and waits for it (before a reset).
    void stopWarmStart();

    // Closes the DB (CHECKPOINTs the WAL). Idempotent; called at app
    // shutdown before the pools drain so late writes fail fast.
    void shutdown();

    // A post that just left the screen after `dwellMs` of visible media:
    // records a skip or a view against the user's adaptive thresholds.
    // Animated posts that played pass their length and how long they
    // played; they need a share of the length watched (with the image
    // thresholds as minimums for short loops).
    // Blocking (DB write lock) - worker threads only.
    void recordDwell(const SourceImage& image, qint64 dwellMs, qint64 mediaDurationMs = 0,
                     qint64 playedMs = 0);

    // Explicit reaction (like / favorite / dislike) with its linger time.
    // Blocking - worker threads only.
    void recordReaction(FeedDatabase::Event event, const SourceImage& image, qint64 dwellMs);

    // Downloads the post from its remote source and imports it into the
    // library (no-op when it is already saved). Blocking - worker threads.
    ImportResult saveToLibrary(const SourceImage& image);

    static QStringList tagNames(const SourceImage& image);

   private:
    SourceManager& m_sources;
    std::unique_ptr<FeedDatabase> m_db;
    std::unique_ptr<VisualFeatures> m_visual;
    // After the DB and visual store: it borrows both.
    std::unique_ptr<FeedRecommender> m_recommender;
    QMutex m_batchMutex;
    QFuture<void> m_warmStart;
    std::atomic<bool> m_cancelWarmStart{false};
};

}  // namespace Mosaic
