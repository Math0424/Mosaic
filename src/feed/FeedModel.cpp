#include "FeedModel.h"

#include <QDateTime>
#include <QTextStream>

#include <cmath>

namespace Mosaic {

namespace {

// Observation noise of the probit link (AdPredictor's beta).
constexpr double kBeta = 1.0;

// Prior spread per feature family. Tags are the main taste carriers;
// visual dimensions are many and individually weak, so they start tight.
constexpr double kPriorVarTag = 2.0;
constexpr double kPriorVarVisual = 0.03;
constexpr double kPriorVarHeuristic = 0.5;
constexpr double kPriorVarBias = 1.0;
constexpr double kMinVariance = 1e-4;

// Variance drifts halfway back to the prior in this many days without
// fresh evidence: certainty must be re-earned, so long-settled beliefs get
// probed again instead of freezing the feed.
constexpr double kVarianceHalfLifeDays = 30.0;

// Log-loss EMA rate per unit of sample weight.
constexpr double kValidationRate = 0.03;
// Blend ramp: half trust after this many live validated reactions.
constexpr double kBlendHalfSamples = 30.0;

constexpr double kPlattLearningRate = 0.05;

// Visual standardization needs a few samples before it means anything.
constexpr double kMinVisualSamples = 20.0;
constexpr double kVisualClip = 3.0;

// Arm bandit: each observation pulls the arm 1% back toward the prior.
constexpr double kArmDiscount = 0.99;

constexpr double kSqrtTwoPi = 2.5066282746310002;

double normalPdf(double t) { return std::exp(-0.5 * t * t) / kSqrtTwoPi; }

double normalCdf(double t) { return 0.5 * std::erfc(-t / std::sqrt(2.0)); }

// v(t) = pdf/cdf, with the asymptote for very negative t where the cdf
// underflows.
double truncV(double t) {
    if (t < -30.0)
        return -t;
    const double cdf = normalCdf(t);
    return cdf > 1e-300 ? normalPdf(t) / cdf : -t;
}

double logLoss(double p, bool engaged) {
    const double q = qBound(1e-4, p, 1.0 - 1e-4);
    return engaged ? -std::log(q) : -std::log(1.0 - q);
}

double sigmoid(double z) { return 1.0 / (1.0 + std::exp(-z)); }

}  // namespace

FeatureVector buildModelFeatures(const QStringList& contentTags, const QList<double>& visual,
                                 const double* signalValues) {
    static const char* kSignalKeys[kSignalCount] = {
        "h:taste", "h:session", "h:tagpref", "h:fav", "h:fresh",
        "h:novel", "h:pop",     "h:bored",   nullptr,
    };
    FeatureVector x;
    x.reserve(1 + contentTags.size() + visual.size() + kSignalCount);
    x.append({QStringLiteral("b"), 1.0});
    if (!contentTags.isEmpty()) {
        // n^-1/4: a 60-tag post does not outvote a 10-tag one, yet a single
        // tag can still move the score a lot (1/sqrt(n) diluted one hated
        // tag into irrelevance in simulation).
        const double v = std::pow(static_cast<double>(contentTags.size()), -0.25);
        for (const QString& tag : contentTags) x.append({QStringLiteral("t:") + tag, v});
    }
    for (int i = 0; i < visual.size(); ++i) x.append({QStringLiteral("v:%1").arg(i), visual.at(i)});
    if (signalValues) {
        for (int i = 0; i < kSignalCount; ++i) {
            if (kSignalKeys[i])
                x.append({QLatin1String(kSignalKeys[i]), signalValues[i]});
        }
    }
    return x;
}

double FeedModel::priorVariance(const QString& key) {
    if (key.startsWith(QLatin1String("t:")))
        return kPriorVarTag;
    if (key.startsWith(QLatin1String("v:")))
        return kPriorVarVisual;
    if (key.startsWith(QLatin1String("h:")))
        return kPriorVarHeuristic;
    return kPriorVarBias;
}

FeedModel::Belief FeedModel::belief(const QString& key, qint64 nowMs) const {
    const double prior = priorVariance(key);
    auto it = m_beliefs.constFind(key);
    if (it == m_beliefs.constEnd()) {
        Belief b;
        b.variance = prior;
        return b;
    }
    Belief b = it.value();
    if (b.variance <= 0.0)
        b.variance = prior;
    if (nowMs <= 0)
        nowMs = QDateTime::currentMSecsSinceEpoch();
    if (b.updatedMs > 0 && b.variance < prior) {
        const double days = (nowMs - b.updatedMs) / 86400000.0;
        if (days > 0.0) {
            const double regained = 1.0 - std::exp2(-days / kVarianceHalfLifeDays);
            b.variance += (prior - b.variance) * regained;
        }
    }
    return b;
}

double FeedModel::meanScore(const FeatureVector& x, qint64 nowMs) const {
    double z = 0.0;
    for (const ModelFeature& f : x) z += f.value * belief(f.key, nowMs).mean;
    return z;
}

double FeedModel::uncertainty(const FeatureVector& x, qint64 nowMs) const {
    double v = 0.0;
    for (const ModelFeature& f : x) {
        if (f.key == QLatin1String("b"))
            continue;
        v += f.value * f.value * belief(f.key, nowMs).variance;
    }
    return std::sqrt(v);
}

double FeedModel::predict(const FeatureVector& x, qint64 nowMs) const {
    double mean = 0.0, var = kBeta * kBeta;
    for (const ModelFeature& f : x) {
        const Belief b = belief(f.key, nowMs);
        mean += f.value * b.mean;
        var += f.value * f.value * b.variance;
    }
    return normalCdf(mean / std::sqrt(var));
}

void FeedModel::train(const FeatureVector& x, bool engaged, double weight, QStringList* touched,
                      bool updateBias) {
    if (weight <= 0.0 || x.isEmpty())
        return;
    weight = qMin(1.0, weight);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QList<Belief> current;
    current.reserve(x.size());
    double mean = 0.0, var = kBeta * kBeta;
    for (const ModelFeature& f : x) {
        const Belief b = belief(f.key, nowMs);
        current.append(b);
        mean += f.value * b.mean;
        var += f.value * f.value * b.variance;
    }
    const double sigma = std::sqrt(var);
    const double y = engaged ? 1.0 : -1.0;
    const double t = y * mean / sigma;
    const double v = truncV(t);
    const double w = v * (v + t);

    for (int i = 0; i < x.size(); ++i) {
        const ModelFeature& f = x.at(i);
        if (f.value == 0.0 || (!updateBias && f.key == QLatin1String("b")))
            continue;
        Belief b = current.at(i);
        const double xv = f.value * b.variance;
        b.mean += weight * y * xv / sigma * v;
        const double shrink = weight * f.value * xv / var * w;
        b.variance = qMax(kMinVariance, b.variance * (1.0 - qMin(0.95, shrink)));
        if (engaged)
            b.positive += weight;
        else
            b.negative += weight;
        b.updatedMs = nowMs;
        m_beliefs.insert(f.key, b);
        if (touched)
            touched->append(f.key);
    }
    m_samples += weight;
}

void FeedModel::recordValidation(double modelProb, double heuristicProb, bool engaged,
                                 double weight) {
    if (weight <= 0.0)
        return;
    const double rate = 1.0 - std::pow(1.0 - kValidationRate, weight);
    m_llModel += rate * (logLoss(modelProb, engaged) - m_llModel);
    m_llHeuristic += rate * (logLoss(heuristicProb, engaged) - m_llHeuristic);
    m_validationSamples += weight;
}

double FeedModel::blendWeight() const {
    if (m_validationSamples <= 0.0)
        return 0.0;
    const double ramp = m_validationSamples / (m_validationSamples + kBlendHalfSamples);
    // Positive advantage = the learned model predicts reactions better.
    // Full trust from +0.03 nats; none at -0.01 (the heuristic is winning).
    const double advantage = m_llHeuristic - m_llModel;
    const double gate = qBound(0.0, (advantage + 0.01) / 0.04, 1.0);
    return ramp * gate;
}

double FeedModel::heuristicProbability(double heuristicScore) const {
    return sigmoid(m_plattA * heuristicScore + m_plattB);
}

void FeedModel::trainHeuristicCalibration(double heuristicScore, bool engaged, double weight) {
    if (weight <= 0.0)
        return;
    const double err = heuristicProbability(heuristicScore) - (engaged ? 1.0 : 0.0);
    m_plattA -= kPlattLearningRate * weight * err * heuristicScore;
    m_plattB -= kPlattLearningRate * weight * err;
}

void FeedModel::observeVisual(const QList<float>& raw) {
    if (raw.isEmpty())
        return;
    if (m_visMean.size() != raw.size()) {
        m_visMean = QList<double>(raw.size(), 0.0);
        m_visM2 = QList<double>(raw.size(), 0.0);
        m_visCount = 0.0;
    }
    m_visCount += 1.0;
    for (int i = 0; i < raw.size(); ++i) {
        const double delta = raw.at(i) - m_visMean.at(i);
        m_visMean[i] += delta / m_visCount;
        m_visM2[i] += delta * (raw.at(i) - m_visMean.at(i));
    }
}

QList<double> FeedModel::standardizeVisual(const QList<float>& raw) const {
    QList<double> out;
    if (raw.isEmpty() || raw.size() != m_visMean.size() || m_visCount < kMinVisualSamples)
        return out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const double sd = std::sqrt(m_visM2.at(i) / (m_visCount - 1.0));
        const double z = sd > 1e-6 ? (raw.at(i) - m_visMean.at(i)) / sd : 0.0;
        out.append(qBound(-kVisualClip, z, kVisualClip));
    }
    return out;
}

void FeedModel::reset() {
    m_beliefs.clear();
    m_samples = 0.0;
    m_llModel = m_llHeuristic = 0.6931;
    m_validationSamples = 0.0;
    m_plattA = 4.0;
    m_plattB = -1.0;
    // Visual statistics describe the catalog, not the user: kept.
}

QByteArray FeedModel::serializeState() const {
    QString out;
    QTextStream s(&out);
    s.setRealNumberPrecision(10);
    s << "fm1 " << m_samples << ' ' << m_llModel << ' ' << m_llHeuristic << ' '
      << m_validationSamples << ' ' << m_plattA << ' ' << m_plattB << ' ' << m_visCount << ' '
      << m_visMean.size();
    for (double v : m_visMean) s << ' ' << v;
    for (double v : m_visM2) s << ' ' << v;
    return out.toUtf8();
}

bool FeedModel::deserializeState(const QByteArray& blob) {
    QString str = QString::fromUtf8(blob);
    QTextStream s(&str);
    QString tag;
    s >> tag;
    if (tag != QLatin1String("fm1"))
        return false;
    double samples = 0, llM = 0, llH = 0, valN = 0, a = 0, b = 0, visN = 0;
    int dims = 0;
    s >> samples >> llM >> llH >> valN >> a >> b >> visN >> dims;
    if (s.status() != QTextStream::Ok || dims < 0 || dims > 1024)
        return false;
    QList<double> mean(dims), m2(dims);
    for (int i = 0; i < dims; ++i) s >> mean[i];
    for (int i = 0; i < dims; ++i) s >> m2[i];
    if (s.status() != QTextStream::Ok)
        return false;
    m_samples = samples;
    m_llModel = llM;
    m_llHeuristic = llH;
    m_validationSamples = valN;
    m_plattA = a;
    m_plattB = b;
    m_visCount = visN;
    m_visMean = mean;
    m_visM2 = m2;
    return true;
}

// Thompson draw

ThompsonDraw::ThompsonDraw(const FeedModel& model, double temperature, quint64 seed)
    : m_model(model),
      m_temperature(temperature),
      m_nowMs(QDateTime::currentMSecsSinceEpoch()),
      m_rng(seed) {}

double ThompsonDraw::weight(const QString& key) {
    auto it = m_weights.constFind(key);
    if (it != m_weights.constEnd())
        return it.value();
    const FeedModel::Belief b = m_model.belief(key, m_nowMs);
    std::normal_distribution<double> dist(b.mean, m_temperature * std::sqrt(b.variance));
    const double w = dist(m_rng);
    m_weights.insert(key, w);
    return w;
}

double ThompsonDraw::score(const FeatureVector& x) {
    double z = 0.0;
    for (const ModelFeature& f : x) z += f.value * weight(f.key);
    return z;
}

// arm bandit

void ArmBandit::observe(const QString& arm, double reward, double weight) {
    if (arm.isEmpty() || weight <= 0.0)
        return;
    Arm a = m_arms.value(arm);
    const double keep = std::pow(kArmDiscount, weight);
    a.alpha = 1.0 + keep * (a.alpha - 1.0) + weight * qBound(0.0, reward, 1.0);
    a.beta = 1.0 + keep * (a.beta - 1.0) + weight * (1.0 - qBound(0.0, reward, 1.0));
    m_arms.insert(arm, a);
}

QHash<QString, double> ArmBandit::drawMultipliers(const QStringList& arms,
                                                  std::mt19937_64& rng) const {
    QHash<QString, double> draws;
    double sum = 0.0;
    for (const QString& name : arms) {
        const Arm a = m_arms.value(name);
        std::gamma_distribution<double> ga(a.alpha, 1.0), gb(a.beta, 1.0);
        const double x = ga(rng), y = gb(rng);
        const double theta = x + y > 0.0 ? x / (x + y) : 0.5;
        draws.insert(name, theta);
        sum += theta;
    }
    const double mean = arms.isEmpty() ? 1.0 : sum / arms.size();
    for (auto it = draws.begin(); it != draws.end(); ++it)
        it.value() = mean > 1e-9 ? qBound(0.3, it.value() / mean, 2.5) : 1.0;
    return draws;
}

double ArmBandit::expectedReward(const QString& arm) const {
    const Arm a = m_arms.value(arm);
    return a.alpha / (a.alpha + a.beta);
}

}  // namespace Mosaic
