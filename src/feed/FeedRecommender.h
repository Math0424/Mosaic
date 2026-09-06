#pragma once

#include "FeedDatabase.h"
#include "../sources/Source.h"

#include <QHash>
#include <QList>
#include <QStringList>

namespace Mosaic {

class EmbeddingBackend;
class SourceManager;

// Multi-taste recommender for the feed.

// Pipeline (never collapsed into one score):
//   1. Candidate generation (taste-tag queries, similar-to-liked, recent,
//      popular, exploration), deduped.
//   2. Hard-negative filter: images with a learned hard-negative tag are
//      removed. Unknown is never negative.
//   3. Ranking: 0.55 user_similarity + 0.20 session + 0.10 tag_preference
//      + 0.05 freshness + 0.05 novelty + 0.05 exploration.
//   4. Taste allocation by dynamic priority (strength x underexposure x
//      engagement).
//   5. MMR diversity pass.
class FeedRecommender {
   public:
    explicit FeedRecommender(FeedDatabase& db, SourceManager& sources,
                             const QStringList& feedSourceIds = {},
                             EmbeddingBackend* embeddings = nullptr);

    QList<SourceImage> nextBatch(int count = 10);

    // True for tags too generic to carry taste information.
    static bool isGenericTag(const QString& tag);

    static QStringList contentTags(const SourceImage& image);

    struct ModelStats {
        int tasteCount = 0;
        int totalShown = 0;
        int favPosts = 0;
        int hardNegatives = 0;
        QStringList hardNegativeTags;
        int skipStreak = 0;
        double sessionBlend = 0.0;
        QStringList tasteSummary;
        QList<QPair<QString, double>> likedTags;
        QList<QPair<QString, double>> dislikedTags;
        int negativeAnchors = 0;
        double totalImpressions = 0.0;
        QStringList lowSpecificityTags;
        double learnedSamples = 0.0;
        double learnedBlend = 0.0;
        QList<double> learnedWeights;
    };
    ModelStats modelStats() const;

    // Funnel diagnostics for the most recent nextBatch() call.
    struct FunnelStats {
        int stage1Candidates = 0;
        int stage1Filtered = 0;
        int dropShown = 0, dropDup = 0, dropAnimated = 0, dropScore = 0, dropHardNeg = 0,
            dropNoMd5 = 0, dropNegVisual = 0;
        int stage2Shortlist = 0;
        int stage3Finalists = 0;
        int embeddingsComputedThisBatch = 0;
        int batchSize = 0;
        int coldStart = 0;
        int emergencyFallbackUsed = 0;
    };
    FunnelStats lastFunnelStats() const { return m_lastFunnel; }

    FeedDatabase::RecommendationRow explainPost(const SourceImage& image) const;

    // How much the visual model thinks the user likes this post relative
    // to their overall taste (best positive-taste visual similarity).
    struct VisualVerdict {
        bool available = false;  // embedding backend ready + embedding exists
        double bestSim = 0.0;    // best taste visual similarity
        qint64 bestTasteId = 0;
        double negAnchorSim = 0.0;  // similarity to learned negative looks
        double learnedProb = 0.5;   // stage-2 model P(positive reaction)
    };
    VisualVerdict explainVisual(const SourceImage& image) const;

   private:
    struct Candidate {
        SourceImage image;
        QString source;
        qint64 tasteId = 0;
    };

    // Candidate generation arms.
    QList<SourceImage> fetchFromSource(const QString& sourceId, const QString& query,
                                       int limit) const;
    QList<Candidate> candidatesFromTastes(const QList<FeedDatabase::Taste>& tastes,
                                          const QStringList& sourceIds) const;
    // Broad-reach arm: queries seeded with generic liked tags (high
    // preference, low specificity) - the recall counterpart to the
    // niche-taste arm.
    QList<Candidate> candidatesBroad(const QList<FeedDatabase::Taste>& tastes,
                                     const QStringList& sourceIds) const;
    QList<Candidate> candidatesFromCombos(const QStringList& sourceIds) const;
    QList<Candidate> candidatesFromFavorites(const QStringList& sourceIds) const;
    QList<Candidate> candidatesRecent(const QStringList& sourceIds) const;
    QList<Candidate> candidatesPopular(const QStringList& sourceIds) const;
    QList<Candidate> candidatesExploration(const QList<FeedDatabase::Taste>& tastes,
                                           const QStringList& sourceIds) const;

    // Builds a set of library-favorited content tags with their frequency counts
    QHash<QString, int> favoriteTagFrequencies() const;

    // Dynamic taste priority.
    double tastePriority(const FeedDatabase::Taste& taste, int totalExposure) const;

    // Taste allocation: assigns each candidate the taste it
    // best matches, then fills the batch respecting per-taste quotas.
    QList<SourceImage> allocateAndDiversify(QList<Candidate>& candidates, int count) const;

    FeedDatabase& m_db;
    SourceManager& m_sources;
    EmbeddingBackend* m_embeddings = nullptr;  // optional
    QStringList m_feedSourceIds;
    mutable FunnelStats m_lastFunnel;
};

}  // namespace Mosaic
