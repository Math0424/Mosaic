#pragma once

#include "../sources/Source.h"

#include <QHash>

#include <atomic>

namespace Mosaic {

class FeedDatabase;
class SourceManager;
class VisualFeatures;

// Gives the learned feed model a running start instead of a blank slate.

// Three phases, each resumable across launches (progress lives in
// feed.duckdb), run once on a worker thread:
//   1. priors   - the heuristic's per-tag preferences become model priors
//   2. library  - library favorites replay as positives (local, fast; the
//                 bias is frozen so "everything is liked" is not learned)
//   3. history  - past likes / favorites / dislikes plus a sample of long
//                 views and skips replay in shuffled order. Old events have
//                 no stored tags, so each post is resolved by md5 from the
//                 library or its site, rate-limited to stay polite.
class FeedWarmStart {
   public:
    FeedWarmStart(FeedDatabase& db, SourceManager& sources, VisualFeatures& visual);

    // Blocks until finished or `cancel` is set. Worker threads only.
    void run(const std::atomic<bool>& cancel);

    // Resolves a post by md5: the library catalog first, then the site it
    // came from, then the other remote sources (at most `maxRemoteQueries`
    // site requests). Returns false when nothing knows it.
    static bool resolvePost(SourceManager& sources, const QString& md5, const QString& sourceId,
                            SourceImage* out, int maxRemoteQueries = 2);

   private:
    void runFavorites(const std::atomic<bool>& cancel);
    void runHistory(const std::atomic<bool>& cancel);
    // Sleeps until this site's lookup pacing allows the next request; false
    // when cancelled while waiting.
    bool waitForRemoteSlot(const QString& sourceId, const std::atomic<bool>& cancel);
    bool sleepCancellable(qint64 ms, const std::atomic<bool>& cancel);

    FeedDatabase& m_db;
    SourceManager& m_sources;
    VisualFeatures& m_visual;
    QHash<QString, qint64> m_lastRemoteMs;  // per source id
};

}  // namespace Mosaic
