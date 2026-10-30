#pragma once

#include "FeedDatabase.h"
#include "FeedModel.h"
#include "../sources/Source.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QStringList>

namespace Mosaic {

class SourceManager;
class VisualFeatures;

// Learning recommender for the feed.

// Pipeline:
//   1. Candidate generation: query arms (taste, broad, combo, similar-to-
//      liked, recent, popular, exploration, tangent, sway). A discounted
//      Thompson bandit over the arms decides how much of the network budget
//      each one gets, from how often its posts were engaged with.
//   2. Hard-negative filter: images with a learned hard-negative tag are
//      removed. Unknown is never negative.
//   3. Scoring: a fixed heuristic formula (cold-start prior) and the learned
//      Bayesian model (FeedModel, Thompson-sampled), blended by how much
//      better the model has proven at predicting this user's reactions.
//   4. Tangent slots: a share of every batch (set by the exploration
//      slider) goes to plausible-but-uncertain posts - what the model would
//      learn the most from. Liked tangents are promoted into new tastes.
//   5. Taste quotas + greedy MMR diversity for the remaining slots.
class FeedRecommender {
   public:
    explicit FeedRecommender(FeedDatabase& db, SourceManager& sources,
                             const QStringList& feedSourceIds = {},
                             VisualFeatures* visual = nullptr);

    QList<SourceImage> nextBatch(int count = 10);

    // True for tags too generic to carry taste information.
    static bool isGenericTag(const QString& tag);

    static QStringList contentTags(const SourceImage& image);

    // Heuristic per-tag weight: positives saturate, negatives are shrunk by
    // their dislike evidence (one accidental dislike stays weak).
    static double tagPreferenceWeight(double preference, double confidence);

    struct ModelStats {
        // Full per-taste breakdown for the debug-panel hover tooltip.
        struct TasteDetail {
            qint64 id = 0;
            double strength = 0.0;
            double confidence = 0.0;
            double exposure = 0.0;
            double engagement = 0.0;
            // Per-tag: taste vector weight + query niche score (what the
            // candidate-generation arm actually ranks by).
            struct Tag {
                QString name;
                double weight = 0.0;
                double niche = 0.0;
            };
            QList<Tag> tags;  // ranked by niche score
        };
        using TasteDetails = QList<TasteDetail>;
        int tasteCount = 0;
        int totalShown = 0;
        int favPosts = 0;
        int hardNegatives = 0;
        QStringList hardNegativeTags;
        int skipStreak = 0;
        double sessionBlend = 0.0;
        QStringList tasteSummary;
        QList<TasteDetail> tasteDetails;
        // Every negatively-scored tag with raw preference + confidence,
        // for debugging tags that keep resurfacing.
        struct TagNeg {
            QString tag;
            double pref = 0.0;
            double conf = 0.0;
        };
        using TagNegs = QList<TagNeg>;
        TagNegs dislikedTagsFull;
        double totalImpressions = 0.0;
        QStringList lowSpecificityTags;
        QStringList swayTags;

        // learned model
        struct LearnedTag {
            QString tag;
            double mean = 0.0;
            double sd = 0.0;
            double evidence = 0.0;
        };
        QList<LearnedTag> learnedLiked;
        QList<LearnedTag> learnedDisliked;
        QList<LearnedTag> tangentCandidates;
        QList<QPair<QString, double>> signalWeights;  // learned weight per heuristic
        QList<QPair<QString, double>> armOdds;        // expected engagement per arm
        int beliefCount = 0;
        double modelSamples = 0.0;
        double validationSamples = 0.0;
        double blend = 0.0;
        double modelLogLoss = 0.0;
        double heuristicLogLoss = 0.0;
        double exploration = 0.5;
        double tangentShown = 0.0;
        double tangentHits = 0.0;
        bool warmStartDone = false;
        double warmStartProgress = 0.0;
        double warmStartTotal = 0.0;
    };
    ModelStats modelStats() const;

    // Funnel diagnostics for the most recent nextBatch() call.
    struct FunnelStats {
        int stage1Candidates = 0;
        int stage1Filtered = 0;
        // Shown/dup/animated are mechanical dedup, not rejections - not
        // counted here.
        int dropScore = 0, dropHardNeg = 0, dropNoMd5 = 0;
        // (post, reason) of funnel drops, capped - debug "dropped images"
        // view. Stage-1 rejections plus stage-2 shortlist drops.
        QList<QPair<SourceImage, QString>> dropped;
        int stage2Shortlist = 0;
        int visualsComputedThisBatch = 0;
        int tangentSlots = 0;
        int batchSize = 0;
        int coldStart = 0;
        int emergencyFallbackUsed = 0;
        double blend = 0.0;
        double exploration = 0.0;
        // Candidates found per arm this batch.
        QList<QPair<QString, int>> armCandidates;
    };
    FunnelStats lastFunnelStats() const { return m_lastFunnel; }

    // Why a post scores what it scores: the heuristic breakdown, the
    // learned model's verdict, and how it was originally ranked.
    struct Explanation {
        FeedDatabase::RecommendationRow heuristic;  // signal breakdown
        double heuristicScore = 0.0;
        double modelProbability = 0.5;
        double uncertainty = 0.0;
        double blend = 0.0;
        bool visualKnown = false;
        QString loggedSource;  // arm that found it (empty if never ranked)
        QString loggedSlot;    // "main" / "tangent"
    };
    QList<Explanation> explainPosts(const QList<SourceImage>& images) const;

   private:
    struct Candidate {
        SourceImage image;
        QString source;
        qint64 tasteId = 0;
    };

    // Everything the heuristic needs, loaded once per ranking pass.
    struct ScoringContext {
        QList<FeedDatabase::Taste> tastes;
        int totalExposure = 1;
        QHash<QString, double> prefs;
        QHash<QString, double> specificity;
        QHash<QString, int> favTagFreqs;
        QList<QStringList> recentTags;
        double sessionBlend = 0.25;
        double exploration = 0.5;
    };
    ScoringContext buildContext() const;

    // Fills `signals` (kSignalCount values) and returns the heuristic score.
    double computeSignals(const ScoringContext& ctx, const FeedModel& model,
                          const SourceImage& image, const QString& source, double* signalValues,
                          qint64* tasteId, double* tastePriority) const;

    // Candidate generation arms.
    QList<SourceImage> fetchFromSource(const QString& sourceId, const QString& query,
                                       int limit) const;
    // Random feed site for the next query, avoiding sites that rate limited
    // us this batch.
    const QString& pickSource(const QStringList& sourceIds) const;
    QList<Candidate> candidatesFromTastes(const QList<FeedDatabase::Taste>& tastes,
                                          const QStringList& sourceIds) const;
    // Broad-reach arm: queries seeded with generic liked tags (high
    // preference, low specificity) - the recall counterpart to the
    // niche-taste arm.
    QList<Candidate> candidatesBroad(const QList<FeedDatabase::Taste>& tastes,
                                     const QStringList& sourceIds) const;
    // Pairs of strongly learned tags (model beliefs).
    QList<Candidate> candidatesFromCombos(const FeedModel& model,
                                          const QStringList& sourceIds) const;
    QList<Candidate> candidatesFromFavorites(const QStringList& sourceIds) const;
    QList<Candidate> candidatesRecent(const QStringList& sourceIds) const;
    QList<Candidate> candidatesPopular(const QStringList& sourceIds) const;
    QList<Candidate> candidatesExploration(const QList<FeedDatabase::Taste>& tastes,
                                           const QStringList& sourceIds) const;
    // Tangents: tags seen on liked posts but still uncertain on their own,
    // queried alone and next to an established liked tag.
    QList<Candidate> candidatesTangent(const FeedModel& model, const QStringList& sourceIds) const;
    // User-typed sway tags: a dedicated arm so swayed queries get real
    // representation in the batch, not just a ranking nudge.
    QList<Candidate> candidatesFromSway(const QStringList& sourceIds) const;

    // Tangent tag candidates ranked by optimism (mean + k * sd).
    QList<ModelStats::LearnedTag> tangentTags(const FeedModel& model, double exploration,
                                              int limit) const;

    // Arm budget from the bandit draw (and the exploration level for the
    // exploratory arms). 0 = skip the arm this batch.
    int armLimit(const QString& arm, int base) const;

    // Builds a set of library-favorited content tags with their frequency counts
    QHash<QString, int> favoriteTagFrequencies() const;

    // Dynamic taste priority.
    double tastePriority(const FeedDatabase::Taste& taste, int totalExposure) const;

    // Scoring, tangent slots, taste quotas and MMR.
    QList<SourceImage> allocateAndDiversify(QList<Candidate>& candidates, int count,
                                            const FeedModel& model, FunnelStats* funnel);

    FeedDatabase& m_db;
    SourceManager& m_sources;
    VisualFeatures* m_visual = nullptr;  // optional
    QStringList m_feedSourceIds;
    QHash<QString, double> m_armMultipliers;  // this batch's bandit draw
    double m_exploration = 0.5;               // this batch's slider value
    QStringList m_feedSourceIdsThisBatch;
    mutable QSet<QString> m_throttled;  // sites that rate limited this batch
    mutable FunnelStats m_lastFunnel;
};

}  // namespace Mosaic
