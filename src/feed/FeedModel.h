#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <random>

namespace Mosaic {

// Learned feed model: online Bayesian probit regression (AdPredictor,
// Graepel et al. 2010) with an independent Gaussian belief per feature.

// Every feature carries a mean AND a variance. Ranking draws one plausible
// weight vector from the posterior per batch (Thompson sampling), so the
// feed exploits what it is sure about and probes what it is unsure about -
// a tag it has never seen starts at the prior variance and gets tried.
// Variance creeps back toward the prior over time, so old certainty fades
// and the model keeps re-checking instead of locking onto one path.

// Feature keys:
//   "b"        bias
//   "t:<tag>"  content tag, value (tag count)^-1/4
//   "v:<i>"    visual style dimension i (standardized)
//   "h:<name>" heuristic signal (taste similarity, session, ...)
struct ModelFeature {
    QString key;
    double value = 0.0;
};
using FeatureVector = QList<ModelFeature>;

// Heuristic signals, fixed order. Logged with every recommendation and
// replayed as model inputs when the reaction to that post arrives.
enum HeuristicSignal {
    kSignalTaste,       // best taste similarity
    kSignalSession,     // in-session interest
    kSignalTagPref,     // heuristic per-tag preference
    kSignalFavorite,    // overlap with library favorites
    kSignalFresh,       // post age
    kSignalNovelty,     // share of tags never reacted to
    kSignalPopularity,  // site score
    kSignalBoredom,     // overlap with what was just shown
    kSignalExplore,     // came from an exploration arm (heuristic only)
    kSignalCount
};

// Model inputs for one post. `visual` is the standardized descriptor
// (empty = unknown look); `signals` may be null (no ranking context, e.g.
// history replay). The explore flag is deliberately NOT a model input: a
// learned penalty on it would let the model switch exploration off.
FeatureVector buildModelFeatures(const QStringList& contentTags, const QList<double>& visual,
                                 const double* signalValues);

class FeedModel {
   public:
    struct Belief {
        double mean = 0.0;
        double variance = 0.0;  // 0 = unset, priorVariance() applies
        double positive = 0.0;  // weighted engaged evidence
        double negative = 0.0;  // weighted rejected evidence
        qint64 updatedMs = 0;
    };

    static double priorVariance(const QString& key);

    // Belief with time-based variance inflation applied.
    Belief belief(const QString& key, qint64 nowMs = 0) const;
    bool hasBelief(const QString& key) const { return m_beliefs.contains(key); }
    void setBelief(const QString& key, const Belief& b) { m_beliefs.insert(key, b); }
    const QHash<QString, Belief>& beliefs() const { return m_beliefs; }

    // Mean logit (sum of x * mean).
    double meanScore(const FeatureVector& x, qint64 nowMs = 0) const;
    // sqrt(sum x^2 var), bias excluded: how unsure the model is about x.
    double uncertainty(const FeatureVector& x, qint64 nowMs = 0) const;
    // P(engaged) under the posterior.
    double predict(const FeatureVector& x, qint64 nowMs = 0) const;

    // One assumed-density-filtering step. `weight` in (0, 1] scales the
    // update (a skip is weaker evidence than a dislike). Keys whose belief
    // changed are appended to `touched`.
    void train(const FeatureVector& x, bool engaged, double weight, QStringList* touched = nullptr,
               bool updateBias = true);

    double samples() const { return m_samples; }

    // Progressive validation: both scorers predict each reaction before
    // training on it; the running log-loss decides how far the learned
    // model is trusted.
    void recordValidation(double modelProb, double heuristicProb, bool engaged, double weight);
    double modelLogLoss() const { return m_llModel; }
    double heuristicLogLoss() const { return m_llHeuristic; }
    double validationSamples() const { return m_validationSamples; }

    // Learned-vs-heuristic blend in [0, 1]: needs evidence AND a measured
    // advantage over the heuristic. Backs off if the model starts losing.
    double blendWeight() const;

    // Platt calibration of the heuristic score into a probability, so the
    // two scorers can be compared on the same log-loss.
    double heuristicProbability(double heuristicScore) const;
    void trainHeuristicCalibration(double heuristicScore, bool engaged, double weight);

    // Running visual-feature statistics (Welford) for standardization.
    void observeVisual(const QList<float>& raw);
    QList<double> standardizeVisual(const QList<float>& raw) const;

    void reset();

    // Scalar state (counters, calibration, visual stats) - beliefs are
    // persisted row-per-key by FeedDatabase.
    QByteArray serializeState() const;
    bool deserializeState(const QByteArray& blob);

   private:
    QHash<QString, Belief> m_beliefs;
    double m_samples = 0.0;

    double m_llModel = 0.6931;  // ln 2: an uninformed coin flip
    double m_llHeuristic = 0.6931;
    double m_validationSamples = 0.0;

    double m_plattA = 4.0;
    double m_plattB = -1.0;

    QList<double> m_visMean;
    QList<double> m_visM2;
    double m_visCount = 0.0;
};

// One posterior draw for a ranking batch: each key's weight is sampled once
// and reused for every candidate, which is what makes it Thompson sampling
// rather than per-item noise. `temperature` scales the posterior spread -
// the user's exploration slider.
class ThompsonDraw {
   public:
    ThompsonDraw(const FeedModel& model, double temperature, quint64 seed);

    double weight(const QString& key);
    double score(const FeatureVector& x);

   private:
    const FeedModel& m_model;
    double m_temperature = 1.0;
    qint64 m_nowMs = 0;
    std::mt19937_64 m_rng;
    QHash<QString, double> m_weights;
};

// Discounted Beta-Bernoulli bandit over the candidate-generation arms
// (which queries to spend the network budget on). Old outcomes fade so a
// previously dull arm gets re-checked.
class ArmBandit {
   public:
    struct Arm {
        double alpha = 1.0;
        double beta = 1.0;
    };

    void observe(const QString& arm, double reward, double weight);
    // Thompson draw per arm, normalized so the mean multiplier is 1.
    QHash<QString, double> drawMultipliers(const QStringList& arms, std::mt19937_64& rng) const;
    double expectedReward(const QString& arm) const;

    const QHash<QString, Arm>& arms() const { return m_arms; }
    void setArm(const QString& name, const Arm& arm) { m_arms.insert(name, arm); }
    void reset() { m_arms.clear(); }

   private:
    QHash<QString, Arm> m_arms;
};

}  // namespace Mosaic
