#pragma once

#include <QByteArray>

namespace Mosaic {

// Online logistic-regression user model (stage 2 of the feed's learning).
// Trained by SGD on every interaction over the same feature vector the
// Bayesian ranker computes. blendWeight() is 0 at cold start (pure Bayesian)
// and asymptotically 1. Deliberately logistic regression: ~10 features,
// interpretable weights, trains in microseconds. ONNX stays the feature
// extractor only.

// Feature layout (fixed order, shared with the recommender):
//   0 userSim   1 sessionSim   2 tagScore   3 favOverlap   4 freshness
//   5 novelty   6 exploration  7 popularity 8 negAnchorSim 9 bias
class UserModel {
   public:
    static constexpr int kFeatureCount = 10;
    static constexpr int kBiasIndex = kFeatureCount - 1;

    double predict(const double* features) const;

    // One SGD step; `label` 1 = engaged, 0 = rejected.
    void train(const double* features, double label, double sampleWeight);

    double samples() const { return m_samples; }

    double weight(int index) const { return m_weights[index]; }

    double blendWeight() const;

    void reset();

    // Flat text serialization for DuckDB storage.
    QByteArray serialize() const;
    bool deserialize(const QByteArray& blob);

   private:
    double m_weights[kFeatureCount] = {};
    double m_samples = 0.0;
};

}  // namespace Mosaic
