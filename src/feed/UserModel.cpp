#include "UserModel.h"

#include <QString>
#include <QTextStream>

#include <cmath>

namespace Mosaic {

namespace {

// Small fixed learning rate: single-sample updates must stay stable.
constexpr double kLearningRate = 0.07;

constexpr double kL2Lambda = 1e-4;

// Half Bayesian -> learned crossover at this many samples.
constexpr double kBlendHalf = 60.0;

double sigmoid(double z) { return 1.0 / (1.0 + std::exp(-z)); }

}  // namespace

double UserModel::predict(const double* features) const {
    double z = 0.0;
    for (int i = 0; i < kFeatureCount; ++i) z += m_weights[i] * features[i];
    return sigmoid(z);
}

void UserModel::train(const double* features, double label, double sampleWeight) {
    if (sampleWeight <= 0.0)
        return;
    const double p = predict(features);
    const double err = p - label;
    for (int i = 0; i < kFeatureCount; ++i) {
        const double reg = kL2Lambda * m_weights[i];
        m_weights[i] -= kLearningRate * sampleWeight * (err * features[i] + reg);
    }
    m_samples += sampleWeight;
}

double UserModel::blendWeight() const {
    if (m_samples <= 0.0)
        return 0.0;
    // Logistic ramp; Bayesian ranker stays as a prior + exploration signal.
    return m_samples / (m_samples + kBlendHalf);
}

void UserModel::reset() {
    for (int i = 0; i < kFeatureCount; ++i) m_weights[i] = 0.0;
    m_samples = 0.0;
}

QByteArray UserModel::serialize() const {
    QString out;
    QTextStream s(&out);
    s << m_samples;
    for (int i = 0; i < kFeatureCount; ++i) s << ' ' << m_weights[i];
    return out.toUtf8();
}

bool UserModel::deserialize(const QByteArray& blob) {
    QString str = QString::fromUtf8(blob);
    QTextStream s(&str);
    double samples = 0.0;
    s >> samples;
    double w[kFeatureCount] = {};
    for (int i = 0; i < kFeatureCount; ++i) s >> w[i];
    if (s.status() != QTextStream::Ok)
        return false;
    m_samples = samples;
    for (int i = 0; i < kFeatureCount; ++i) m_weights[i] = w[i];
    return true;
}

}  // namespace Mosaic
