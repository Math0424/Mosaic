#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <duckdb.h>

#include "FeedModel.h"

namespace Mosaic {

// Persistent storage for the recommendation feed, in its own DuckDB file
// (feed.duckdb) so the feature can be removed without touching the library.

// Tables: feed_events (raw interaction log; skips recorded separately from
// impressions - unknown is never negative), feed_tastes (multiple positive
// taste profiles, tag space), feed_tag_preferences (per-tag preference +
// confidence; the heuristic's negative model), feed_model (learned model:
// one Gaussian belief per feature), feed_arm_stats (candidate-arm bandit),
// feed_post_tags (tags of every post the user reacted to, so the model can
// be rebuilt from history), feed_visual (visual style descriptors),
// feed_recommendations (ranking log the model trains from),
// feed_linger_stats (adaptive dwell thresholds).

// Session model: an in-memory fast-EMA tag vector, decays in-session and
// never touches long-term tastes.
class FeedDatabase {
   public:
    FeedDatabase() = default;
    ~FeedDatabase();

    FeedDatabase(const FeedDatabase&) = delete;
    FeedDatabase& operator=(const FeedDatabase&) = delete;

    // Opens (and creates) feed.duckdb; idempotent. Returns false on error.
    bool open(QString* error = nullptr);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // events

    enum class Event {
        Impression,
        View,
        Skip,
        Like,
        Favorite,
        Dislike,
    };

    static QString eventName(Event e);
    static bool eventFromName(const QString& name, Event* out);

    void recordEvent(Event kind, const QString& sourceId, const QString& md5,
                     const QStringList& tags, double watchSeconds = 0.0, int position = -1,
                     qint64 remoteId = 0);

    // Shown before, matched by md5 or by post id (sourceId + remoteId), so a
    // post whose file was replaced or re-hashed still counts as seen.
    bool wasShown(const QString& md5, const QString& sourceId = {}, qint64 remoteId = 0) const;

    // visual style descriptors (see VisualFeatures)

    bool hasVisual(const QString& md5) const;
    // Raw descriptor, empty when missing or computed by an older version.
    QList<float> visualFor(const QString& md5) const;
    QHash<QString, QList<float>> visualsFor(const QStringList& md5s) const;
    // Stores the descriptor and folds it into the standardization stats.
    void storeVisual(const QString& md5, const QList<float>& raw);

    // tastes

    // One positive taste profile: a tag vector plus strength/confidence.
    // Tastes seed the candidate queries and the per-taste batch quotas.
    struct Taste {
        qint64 id = 0;
        QHash<QString, double> embedding;
        double strength = 0.0;
        double confidence = 0.0;
        double exposure = 0.0;
        double engagement = 0.0;
        qint64 lastUsedMs = 0;
        qint64 createdAtMs = 0;
    };

    QList<Taste> tastes() const;

    // Cosine similarity between a tag vector and a taste.
    static double similarity(const QHash<QString, double>& tasteEmbedding, const QStringList& tags);

    void recordTasteExposure(qint64 tasteId, double amount = 1.0);

    // Strength/exposure/engagement age-out pass; called lazily at batch time.
    void decayTastes();

    // negative preferences

    struct TagPreference {
        QString tag;
        double preference = 0.0;
        double confidence = 0.0;
        qint64 updatedAtMs = 0;
    };

    QHash<QString, double> tagPreferences() const;

    QHash<QString, TagPreference> tagPreferenceRows() const;

    // Images carrying one of these are REMOVED, not ranked down.
    QStringList hardNegativeTags() const;

    QStringList negativeQueryTags(int limit = 5) const;

    // tag specificity

    // Self-learned document frequency from impressions: replaces a hardcoded
    // generic-tag blacklist with data.
    // tag -> IDF-style specificity in [0,1]; rare/unseen tags default to 0.6.
    QHash<QString, double> tagSpecificities() const;
    double totalImpressions() const;

    // learned model

    // Copy of the learned model for a ranking pass (implicitly shared, so
    // cheap) - ranking never holds the DB lock while it scores.
    FeedModel modelSnapshot() const;
    ArmBandit armSnapshot() const;

    // How adventurous the feed is, 0 (stick to what works) .. 1 (probe a
    // lot). Persisted in feed_meta; the feed page slider drives it.
    double explorationLevel() const;
    void setExplorationLevel(double level);

    // feed_meta counters (tangent hits, warm-start progress, ...).
    double metaValue(const QString& key, double fallback = 0.0) const;
    void setMetaValue(const QString& key, double value);

    // warm start (see FeedWarmStart)

    bool warmStartDone() const;
    void setWarmStartDone(bool done);

    // Turns the heuristic's learned tag preferences into model priors so
    // the learned model starts from what the feed already knows.
    void seedModelFromTagPreferences();

    struct HistoryEvent {
        QString md5;
        QString sourceId;
        Event kind = Event::View;
        double watchSeconds = 0.0;
    };
    // Past reactions not yet replayed: every like/favorite/dislike, plus a
    // capped sample of long views and skips (the bulk of the negatives).
    QList<HistoryEvent> warmStartHistory(int maxViews, int maxSkips) const;
    void markWarmStartProcessed(const QString& md5);

    // Trains the model (and tastes, for positives) from one replayed
    // reaction. Never touches tag preferences or the event log - those
    // already contain the original event. `updateBias` false for
    // positives-only seeding, which would otherwise teach "everything is
    // liked".
    void trainFromHistory(const QString& md5, const QStringList& tags, Event kind,
                          double watchSeconds, bool updateBias = true);

    // Tags of a post the user reacted to (empty when unknown).
    QStringList postTags(const QString& md5) const;
    void storePostTags(const QString& md5, const QString& sourceId, const QStringList& tags);

    // session model

    // tag -> session weight (fast EMA), in-memory only.
    QHash<QString, double> sessionWeights() const;

    // Cosine similarity of an image against the session vector (0 when the
    // session vector is still empty). Active sway tags dominate the score.
    double sessionSimilarity(const QStringList& tags) const;

    // Content tags of the most recent impressions, newest first: the
    // "you just saw a lot of this" boredom signal.
    QList<QStringList> recentImpressionTags() const;

    // Temporary user-typed tags steering the feed right now. Session-only:
    // they bias ranking and candidate queries but never train the long-term
    // model. Empty list clears the sway.
    void setSwayTags(const QStringList& tags);
    QStringList swayTags() const;

    // linger

    // Records how long the user actually looked at a post that ended in
    // `kind` ("skipped"/"watched"/"liked"/"saved").
    void recordLinger(const QString& kind, qint64 durationMs);

    // kind -> average linger in ms (only kinds with recorded samples).
    QHash<QString, qint64> averageLingers() const;

    // kind -> number of recorded interactions ("liked" -> 12, ...).
    QHash<QString, int> interactionCounts() const;

    // stats

    // Number of posts fed so far (for "explore vs exploit" decisions).
    int totalShown() const;

    // How many of the most recent interactions were skips/dislikes in a
    // row. A long streak means the feed has gone stale for this session.
    int recentSkipStreak() const;

    // The most recent interactions, newest first ("kind", "md5").
    QList<QPair<QString, QString>> recentInteractions(int limit = 5) const;

    // Debug: every md5 the user reacted positively to (like/favorite),
    // newest first. Distinct - one entry per post. Paged so a profile with
    // thousands of likes can be walked without loading them all at once.
    // `seed` shuffles the order deterministically: the same seed always
    // yields the same sequence (so paging stays consistent), a new seed
    // gives a different walk through the same set.
    QList<QPair<QString, QString>> likedInteractions(int limit = 500, int offset = 0,
                                                     quint32 seed = 0) const;

    // Total distinct liked/favorited posts (progress reporting).
    int likedInteractionCount() const;

    // recommendation log

    // One ranked candidate's full score breakdown.
    struct RecommendationRow {
        QString md5;
        double userSimilarity = 0.0;
        double sessionSimilarity = 0.0;
        double tagScore = 0.0;
        double freshnessScore = 0.0;
        double noveltyScore = 0.0;
        double explorationScore = 0.0;
        qint64 tasteId = 0;
        double tastePriority = 0.0;
        double preDiversityScore = 0.0;
        double finalScore = 0.0;
        QString candidateSource;
        bool rejectedNegative = false;
        // "main" or "tangent" (an exploration slot).
        QString slot;
        double modelProbability = 0.5;
        double uncertainty = 0.0;
        // Heuristic score + signals the post was ranked under (see
        // packSignals). The model trains on these when the reaction arrives.
        QString features;
    };

    // Encodes the heuristic score + signal vector for the log.
    static QString packSignals(double heuristicScore, const double* signalValues);
    // Decodes it; false for rows logged by an older feature layout.
    static bool unpackSignals(const QString& packed, double* heuristicScore, double* signalValues);

    void recordRecommendations(const QList<RecommendationRow>& rows);

    // Most recent recommendation rows, newest first (debug panel).
    QList<RecommendationRow> recentRecommendations(int limit = 40) const;

    // The row the recommender logged when it last ranked this md5
    // (empty md5 when the post was never ranked).
    RecommendationRow recommendationFor(const QString& md5) const;

    // tuning

    // Forgets the learned profile (tastes, preferences, model, history).
    // With `rerunWarmStart` the next launch of the warm start re-seeds from
    // library favorites; otherwise the profile stays empty.
    void reset(bool rerunWarmStart, QString* error = nullptr);

   private:
    void ensureSchema();
    void migrateLegacy();
    // One-time repair of tag confidence poisoned by skip events.
    void repairSkipPoisonedConfidence();

    // Unlocked variants: called with the lock already held
    // (QReadWriteLock is non-recursive).
    QList<Taste> tastesUnlocked() const;
    QList<float> visualForUnlocked(duckdb_connection conn, const QString& md5) const;
    QHash<QString, double> tagSpecificitiesUnlocked(duckdb_connection conn) const;
    RecommendationRow recommendationForUnlocked(duckdb_connection conn, const QString& md5) const;
    void storePostTagsUnlocked(duckdb_connection conn, const QString& md5, const QString& sourceId,
                               const QStringList& tags);
    void bumpMetaUnlocked(duckdb_connection conn, const QString& key, double delta);

    // Taste updates (tag space). `newTasteThreshold`: below this similarity
    // to every existing taste, a new taste is created.
    void updateTastesOnPositive(const QStringList& tags, double weight, double newTasteThreshold);
    void updateTastesOnNegative(const QStringList& tags, double weight);
    void createTaste(const QStringList& tags, double weight, qint64 nowMs);
    void writeTaste(qint64 id, const QHash<QString, double>& embedding, double strength,
                    double confidence, double engagement, qint64 nowMs);
    void writeTasteStrength(qint64 id, double strength);

    // Model training shared by live events and history replay.
    void trainModelUnlocked(duckdb_connection conn, const QString& md5,
                            const QStringList& contentTags, Event kind, double watchSeconds,
                            const RecommendationRow* logged, bool updateBias);
    void loadModel();
    void saveBeliefsUnlocked(duckdb_connection conn, const QStringList& keys) const;
    void saveModelStateUnlocked(duckdb_connection conn) const;
    void saveArmUnlocked(duckdb_connection conn, const QString& arm) const;

    duckdb_database m_db = nullptr;

    // Session vector (in-memory, fast EMA)
    QHash<QString, double> m_sessionVec;
    double m_sessionMass = 0.0;
    QList<QStringList> m_recentImpressionTags;

    // Temporary taste-sway tags (in-memory, session only)
    QStringList m_swayTags;

    // Learned model + arm bandit (persisted on every update)
    FeedModel m_model;
    ArmBandit m_arms;
    int m_visualSinceSave = 0;
    double m_exploration = 0.5;
};

}  // namespace Mosaic
