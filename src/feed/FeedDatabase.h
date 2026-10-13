#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <duckdb.h>

#include "UserModel.h"

namespace Mosaic {

// Persistent storage for the recommendation feed, in its own DuckDB file
// (feed.duckdb) so the feature can be removed without touching the library.

// Tables: feed_events (raw interaction log; skips recorded separately from
// impressions - unknown is never negative), feed_tastes (multiple positive
// taste profiles), feed_tag_preferences (per-tag preference + confidence;
// the negative model), feed_recommendations (debug ranking log),
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

    void recordEvent(Event kind, const QString& sourceId, const QString& md5,
                     const QStringList& tags, double watchSeconds = 0.0, int position = -1);

    bool wasShown(const QString& md5) const;

    // image embeddings

    // Returns an embedding produced by a different model version as missing
    // (never silently mix incompatible models).
    QByteArray embeddingFor(const QString& md5, const QString& model = {},
                            const QString& modelVersion = {}) const;
    // Stores/updates a packed float32 embedding for an image.
    void storeImageEmbedding(const QString& md5, const QByteArray& packed, const QString& model,
                             const QString& modelVersion);

    // tastes

    // One positive taste profile: packed float32 vector in the visual
    // embedding space (main), tag vector (fallback), plus strength/confidence.
    struct Taste {
        qint64 id = 0;
        QByteArray visual;
        QHash<QString, double> embedding;
        double strength = 0.0;
        double confidence = 0.0;
        double exposure = 0.0;
        double engagement = 0.0;
        qint64 lastUsedMs = 0;
        qint64 createdAtMs = 0;
    };

    QList<Taste> tastes() const;

    // Cosine similarity between a tag vector and a taste (fallback).
    static double similarity(const QHash<QString, double>& tasteEmbedding, const QStringList& tags);

    // MAIN similarity: packed visual vectors.
    static double similarityVisual(const QByteArray& tasteVisual, const QByteArray& imageVisual);

    // Pull the most similar taste toward the image vector, or create a taste.
    void updateTastesOnPositiveVisual(const QByteArray& imageVisual, double weight);

    void updateTastesOnNegativeVisual(const QByteArray& imageVisual, double weight);

    // Tag-space fallbacks while the embedding backend is unavailable.
    void updateTastesOnPositive(const QStringList& tags, double weight);
    void updateTastesOnNegative(const QStringList& tags, double weight);

    void recordTasteExposure(qint64 tasteId, double amount = 1.0);

    void recordTasteEngagement(qint64 tasteId, double weight);

    // Strength/exposure/engagement age-out pass; called lazily at batch time.
    void decayTastes();

    // negative visual anchors

    // "Avoid this look" embeddings built from dislikes that fit no positive
    // taste (dislike was about looks, not subject).
    struct NegativeAnchor {
        qint64 id = 0;
        QByteArray visual;
        double strength = 0.0;
        qint64 lastUsedMs = 0;
    };
    QList<NegativeAnchor> negativeAnchors() const;
    void updateNegativeAnchors(const QByteArray& imageVisual, double weight);
    // Max visual similarity against all anchors (0 when none).
    double maxNegativeAnchorSimilarity(const QByteArray& imageVisual) const;

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
    void recordTagDocFrequency(const QStringList& tags);
    // tag -> IDF-style specificity in [0,1]; rare/unseen tags default to 0.6.
    QHash<QString, double> tagSpecificities() const;
    double totalImpressions() const;

    // learned user model

    // Stage-2 online logistic model (see UserModel.h), persisted in
    // feed_user_model, saved after every training step.
    void loadUserModel();
    void trainUserModel(const double* features, double label, double sampleWeight);
    double userModelPredict(const double* features) const;
    double userModelBlend() const;
    double userModelSamples() const;
    QList<double> userModelWeights() const;

    // session model

    // tag -> session weight (fast EMA), in-memory only.
    QHash<QString, double> sessionWeights() const;

    // Cosine similarity of an image against the session vector (0 when the
    // session vector is still empty). Active sway tags dominate the score.
    double sessionSimilarity(const QStringList& tags) const;

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
    // newest first. Distinct - one entry per post.
    QList<QPair<QString, QString>> likedInteractions(int limit = 500) const;

    // recommendation debug

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
        // Packed stage-2 model features (UserModel::kFeatureCount values,
        // space-separated, bias included). Empty when the learned model
        // is not in play. Used to train the user model when the user's
        // reaction to this post arrives.
        QString features;
    };

    // Persists one batch of recommendation rows (debug table).
    void recordRecommendations(const QList<RecommendationRow>& rows);

    // Most recent recommendation rows, newest first (debug panel).
    QList<RecommendationRow> recentRecommendations(int limit = 40) const;

    // The feature vector the recommender logged when it last ranked this
    // md5 (empty when the post was never ranked). This is what the stage-2
    // user model trains on: reaction arrives -> look up the features the
    // candidate was ranked under -> SGD step.
    QString rankedFeaturesFor(const QString& md5) const;

    // tuning

    // Forgets everything (tastes, preferences, history). Used by a reset.
    void reset(QString* error = nullptr);

    void ensureSchema();
    // One-time repair of tag confidence poisoned by skip events.
    void repairSkipPoisonedConfidence();

    // Unlocked variants: called from recordEvent() with the write lock
    // already held (QReadWriteLock is non-recursive).
    QList<Taste> tastesUnlocked() const;
    QByteArray embeddingForUnlocked(const QString& md5, const QString& model = {},
                                    const QString& modelVersion = {}) const;
    QHash<QString, double> tagSpecificitiesUnlocked(duckdb_connection conn) const;
    bool contentTagsTooFew(const QStringList& tags) const;
    void createTaste(const QStringList& tags, double weight, qint64 nowMs);
    void createTasteVisual(const QByteArray& imageVisual, double weight, qint64 nowMs);
    void writeTaste(qint64 id, const QHash<QString, double>& embedding, double strength,
                    double confidence, double engagement, qint64 nowMs);
    void writeTasteVisual(qint64 id, const QByteArray& visual, double strength, double confidence,
                          double engagement, qint64 nowMs);
    void writeTasteStrength(qint64 id, double strength);
    void saveUserModelUnlocked() const;
    QString rankedFeaturesForUnlocked(duckdb_connection conn, const QString& md5) const;

    duckdb_database m_db = nullptr;

    // Session vector (in-memory, fast EMA)
    QHash<QString, double> m_sessionVec;
    double m_sessionMass = 0.0;

    // Temporary taste-sway tags (in-memory, session only)
    QStringList m_swayTags;

    // Stage-2 learned user model (persisted on train)
    UserModel m_userModel;
};

}  // namespace Mosaic
