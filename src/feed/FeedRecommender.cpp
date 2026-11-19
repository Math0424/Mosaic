#include "FeedRecommender.h"
#include "FeedDatabase.h"
#include "VisualFeatures.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"
#include "../sources/web/HttpSource.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFuture>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentMap>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>

// Dedicated logging category; enable per-batch traces with
// QT_LOGGING_RULES="mosaic.feed.debug=true".
Q_LOGGING_CATEGORY(lcFeed, "mosaic.feed")

namespace Mosaic {

namespace {

// Heuristic ranking coefficients (the cold-start prior).
constexpr double kUserSimWeight = 0.35;
constexpr double kSessionSimWeight = 0.2;
constexpr double kTagPrefWeight = 0.10;
constexpr double kFavOverlapWeight = 0.15;
constexpr double kFreshnessWeight = 0.05;
constexpr double kNoveltyWeight = 0.04;
constexpr double kExplorationWeight = 0.03;
constexpr double kPopularityWeight = 0.03;
constexpr double kBoredomWeight = 0.15;

// Per-batch diversity guard: no taste may claim more than this share.
constexpr double kMaxShareOfBatchPerTaste = 0.5;

// Long-term vs session blend baseline, shifting toward session on skips.
constexpr double kBaseSessionBlend = 0.25;
constexpr double kMaxSessionBlend = 0.6;

double sessionBlendFor(int skipStreak) {
    if (skipStreak <= 0)
        return kBaseSessionBlend;
    return qMin(kMaxSessionBlend, kBaseSessionBlend + 0.05 * skipStreak);
}

// MMR lambda: relevance dominates, redundancy penalized.
constexpr double kMmrLambda = 0.75;

// Exploration slider mapping (level in [0, 1]):
// Extra push for the upper half of the slider: exactly 0 up to 50%, then
// rising quadratically to 1 at 100%. The bottom half behaves as the plain
// linear mappings below; only the adventurous end goes further.
double topBoost(double level) {
    const double t = qMax(0.0, (level - 0.5) / 0.5);
    return t * t;
}
// Thompson temperature - how far ranking strays from the posterior mean.
double thompsonTemperature(double level) { return 0.3 + 1.7 * level + 1.0 * topBoost(level); }
// Share of each batch reserved for tangent slots (capped at half a batch).
double tangentShare(double level) { return 0.05 + 0.30 * level + 0.15 * topBoost(level); }
// Optimism bonus per unit of uncertainty when picking tangents.
double tangentOptimism(double level) { return 0.5 + 1.5 * level + 1.0 * topBoost(level); }
// Minimum predicted engagement for a tangent: plausible, not random.
double tangentFloor(double level) { return 0.30 - 0.20 * level - 0.05 * topBoost(level); }
// Budget multiplier for the exploratory candidate arms.
double exploratoryArmScale(double level) { return 0.4 + 1.2 * level + 0.8 * topBoost(level); }
// Optimism (mean + k * sd) when ranking tangent tags to query.
double tangentTagOptimism(double level) { return 0.3 + 0.9 * level + 0.6 * topBoost(level); }
// Weight on the heuristic's novelty / exploration terms.
double curiosityFor(double level) { return 0.5 + level + 0.5 * topBoost(level); }
// "Throw it at the wall": random posts with no link to any taste, added to
// the exploration arm only in the upper half of the slider (0 below 50%).
// Independent of the bandit's arm budget so the slider always delivers it.
constexpr int kWildcardPostsAtMax = 16;
int wildcardPosts(double level) {
    const int n = static_cast<int>(std::lround(kWildcardPostsAtMax * topBoost(level)));
    return n >= 4 ? n : 0;  // below a handful a query is not worth the request
}

// Quality floor: minimum site score.
constexpr int kMinPostScore = 0;

// Candidate funnel sizes.
constexpr int kFunnelTasteCount = 150;
// Exploratory candidates that survive the shortlist regardless of score.
constexpr int kProtectedExploratory = 24;
// Max thumbnails analyzed per batch - keeps the first post appearing fast.
constexpr int kMaxVisualsPerBatch = 24;

// Recent impressions compared for the boredom signal.
constexpr int kBoredomWindow = 12;

const QStringList& armNames() {
    static const QStringList arms = {
        QStringLiteral("taste"),   QStringLiteral("taste-strict"),     QStringLiteral("broad"),
        QStringLiteral("combo"),   QStringLiteral("similar-to-liked"), QStringLiteral("recent"),
        QStringLiteral("popular"), QStringLiteral("exploration"),      QStringLiteral("tangent"),
    };
    return arms;
}

bool isExploratorySource(const QString& source) {
    return source == QLatin1String("exploration") || source == QLatin1String("tangent");
}

// Animated content is excluded from the feed unless the setting allows it.
bool isAnimated(const SourceImage& img) {
    static const QSet<QString> animatedFormats = {
        QStringLiteral("gif"), QStringLiteral("webm"), QStringLiteral("mp4"),
        QStringLiteral("mov"), QStringLiteral("mkv"),  QStringLiteral("apng"),
        QStringLiteral("swf"), QStringLiteral("webp"),
    };
    if (animatedFormats.contains(img.format.toLower()))
        return true;
    for (const SourceTag& t : img.tags) {
        const QString n = t.name.toLower();
        if (n == QLatin1String("animated") || n == QLatin1String("animation") ||
            n == QLatin1String("video") || n == QLatin1String("gif"))
            return true;
    }
    return false;
}

// Appended to every candidate query. Plain "score:>2" - some sources
// mishandle ">=" inside compound queries. "-animated" is added on top while
// animated posts are excluded.
const QString kMinScoreFilter = QStringLiteral("score:>2");
const QString kNoAnimatedFilter = QStringLiteral(" -animated");

// Tags with no taste information (quality, format, site mechanics).
const QSet<QString>& genericTagSet() {
    static const QSet<QString> tags = {
        // image quality / format / meta
        QStringLiteral("hi_res"),
        QStringLiteral("high_res"),
        QStringLiteral("low_res"),
        QStringLiteral("absurd_res"),
        QStringLiteral("highres"),
        QStringLiteral("4k"),
        QStringLiteral("8k"),
        QStringLiteral("hd"),
        QStringLiteral("jpeg_artifacts"),
        QStringLiteral("compression_artifacts"),
        QStringLiteral("comic"),
        QStringLiteral("animated"),
        QStringLiteral("gif"),
        QStringLiteral("png"),
        QStringLiteral("webm"),
        QStringLiteral("mp4"),
        QStringLiteral("video"),
        QStringLiteral("image"),
        QStringLiteral("photo"),
        QStringLiteral("widescreen"),
        QStringLiteral("16:9"),
        QStringLiteral("landscape"),
        QStringLiteral("portrait"),
        QStringLiteral("wallpaper"),
        QStringLiteral("scan"),
        QStringLiteral("official_art"),
        QStringLiteral("fanart"),
        QStringLiteral("alternate_version"),
        QStringLiteral("edit"),
        QStringLiteral("redraw"),
        // site mechanics
        QStringLiteral("commentary"),
        QStringLiteral("translated"),
        QStringLiteral("tagme"),
        QStringLiteral("unknown_artist"),
    };
    return tags;
}

// Jaccard overlap of two content-tag sets.
double jaccard(const QSet<QString>& a, const QSet<QString>& b) {
    if (a.isEmpty() || b.isEmpty())
        return 0.0;
    const QSet<QString>& small = a.size() <= b.size() ? a : b;
    const QSet<QString>& large = a.size() <= b.size() ? b : a;
    int inter = 0;
    for (const QString& t : small) {
        if (large.contains(t))
            ++inter;
    }
    const int unionSize = a.size() + b.size() - inter;
    return unionSize > 0 ? static_cast<double>(inter) / unionSize : 0.0;
}

QSet<QString> toSet(const QStringList& list) { return QSet<QString>(list.begin(), list.end()); }

// Positive tag prefs are tanh-compressed; negatives are shrunk by evidence.
constexpr double kTagPrefCeiling = 8.0;

// Bayesian shrinkage prior for negative tag preferences.
constexpr double kTagPrefPrior = 3.0;

// Query-tag selection: prefer NICHE tags. A tag seen on few impressions
// (high specificity) identifies content much more precisely than a
// near-universal one, so query seeds are ranked by
// preference x specificity instead of raw preference alone.
double queryNicheScore(const QString& tag, double preference,
                       const QHash<QString, double>& specificity) {
    auto it = specificity.constFind(tag);
    // Unseen tags default to 0.6 (same neutral value the ranker uses).
    const double spec = it != specificity.constEnd() ? it.value() : 0.6;
    return preference * (0.35 + 0.65 * spec);
}

// The opposite strategy: BROAD tags. High preference x low specificity
// (near-universal tags) cast a wide net - they pull in lots of content
// the taste model can then filter, and keep the feed from collapsing
// into one narrow niche.
double queryBroadScore(const QString& tag, double preference,
                       const QHash<QString, double>& specificity) {
    auto it = specificity.constFind(tag);
    const double spec = it != specificity.constEnd() ? it.value() : 0.6;
    return preference * (1.35 - spec);
}

// Who made it / who is in it says more about taste than descriptive tags.
double categoryBoost(const QString& category) {
    if (category == QLatin1String("artist"))
        return 2.0;
    if (category == QLatin1String("character"))
        return 1.6;
    if (category == QLatin1String("copyright"))
        return 1.4;
    if (category == QLatin1String("species"))
        return 1.2;
    return 1.0;  // general / meta / unknown
}

// Mean / standard deviation, for blending scores on a common scale.
void meanStd(const QList<double>& values, double* mean, double* sd) {
    *mean = 0.0;
    *sd = 1.0;
    if (values.isEmpty())
        return;
    double sum = 0.0;
    for (double v : values) sum += v;
    *mean = sum / values.size();
    double var = 0.0;
    for (double v : values) var += (v - *mean) * (v - *mean);
    var /= values.size();
    *sd = var > 1e-12 ? std::sqrt(var) : 1.0;
}

}  // namespace

FeedRecommender::FeedRecommender(FeedDatabase& db, SourceManager& sources,
                                 const QStringList& feedSourceIds, VisualFeatures* visual)
    : m_db(db), m_sources(sources), m_visual(visual), m_feedSourceIds(feedSourceIds) {}

bool FeedRecommender::isGenericTag(const QString& tag) {
    const QString t = tag.trimmed().toLower();
    if (t.isEmpty())
        return true;
    if (genericTagSet().contains(t))
        return true;
    // Meta-ish categories never describe content taste.
    if (t == QLatin1String("meta") || t == QLatin1String("rating") ||
        t == QLatin1String("uploader") || t == QLatin1String("status"))
        return true;
    // Pure count tags like "5girls", "6+boys": digits + suffix pattern.
    static const QRegularExpression countTag(QStringLiteral("^\\d+\\+?(girls?|boys?|others?)$"));
    return countTag.match(t).hasMatch();
}

QStringList FeedRecommender::contentTags(const SourceImage& image) {
    QStringList tags;
    for (const SourceTag& t : image.tags) {
        if (!isGenericTag(t.name))
            tags << t.name.trimmed().toLower();
    }
    tags.removeDuplicates();
    return tags;
}

double FeedRecommender::tagPreferenceWeight(double preference, double confidence) {
    // Confidence only accumulates from dislikes, so it gates negatives:
    // shrinking positives by it would zero every liked tag.
    if (preference >= 0.0)
        return kTagPrefCeiling * std::tanh(preference / kTagPrefCeiling);
    return preference * (confidence / (confidence + kTagPrefPrior));
}

// candidate arms

QList<SourceImage> FeedRecommender::fetchFromSource(const QString& sourceId, const QString& query,
                                                    int limit) const {
    Source* source = m_sources.source(sourceId);
    if (!source || !source->isEnabled() || limit <= 0)
        return {};

    SourceQuery q;
    const QString trimmed = query.trimmed();

    // Top negative tags become exclusion terms.
    QString negExclusions;
    const QStringList negTags = m_db.negativeQueryTags(4);
    for (const QString& nt : negTags) {
        if (!nt.isEmpty() && !trimmed.contains(nt)) {
            negExclusions += QStringLiteral(" -") + nt;
        }
    }

    if (sourceId == QLatin1String("r34") || sourceId == QLatin1String("r34xxx")) {
        // Both rule34 sites use "sort:*" instead of "order:*". rule34.us only
        // understands "sort:random" and no "score:>N" filter; rule34.xxx
        // supports the full sort set plus score filters.
        QString r34Query = trimmed;
        if (sourceId == QLatin1String("r34")) {
            r34Query.replace(QRegularExpression(QStringLiteral("\\border:random\\b")),
                             QStringLiteral("sort:random"));
            r34Query.remove(QRegularExpression(QStringLiteral("\\border:[a-z_]+\\b")));
            r34Query.remove(QRegularExpression(QStringLiteral("\\bscore:>?=?-?\\d+\\b")));
        } else {
            r34Query.replace(QRegularExpression(QStringLiteral("\\border:")),
                             QStringLiteral("sort:"));
        }
        r34Query = r34Query.simplified();

        QString fullTags = r34Query;
        if (!m_allowAnimated.load())
            fullTags += kNoAnimatedFilter;
        if (!negExclusions.isEmpty())
            fullTags += negExclusions;
        q.tags = fullTags.trimmed();
    } else {
        QString filter = kMinScoreFilter;
        if (!m_allowAnimated.load())
            filter += kNoAnimatedFilter;
        if (!negExclusions.isEmpty())
            filter += negExclusions;

        if (trimmed.contains(QLatin1String("order:")))
            q.tags = trimmed.isEmpty() ? filter : trimmed + QLatin1Char(' ') + filter;
        else
            q.tags = QStringLiteral("order:random ") +
                     (trimmed.isEmpty() ? filter : trimmed + QLatin1Char(' ') + filter);
    }
    q.limit = limit;
    q.page = 1;
    const SourceResult result = source->search(q);
    if (result.ok)
        return result.images;

    // A throttled site is skipped for the rest of the batch and the query
    // goes to another feed site instead of coming back empty.
    if (!source->isLocal() && HttpSource::lastRequestRateLimited()) {
        m_throttled.insert(sourceId);
        qCInfo(lcFeed).noquote() << sourceId << "is rate limiting - skipping it this batch";
        const QString fallback = pickSource(m_feedSourceIdsThisBatch);
        if (!fallback.isEmpty() && !m_throttled.contains(fallback))
            return fetchFromSource(fallback, query, limit);
    }
    return {};
}

const QString& FeedRecommender::pickSource(const QStringList& sourceIds) const {
    QStringList usable;
    for (const QString& id : sourceIds)
        if (!m_throttled.contains(id))
            usable << id;
    // Everything throttled: any pick fails fast anyway.
    const QStringList& pool = usable.isEmpty() ? sourceIds : usable;
    static const QString kNone;
    if (pool.isEmpty())
        return kNone;
    const QString& picked = pool.at(QRandomGenerator::global()->bounded(pool.size()));
    // Return a reference into the caller's list, which outlives the call.
    return sourceIds.at(sourceIds.indexOf(picked));
}

int FeedRecommender::armLimit(const QString& arm, int base) const {
    double m = m_armMultipliers.value(arm, 1.0);
    // The slider scales the exploratory arms on top of the bandit's view.
    if (isExploratorySource(arm))
        m *= exploratoryArmScale(m_exploration);
    const int limit = static_cast<int>(std::lround(base * m));
    // Below a handful of results a query is not worth the request.
    return limit >= 4 ? limit : 0;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromTastes(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (tastes.isEmpty() || sourceIds.isEmpty())
        return out;

    const int broadLimit = armLimit(QStringLiteral("taste"), 30);
    const int strictLimit = armLimit(QStringLiteral("taste-strict"), 20);
    if (broadLimit == 0 && strictLimit == 0)
        return out;
    // The bandit's opinion of the taste arm also sets how many tastes get
    // their own query this batch.
    const int tasteQueries = qBound(
        1, static_cast<int>(std::lround(5 * m_armMultipliers.value(QStringLiteral("taste"), 1.0))),
        5);

    const QHash<QString, double> specificity = m_db.tagSpecificities();
    int totalExposure = 0;
    for (const FeedDatabase::Taste& t : tastes) totalExposure += static_cast<int>(t.exposure);
    totalExposure = qMax(1, totalExposure);
    // Rotate by dynamic priority (strength x underexposure x engagement)
    // with jitter, so rarely-shown tastes get queried instead of the same
    // top-strength few every batch.
    QList<QPair<double, int>> ranked;
    for (int ti = 0; ti < tastes.size(); ++ti) {
        const double jitter = 0.75 + QRandomGenerator::global()->bounded(0.5);
        ranked.append({tastePriority(tastes.at(ti), totalExposure) * jitter, ti});
    }
    std::sort(
        ranked.begin(), ranked.end(),
        [](const QPair<double, int>& a, const QPair<double, int>& b) { return a.first > b.first; });
    for (int rank = 0; rank < qMin(tasteQueries, ranked.size()); ++rank) {
        const FeedDatabase::Taste& taste = tastes.at(ranked.at(rank).second);
        QList<QPair<double, QString>> tagsRanked;
        for (auto it = taste.embedding.constBegin(); it != taste.embedding.constEnd(); ++it)
            tagsRanked.append({queryNicheScore(it.key(), it.value(), specificity), it.key()});
        std::sort(tagsRanked.begin(), tagsRanked.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first > b.first;
                  });

        if (tagsRanked.isEmpty())
            continue;

        // Two query shapes per taste: the broad one (1 hard tag + optional
        // OR-group) and a stricter one (2 hard tags) when the taste has
        // enough strong tags. The stricter shape finds the specific
        // intersection the broad one dilutes.
        const QString anchor = tagsRanked.at(0).second;
        QString optional;
        if (tagsRanked.size() >= 2) {
            QStringList opts;
            for (int i = 1; i < qMin(5, tagsRanked.size()); ++i)
                opts << QLatin1Char('~') + tagsRanked.at(i).second;
            optional = QStringLiteral(" (%1)").arg(opts.join(QLatin1Char(' ')));
        }
        if (tagsRanked.size() >= 2 && strictLimit > 0) {
            const QString strict = anchor + QLatin1Char(' ') + tagsRanked.at(1).second + optional;
            const QString& strictSrc = pickSource(sourceIds);
            for (const SourceImage& img : fetchFromSource(strictSrc, strict, strictLimit))
                out.append({img, QStringLiteral("taste-strict"), taste.id});
        }

        if (broadLimit > 0) {
            const QString& src = pickSource(sourceIds);
            for (const SourceImage& img : fetchFromSource(src, anchor + optional, broadLimit))
                out.append({img, QStringLiteral("taste"), taste.id});
        }
    }
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromCombos(
    const FeedModel& model, const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("combo"), 20);
    if (sourceIds.isEmpty() || limit == 0)
        return out;

    // Pairs of tags the model is confidently positive about, ranked by
    // learned weight x specificity.
    const QHash<QString, double> specificity = m_db.tagSpecificities();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<QPair<double, QString>> strong;
    for (auto it = model.beliefs().constBegin(); it != model.beliefs().constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("t:")))
            continue;
        const FeedModel::Belief b = model.belief(it.key(), nowMs);
        if (b.mean < 0.3 || b.positive < 2.0)
            continue;
        const QString tag = it.key().mid(2);
        if (!isGenericTag(tag))
            strong.append({queryNicheScore(tag, b.mean, specificity), tag});
    }
    if (strong.size() < 2)
        return out;
    std::sort(strong.begin(), strong.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                  return a.first > b.first;
              });

    // Random pick among the top 8 so queries vary between batches.
    const int pool = qMin(8, strong.size());
    const int a = QRandomGenerator::global()->bounded(pool);
    int b = QRandomGenerator::global()->bounded(pool);
    if (b == a)
        b = (b + 1) % pool;
    const QString query = strong.at(a).second + QLatin1Char(' ') + strong.at(b).second;
    const QString& src = pickSource(sourceIds);
    for (const SourceImage& img : fetchFromSource(src, query, limit))
        out.append({img, QStringLiteral("combo"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesBroad(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("broad"), 30);
    if (tastes.isEmpty() || sourceIds.isEmpty() || limit == 0)
        return out;

    // Broad-reach arm: seed queries with the most GENERIC liked tags
    // (high preference, low specificity). Complements the niche arm -
    // together they alternate between precision and recall.
    // Same priority rotation as the niche arm so no taste is starved.
    const QHash<QString, double> specificity = m_db.tagSpecificities();
    QList<QPair<double, int>> tasteRank;
    int totalExposure = 0;
    for (const FeedDatabase::Taste& t : tastes) totalExposure += static_cast<int>(t.exposure);
    totalExposure = qMax(1, totalExposure);
    for (int ti = 0; ti < tastes.size(); ++ti) {
        const double jitter = 0.75 + QRandomGenerator::global()->bounded(0.5);
        tasteRank.append({tastePriority(tastes.at(ti), totalExposure) * jitter, ti});
    }
    std::sort(
        tasteRank.begin(), tasteRank.end(),
        [](const QPair<double, int>& a, const QPair<double, int>& b) { return a.first > b.first; });
    for (int rank = 0; rank < qMin(3, tasteRank.size()); ++rank) {
        const FeedDatabase::Taste& taste = tastes.at(tasteRank.at(rank).second);
        QList<QPair<double, QString>> ranked;
        for (auto it = taste.embedding.constBegin(); it != taste.embedding.constEnd(); ++it)
            ranked.append({queryBroadScore(it.key(), it.value(), specificity), it.key()});
        std::sort(ranked.begin(), ranked.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first > b.first;
                  });
        if (ranked.isEmpty())
            continue;

        QString query;
        if (ranked.size() >= 2)
            query = ranked.at(0).second + QLatin1Char(' ') + ranked.at(1).second;
        else
            query = ranked.at(0).second;

        const QString& src = pickSource(sourceIds);
        for (const SourceImage& img : fetchFromSource(src, query, limit))
            out.append({img, QStringLiteral("broad"), taste.id});
    }
    return out;
}

QHash<QString, int> FeedRecommender::favoriteTagFrequencies() const {
    QHash<QString, int> freqs;
    LibrarySource library;
    SourceQuery favQuery;
    favQuery.tags = QStringLiteral("usergroup:favorite");
    favQuery.limit = 200;
    const SourceResult favs = library.search(favQuery);
    if (!favs.ok)
        return freqs;

    for (const SourceImage& img : favs.images) {
        for (const SourceTag& t : img.tags) {
            if (isGenericTag(t.name) || t.category == QLatin1String("usergroup"))
                continue;
            const QString tag = t.name.trimmed().toLower();
            if (!tag.isEmpty())
                freqs[tag] += 1;
        }
    }
    return freqs;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromFavorites(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("similar-to-liked"), 25);
    if (sourceIds.isEmpty() || limit == 0)
        return out;

    // Query the tags of a random library favorite on the remote sources.
    LibrarySource library;
    SourceQuery favQuery;
    favQuery.tags = QStringLiteral("usergroup:favorite");
    favQuery.limit = 50;
    const SourceResult favs = library.search(favQuery);
    if (!favs.ok || favs.images.isEmpty())
        return out;

    const SourceImage& seed =
        favs.images.at(QRandomGenerator::global()->bounded(favs.images.size()));
    QStringList seedTags;
    for (const SourceTag& t : seed.tags) {
        if (!isGenericTag(t.name) && t.category != QLatin1String("usergroup"))
            seedTags << t.name.toLower();
    }
    if (seedTags.isEmpty())
        return out;

    // AND two random seed tags for a high-precision query.
    std::shuffle(seedTags.begin(), seedTags.end(), *QRandomGenerator::global());
    const QString query = seedTags.mid(0, 2).join(QLatin1Char(' '));
    const QString& src = pickSource(sourceIds);
    for (const SourceImage& img : fetchFromSource(src, query, limit))
        out.append({img, QStringLiteral("similar-to-liked"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesRecent(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("recent"), 15);
    if (sourceIds.isEmpty() || limit == 0)
        return out;
    const QString& src = pickSource(sourceIds);
    for (const SourceImage& img : fetchFromSource(src, QStringLiteral("order:score"), limit))
        out.append({img, QStringLiteral("recent"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesPopular(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("popular"), 15);
    if (sourceIds.isEmpty() || limit == 0)
        return out;
    const QString& src = pickSource(sourceIds);
    for (const SourceImage& img : fetchFromSource(src, QStringLiteral("order:hot"), limit))
        out.append({img, QStringLiteral("popular"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesExploration(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (sourceIds.isEmpty())
        return out;

    const int wild = wildcardPosts(m_exploration);
    if (wild > 0) {
        const QString& src = pickSource(sourceIds);
        for (const SourceImage& img : fetchFromSource(src, QStringLiteral("order:random"), wild))
            out.append({img, QStringLiteral("exploration"), 0});
    }

    const int limit = armLimit(QStringLiteral("exploration"), 15);
    if (limit == 0)
        return out;

    // The regular arm must not be random garbage: wildcard-expand a
    // tag from a known taste prefix to find nearby-but-unfamiliar content.
    // The seed tag is the most NICHE one (preference x specificity), since
    // a specific prefix expands into a far tighter neighborhood.
    if (!tastes.isEmpty()) {
        const FeedDatabase::Taste& taste =
            tastes.at(QRandomGenerator::global()->bounded(qMin(3, tastes.size())));
        const QHash<QString, double> specificity = m_db.tagSpecificities();
        QList<QPair<double, QString>> ranked;
        for (auto it = taste.embedding.constBegin(); it != taste.embedding.constEnd(); ++it)
            ranked.append({queryNicheScore(it.key(), it.value(), specificity), it.key()});
        std::sort(ranked.begin(), ranked.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first > b.first;
                  });
        if (!ranked.isEmpty()) {
            const QString& tag = ranked.first().second;
            const int underscore = tag.lastIndexOf(QLatin1Char('_'));
            if (underscore > 0) {
                const QString& src = pickSource(sourceIds);
                const QList<SourceImage> imgs =
                    fetchFromSource(src, tag.left(underscore) + QStringLiteral("_*"), limit);
                for (const SourceImage& img : imgs)
                    out.append({img, QStringLiteral("exploration"), 0});
                return out;
            }
        }
    }

    // Fallback: a small amount of pure random.
    const QString& src = pickSource(sourceIds);
    for (const SourceImage& img :
         fetchFromSource(src, QStringLiteral("order:random"), qMax(4, limit * 2 / 3)))
        out.append({img, QStringLiteral("exploration"), 0});
    return out;
}

QList<FeedRecommender::ModelStats::LearnedTag> FeedRecommender::tangentTags(const FeedModel& model,
                                                                            double exploration,
                                                                            int limit) const {
    // A tangent tag looks promising but nothing pursues it yet: it rode
    // along on posts the user engaged with (positive learned weight) and
    // is not already a query anchor of a taste. Optimism (mean + k * sd)
    // ranks them - the classic way to go looking where the payoff is
    // uncertain. Ranking by uncertainty alone would chase the hundreds of
    // tags that appeared on a single liked post by chance.
    const QSet<QString> hardNegs = [this]() {
        const QStringList list = m_db.hardNegativeTags();
        return QSet<QString>(list.begin(), list.end());
    }();
    // Tags the taste arms already query (each taste's top tags).
    QSet<QString> anchored;
    for (const FeedDatabase::Taste& t : m_db.tastes()) {
        QList<QPair<double, QString>> top;
        for (auto it = t.embedding.constBegin(); it != t.embedding.constEnd(); ++it)
            top.append({it.value(), it.key()});
        std::sort(top.begin(), top.end(), std::greater<QPair<double, QString>>());
        for (int i = 0; i < qMin(5, top.size()); ++i) anchored.insert(top.at(i).second);
    }
    const QStringList sway = m_db.swayTags();
    const double k = tangentTagOptimism(exploration);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<QPair<double, ModelStats::LearnedTag>> ranked;
    for (auto it = model.beliefs().constBegin(); it != model.beliefs().constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("t:")))
            continue;
        const FeedModel::Belief b = model.belief(it.key(), nowMs);
        const double evidence = b.positive + b.negative;
        // Well-established tags (lots of evidence) are not tangents either.
        // Normally a tangent must have ridden along on liked posts; at the
        // adventurous end, tags the model is unsure or mildly negative
        // about qualify too (optimism then ranks the uncertain ones up).
        const double boost = topBoost(exploration);
        if (evidence >= 25.0 || b.mean <= -0.15 * boost || (boost <= 0.0 && b.positive <= 0.0))
            continue;
        const QString tag = it.key().mid(2);
        if (isGenericTag(tag) || hardNegs.contains(tag) || sway.contains(tag) ||
            anchored.contains(tag))
            continue;
        const double sd = std::sqrt(b.variance);
        ranked.append({b.mean + k * sd, {tag, b.mean, sd, evidence}});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    QList<ModelStats::LearnedTag> out;
    for (int i = 0; i < qMin(limit, ranked.size()); ++i) out.append(ranked.at(i).second);
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesTangent(
    const FeedModel& model, const QStringList& sourceIds) const {
    QList<Candidate> out;
    const int limit = armLimit(QStringLiteral("tangent"), 20);
    if (sourceIds.isEmpty() || limit == 0)
        return out;

    const QList<ModelStats::LearnedTag> pool = tangentTags(model, m_exploration, 10);
    if (pool.isEmpty())
        return out;

    // Established anchor: a tag the model is confidently positive about.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<QPair<double, QString>> anchors;
    for (auto it = model.beliefs().constBegin(); it != model.beliefs().constEnd(); ++it) {
        if (!it.key().startsWith(QLatin1String("t:")))
            continue;
        const FeedModel::Belief b = model.belief(it.key(), nowMs);
        if (b.positive >= 3.0 && b.mean > 0.2 && !isGenericTag(it.key().mid(2)))
            anchors.append({b.mean, it.key().mid(2)});
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                  return a.first > b.first;
              });

    // Two probes, biased toward the most optimistic tags: one tangent on
    // its own (step sideways), one next to a known love (step out from
    // home ground).
    auto pickTangent = [&pool]() {
        const int top = qMin(pool.size(), 5);
        const int a = QRandomGenerator::global()->bounded(top);
        const int b = QRandomGenerator::global()->bounded(top);
        return pool.at(qMin(a, b)).tag;
    };
    const QString solo = pickTangent();
    QString paired = pickTangent();
    if (paired == solo && pool.size() > 1)
        paired = pool.at(pool.first().tag == solo ? 1 : 0).tag;

    QStringList queries{solo};
    if (!anchors.isEmpty()) {
        const QString anchor =
            anchors.at(QRandomGenerator::global()->bounded(qMin(5, anchors.size()))).second;
        if (anchor != paired)
            queries << anchor + QLatin1Char(' ') + paired;
    } else if (paired != solo) {
        queries << paired;
    }
    for (const QString& query : queries) {
        const QString& src = pickSource(sourceIds);
        for (const SourceImage& img : fetchFromSource(src, query, limit))
            out.append({img, QStringLiteral("tangent"), 0});
    }
    qCDebug(lcFeed).noquote() << "tangent queries:" << queries.join(QStringLiteral(" | "));
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromSway(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    const QStringList sway = m_db.swayTags();
    if (sway.isEmpty() || sourceIds.isEmpty())
        return out;

    // Two queries: the full sway ANDed, and a looser one with just the
    // strongest tag, so the arm still returns something when the exact
    // combination is rare on the source.
    const QString full = sway.join(QLatin1Char(' '));
    const QString& src = pickSource(sourceIds);
    QList<SourceImage> imgs = fetchFromSource(src, full, 20);
    if (imgs.isEmpty() && sway.size() > 1)
        imgs = fetchFromSource(src, sway.first(), 20);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("sway"), 0});
    return out;
}

// taste priority

double FeedRecommender::tastePriority(const FeedDatabase::Taste& taste, int totalExposure) const {
    const double strength = taste.strength * (taste.confidence / (taste.confidence + 3.0));
    double underexposure = 1.0;
    if (totalExposure > 0) {
        const double share = taste.exposure / static_cast<double>(totalExposure);
        underexposure = 1.0 / (0.25 + share);
    }
    const double engagement = 0.5 + qBound(0.0, taste.engagement, 2.0) * 0.5;
    return strength * underexposure * engagement;
}

// scoring

FeedRecommender::ScoringContext FeedRecommender::buildContext() const {
    ScoringContext ctx;
    ctx.tastes = m_db.tastes();
    int total = 0;
    for (const FeedDatabase::Taste& t : ctx.tastes) total += static_cast<int>(t.exposure);
    ctx.totalExposure = qMax(1, total);
    const auto prefRows = m_db.tagPreferenceRows();
    ctx.prefs.reserve(prefRows.size());
    for (auto it = prefRows.constBegin(); it != prefRows.constEnd(); ++it)
        ctx.prefs.insert(it.key(),
                         tagPreferenceWeight(it.value().preference, it.value().confidence));
    ctx.specificity = m_db.tagSpecificities();
    ctx.favTagFreqs = favoriteTagFrequencies();
    ctx.recentTags = m_db.recentImpressionTags();
    ctx.sessionBlend = sessionBlendFor(m_db.recentSkipStreak());
    ctx.exploration = m_db.explorationLevel();
    return ctx;
}

double FeedRecommender::computeSignals(const ScoringContext& ctx, const FeedModel& model,
                                       const SourceImage& image, const QString& source,
                                       double* signalValues, qint64* tasteId,
                                       double* tastePriorityOut) const {
    const QStringList tags = contentTags(image);
    for (int i = 0; i < kSignalCount; ++i) signalValues[i] = 0.0;

    // Taste similarity: max over tastes of similarity x strength - the
    // strongest relevant taste drives the score, interests never dilute
    // each other by averaging.
    double bestSim = 0.0;
    qint64 bestTaste = 0;
    double bestPriority = 0.0;
    for (const FeedDatabase::Taste& t : ctx.tastes) {
        const double sim = FeedDatabase::similarity(t.embedding, tags);
        const double weighted = sim * (0.5 + qMin(1.0, t.strength) * 0.5);
        if (weighted > bestSim) {
            bestSim = weighted;
            bestTaste = t.id;
            bestPriority = tastePriority(t, ctx.totalExposure);
        }
    }
    signalValues[kSignalTaste] = bestSim;
    if (tasteId)
        *tasteId = bestTaste;
    if (tastePriorityOut)
        *tastePriorityOut = bestPriority;

    signalValues[kSignalSession] = m_db.sessionSimilarity(tags);

    // Tag preference + library-favorite overlap, weighted by tag category
    // x self-learned specificity.
    double tagSum = 0.0, weightSum = 0.0, favMatches = 0.0;
    for (const SourceTag& t : image.tags) {
        if (isGenericTag(t.name))
            continue;
        const QString tag = t.name.trimmed().toLower();
        auto sit = ctx.specificity.constFind(tag);
        const double boost =
            categoryBoost(t.category) * (sit != ctx.specificity.constEnd() ? sit.value() : 0.6);
        auto it = ctx.prefs.constFind(tag);
        if (it != ctx.prefs.constEnd())
            tagSum += it.value() * boost;
        weightSum += boost;
        auto fit = ctx.favTagFreqs.constFind(tag);
        if (fit != ctx.favTagFreqs.constEnd())
            favMatches += std::log1p(static_cast<double>(fit.value())) * boost;
    }
    signalValues[kSignalTagPref] = weightSum > 0.0 ? tagSum / weightSum : 0.0;
    signalValues[kSignalFavorite] =
        weightSum > 0.0 ? qMin(1.0, favMatches / (weightSum * 1.5)) : 0.0;

    if (image.createdAt > 0) {
        const double ageDays = (QDateTime::currentSecsSinceEpoch() - image.createdAt) / 86400.0;
        signalValues[kSignalFresh] = std::exp(-ageDays / 30.0);
    }

    // Novelty: share of tags the model has never seen a reaction to.
    int unknown = 0;
    for (const QString& tag : tags) {
        const FeedModel::Belief b = model.belief(QStringLiteral("t:") + tag);
        if (b.positive + b.negative < 0.5)
            ++unknown;
    }
    signalValues[kSignalNovelty] =
        tags.isEmpty() ? 0.0 : static_cast<double>(unknown) / tags.size();

    signalValues[kSignalPopularity] =
        image.score > 0 ? std::log1p(static_cast<double>(image.score)) / 10.0 : 0.0;

    // Boredom: mean of the three closest matches among what was just shown.
    const QSet<QString> tagSet = toSet(tags);
    QList<double> overlaps;
    for (int i = 0; i < qMin(kBoredomWindow, ctx.recentTags.size()); ++i)
        overlaps.append(jaccard(tagSet, toSet(ctx.recentTags.at(i))));
    std::sort(overlaps.begin(), overlaps.end(), std::greater<double>());
    double bored = 0.0;
    for (int i = 0; i < qMin(3, overlaps.size()); ++i) bored += overlaps.at(i);
    signalValues[kSignalBoredom] = overlaps.isEmpty() ? 0.0 : bored / qMin(3, overlaps.size());

    signalValues[kSignalExplore] = isExploratorySource(source) ? 1.0 : 0.0;

    // The heuristic score itself. The slider leans its curiosity terms.
    const double curiosity = curiosityFor(ctx.exploration);
    const double blendedTaste = (1.0 - ctx.sessionBlend) * signalValues[kSignalTaste] +
                                ctx.sessionBlend * signalValues[kSignalSession];
    return kUserSimWeight * blendedTaste + kSessionSimWeight * signalValues[kSignalSession] +
           kTagPrefWeight * signalValues[kSignalTagPref] +
           kFavOverlapWeight * signalValues[kSignalFavorite] +
           kFreshnessWeight * signalValues[kSignalFresh] +
           kNoveltyWeight * curiosity * signalValues[kSignalNovelty] +
           kExplorationWeight * curiosity * signalValues[kSignalExplore] +
           kPopularityWeight * signalValues[kSignalPopularity] -
           kBoredomWeight * signalValues[kSignalBoredom];
}

// allocation + MMR

QList<SourceImage> FeedRecommender::allocateAndDiversify(QList<Candidate>& candidates, int count,
                                                         const FeedModel& model,
                                                         FunnelStats* funnel) {
    QList<SourceImage> batch;
    if (candidates.isEmpty() || count <= 0)
        return batch;

    const ScoringContext ctx = buildContext();
    const double level = m_exploration;
    const double blend = model.blendWeight();

    QStringList md5s;
    for (const Candidate& c : candidates) md5s << c.image.md5;
    const QHash<QString, QList<float>> rawVisuals = m_db.visualsFor(md5s);

    struct Scored {
        Candidate candidate;
        QStringList tags;
        QSet<QString> tagSet;
        QList<double> visual;
        double signalValues[kSignalCount] = {};
        double heuristic = 0.0;
        double meanLogit = 0.0;
        double uncertainty = 0.0;
        double probability = 0.5;
        double sampled = 0.0;
        double relevance = 0.0;
        qint64 tasteId = 0;
        double tastePriority = 0.0;
        bool tangent = false;
    };

    // One posterior draw for the whole batch (Thompson sampling).
    ThompsonDraw draw(model, thompsonTemperature(level), QRandomGenerator::global()->generate64());
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QList<Scored> scored;
    scored.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        Scored s;
        s.candidate = c;
        s.tags = contentTags(c.image);
        s.tagSet = toSet(s.tags);
        s.visual = model.standardizeVisual(rawVisuals.value(c.image.md5));
        s.heuristic = computeSignals(ctx, model, c.image, c.source, s.signalValues, &s.tasteId,
                                     &s.tastePriority);
        const FeatureVector x = buildModelFeatures(s.tags, s.visual, s.signalValues);
        s.meanLogit = model.meanScore(x, nowMs);
        s.uncertainty = model.uncertainty(x, nowMs);
        s.probability = model.predict(x, nowMs);
        s.sampled = draw.score(x);
        scored.append(s);
    }

    // Blend on a common scale: z-scores within this candidate set. The
    // learned share is exactly as large as the model has earned.
    {
        QList<double> h, m;
        for (const Scored& s : scored) {
            h << s.heuristic;
            m << s.sampled;
        }
        double hMean, hSd, mMean, mSd;
        meanStd(h, &hMean, &hSd);
        meanStd(m, &mMean, &mSd);
        for (Scored& s : scored)
            s.relevance =
                (1.0 - blend) * (s.heuristic - hMean) / hSd + blend * (s.sampled - mMean) / mSd;
    }

    auto similarity = [](const Scored& a, const Scored& b) {
        const double tag = jaccard(a.tagSet, b.tagSet);
        const double look = qMax(0.0, VisualFeatures::similarity(a.visual, b.visual));
        return qMax(tag, 0.6 * look);
    };

    // tangent slots
    // What would the model learn the most from that the user might well
    // enjoy? Optimistic score (mean + k * uncertainty) among plausible
    // candidates; posts the exploratory arms found deliberately get extra
    // credit. Two tangents never cover the same ground.
    const int tangentTarget =
        level <= 0.0 ? 0
                     : qBound(1, static_cast<int>(std::lround(count * tangentShare(level))),
                              qMax(1, count / 2));
    const double optimism = tangentOptimism(level);
    const double floor = tangentFloor(level);
    QList<Scored> tangents;
    {
        QList<QPair<double, int>> ranked;
        for (int i = 0; i < scored.size(); ++i) {
            const Scored& s = scored.at(i);
            if (s.probability < floor)
                continue;
            // Posts the exploratory arms found deliberately get extra
            // credit, much more at the adventurous end of the slider.
            const double bonus =
                isExploratorySource(s.candidate.source) ? 0.5 + 1.5 * topBoost(level) : 0.0;
            ranked.append({s.meanLogit + optimism * (1.0 + bonus) * s.uncertainty, i});
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const QPair<double, int>& a, const QPair<double, int>& b) {
                      return a.first > b.first;
                  });
        QSet<int> taken;
        for (const auto& [score, index] : ranked) {
            if (tangents.size() >= tangentTarget)
                break;
            bool redundant = false;
            for (const Scored& t : tangents) {
                if (similarity(t, scored.at(index)) > 0.5) {
                    redundant = true;
                    break;
                }
            }
            if (redundant)
                continue;
            Scored pick = scored.at(index);
            pick.tangent = true;
            tangents.append(pick);
            taken.insert(index);
        }
        QList<Scored> rest;
        rest.reserve(scored.size());
        for (int i = 0; i < scored.size(); ++i)
            if (!taken.contains(i))
                rest.append(scored.at(i));
        scored = rest;
    }

    // taste quotas: shares from dynamic priority, plus a hard cap
    const int mainCount = count - tangents.size();
    QHash<qint64, int> quota;
    {
        QHash<qint64, double> prioritySum;
        double totalPriority = 0.0;
        for (const Scored& s : scored) {
            if (s.tasteId > 0) {
                prioritySum[s.tasteId] += s.tastePriority;
                totalPriority += s.tastePriority;
            }
        }
        if (totalPriority > 0.0) {
            for (auto it = prioritySum.constBegin(); it != prioritySum.constEnd(); ++it) {
                const int q =
                    static_cast<int>(std::ceil(it.value() / totalPriority * mainCount * 1.2));
                quota.insert(it.key(), qMax(1, q));
            }
        }
    }
    const int hardCap = qMax(2, static_cast<int>(mainCount * kMaxShareOfBatchPerTaste));

    // Greedy MMR: each pick maximizes relevance minus redundancy with what
    // is already in the batch (tangents included).
    QList<Scored> selected;
    QHash<qint64, int> used;
    QList<bool> picked(scored.size(), false);
    for (int round = 0; round < mainCount; ++round) {
        int best = -1, fallback = -1;
        double bestScore = -std::numeric_limits<double>::infinity();
        double fallbackScore = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < scored.size(); ++i) {
            if (picked.at(i))
                continue;
            const Scored& s = scored.at(i);
            double maxSim = 0.0;
            for (const Scored& sel : selected) maxSim = qMax(maxSim, similarity(s, sel));
            for (const Scored& t : tangents) maxSim = qMax(maxSim, similarity(s, t));
            const double mmr = kMmrLambda * s.relevance - (1.0 - kMmrLambda) * maxSim;
            const bool capped =
                s.tasteId > 0 &&
                (used.value(s.tasteId) >= hardCap ||
                 (quota.contains(s.tasteId) && used.value(s.tasteId) >= quota.value(s.tasteId)));
            if (!capped && mmr > bestScore) {
                bestScore = mmr;
                best = i;
            }
            if (mmr > fallbackScore) {
                fallbackScore = mmr;
                fallback = i;
            }
        }
        // Quotas left us short: backfill with the best capped candidate.
        if (best < 0)
            best = fallback;
        if (best < 0)
            break;
        picked[best] = true;
        Scored s = scored.at(best);
        s.relevance =
            bestScore == -std::numeric_limits<double>::infinity() ? fallbackScore : bestScore;
        if (s.tasteId > 0)
            used[s.tasteId] += 1;
        selected.append(s);
    }

    // Spread the tangents through the batch instead of bunching them.
    QList<Scored> ordered = selected;
    for (int i = 0; i < tangents.size(); ++i) {
        const int pos =
            qMin(ordered.size(), static_cast<int>((i + 1) * (selected.size() + tangents.size()) /
                                                  (tangents.size() + 1.0)));
        ordered.insert(pos, tangents.at(i));
    }

    // log every pick: the model trains on these rows when reactions arrive
    QList<FeedDatabase::RecommendationRow> rows;
    for (const Scored& s : ordered) {
        FeedDatabase::RecommendationRow r;
        r.md5 = s.candidate.image.md5;
        r.userSimilarity = s.signalValues[kSignalTaste];
        r.sessionSimilarity = s.signalValues[kSignalSession];
        r.tagScore = s.signalValues[kSignalTagPref];
        r.freshnessScore = s.signalValues[kSignalFresh];
        r.noveltyScore = s.signalValues[kSignalNovelty];
        r.explorationScore = s.signalValues[kSignalExplore];
        r.tasteId = s.tasteId;
        r.tastePriority = s.tastePriority;
        r.preDiversityScore = s.heuristic;
        r.finalScore = s.relevance;
        r.candidateSource = s.candidate.source;
        r.slot = s.tangent ? QStringLiteral("tangent") : QStringLiteral("main");
        r.modelProbability = s.probability;
        r.uncertainty = s.uncertainty;
        r.features = FeedDatabase::packSignals(s.heuristic, s.signalValues);
        rows.append(r);
    }
    m_db.recordRecommendations(rows);

    // Record exposure for the tastes that got slots.
    for (const Scored& s : ordered) {
        if (s.tasteId > 0)
            m_db.recordTasteExposure(s.tasteId, 1.0);
    }

    if (funnel) {
        funnel->tangentSlots = tangents.size();
        funnel->blend = blend;
    }
    for (const Scored& s : ordered) batch.append(s.candidate.image);
    return batch;
}

// nextBatch

QList<SourceImage> FeedRecommender::nextBatch(int count) {
    QList<SourceImage> batch;
    if (!m_db.isOpen())
        return batch;

    m_db.decayTastes();

    QStringList sourceIds;
    if (!m_feedSourceIds.isEmpty()) {
        for (const QString& id : m_feedSourceIds) {
            Source* s = m_sources.source(id);
            // The local library is a valid feed source when the user picks
            // it in settings; only skip sources that are missing/disabled.
            if (s && s->isEnabled())
                sourceIds << id;
        }
    }
    if (sourceIds.isEmpty()) {
        for (Source* s : m_sources.sources()) {
            if (s->isEnabled())
                sourceIds << s->id();
        }
    }
    if (sourceIds.isEmpty())
        return batch;

    // The feed has several sites to draw from, so it never waits out a rate
    // limit: a throttled site is skipped for this batch (browsing tabs keep
    // the patient retry behavior).
    HttpSource::ImpatientScope impatient;
    m_feedSourceIdsThisBatch = sourceIds;
    m_throttled.clear();

    const QList<FeedDatabase::Taste> tastes = m_db.tastes();
    const FeedModel model = m_db.modelSnapshot();
    m_exploration = m_db.explorationLevel();
    {
        std::mt19937_64 rng(QRandomGenerator::global()->generate64());
        m_armMultipliers = m_db.armSnapshot().drawMultipliers(armNames(), rng);
    }

    QList<Candidate> candidates;
    candidates += candidatesFromTastes(tastes, sourceIds);
    candidates += candidatesBroad(tastes, sourceIds);
    candidates += candidatesFromCombos(model, sourceIds);
    candidates += candidatesFromFavorites(sourceIds);
    candidates += candidatesRecent(sourceIds);
    candidates += candidatesPopular(sourceIds);
    candidates += candidatesExploration(tastes, sourceIds);
    candidates += candidatesTangent(model, sourceIds);
    candidates += candidatesFromSway(sourceIds);

    // Skip-streak rescue: fetch extra trending posts when the mix goes stale.
    const int rescueStreak = m_db.recentSkipStreak();
    if (rescueStreak >= 3) {
        const int extra = qMin(4, rescueStreak - 2);
        for (int i = 0; i < extra; ++i) {
            const QString& src = pickSource(sourceIds);
            const QList<SourceImage> imgs = fetchFromSource(src, QStringLiteral("order:hot"), 15);
            for (const SourceImage& img : imgs)
                candidates.append({img, QStringLiteral("rescue"), 0});
        }
        qCInfo(lcFeed).noquote() << "exploration rescue | skip streak:" << rescueStreak
                                 << "| extra hot fetches:" << extra;
    }

    FunnelStats funnel;
    funnel.coldStart = tastes.isEmpty() ? 1 : 0;
    funnel.exploration = m_exploration;
    funnel.stage1Candidates = candidates.size();
    {
        QHash<QString, int> perArm;
        for (const Candidate& c : candidates) perArm[c.source] += 1;
        for (auto it = perArm.constBegin(); it != perArm.constEnd(); ++it)
            funnel.armCandidates.append({it.key(), it.value()});
        std::sort(funnel.armCandidates.begin(), funnel.armCandidates.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    qCInfo(lcFeed).noquote() << "stage1 candidates:" << candidates.size()
                             << "tastes:" << tastes.size() << "sources:" << sourceIds.join(',')
                             << "coldStart:" << bool(funnel.coldStart);

    // dedup + cheap filter
    const QSet<QString> hardNegs = [this]() {
        const QStringList list = m_db.hardNegativeTags();
        return QSet<QString>(list.begin(), list.end());
    }();

    QSet<QString> seen;
    QList<Candidate> filtered;
    filtered.reserve(candidates.size());
    // Per-reason drop counters, logged below.
    int dropNoMd5 = 0, dropDup = 0, dropShown = 0, dropAnimated = 0, dropScore = 0, dropHardNeg = 0;
    auto recordDrop = [&](const SourceImage& img, const QString& reason) {
        if (funnel.dropped.size() < 60)
            funnel.dropped.append({img, reason});
    };
    for (const Candidate& c : candidates) {
        const SourceImage& img = c.image;
        if (img.md5.isEmpty()) {
            ++dropNoMd5;
            continue;
        }
        // Same post via two arms can arrive with different md5s; the post id
        // catches that.
        const QString postKey = img.remoteId > 0
                                    ? img.sourceId + QLatin1Char(':') + QString::number(img.remoteId)
                                    : QString();
        if (seen.contains(img.md5) || (!postKey.isEmpty() && seen.contains(postKey))) {
            ++dropDup;
            continue;
        }
        if (m_db.wasShown(img.md5, img.sourceId, img.remoteId)) {
            ++dropShown;
            continue;
        }
        if (!m_allowAnimated.load() && isAnimated(img)) {
            ++dropAnimated;
            continue;
        }
        if (img.score < kMinPostScore) {
            ++dropScore;
            recordDrop(img, QStringLiteral("lowScore (%1 < %2)").arg(img.score).arg(kMinPostScore));
            continue;
        }
        if (!hardNegs.isEmpty()) {
            QStringList hit;
            for (const SourceTag& t : img.tags) {
                const QString name = t.name.trimmed().toLower();
                if (hardNegs.contains(name))
                    hit << name;
            }
            if (!hit.isEmpty()) {
                ++dropHardNeg;
                recordDrop(img, QStringLiteral("hardNeg: %1").arg(hit.join(QStringLiteral(", "))));
                continue;
            }
        }
        seen.insert(img.md5);
        if (!postKey.isEmpty())
            seen.insert(postKey);
        filtered.append(c);
    }

    qCInfo(lcFeed).noquote() << "stage1 filtered:" << filtered.size() << "of" << candidates.size()
                             << "- drops: shown" << dropShown << "dup" << dropDup << "animated"
                             << dropAnimated << "score" << dropScore << "hardNeg" << dropHardNeg
                             << "nomd5" << dropNoMd5;
    funnel.stage1Filtered = filtered.size();
    funnel.dropScore = dropScore;
    funnel.dropHardNeg = dropHardNeg;
    funnel.dropNoMd5 = dropNoMd5;

    // Stage 2: cheap shortlist (taste similarity + the model's tag-only
    // opinion). Exploratory candidates are protected - a shortlist ranked
    // by what is already known would filter out every tangent.
    QList<QPair<double, int>> cheap;
    cheap.reserve(filtered.size());
    {
        int protectedLeft = kProtectedExploratory;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < filtered.size(); ++i) {
            const QStringList tags = contentTags(filtered.at(i).image);
            double best = 0.0;
            for (const FeedDatabase::Taste& t : tastes)
                best = qMax(best, FeedDatabase::similarity(t.embedding, tags));
            const double learned = model.meanScore(buildModelFeatures(tags, {}, nullptr), nowMs);
            double score = best + 0.5 * std::tanh(learned / 2.0);
            if (isExploratorySource(filtered.at(i).source) && protectedLeft > 0) {
                --protectedLeft;
                score += 10.0;
            }
            cheap.append({score, i});
        }
        std::sort(cheap.begin(), cheap.end(),
                  [](const QPair<double, int>& a, const QPair<double, int>& b) {
                      return a.first > b.first;
                  });
    }
    QList<Candidate> shortlist;
    for (int i = 0; i < qMin(kFunnelTasteCount, cheap.size()); ++i)
        shortlist.append(filtered.at(cheap.at(i).second));
    for (int i = kFunnelTasteCount; i < cheap.size(); ++i)
        recordDrop(filtered.at(cheap.at(i).second).image, QStringLiteral("shortlist"));
    funnel.stage2Shortlist = shortlist.size();

    // Stage 3: visual descriptors for the most promising shortlisted posts
    // that lack one (thumbnail fetch + a few hundred microseconds each).
    if (m_visual && !shortlist.isEmpty()) {
        QStringList md5s;
        for (const Candidate& c : shortlist) md5s << c.image.md5;
        const QHash<QString, QList<float>> known = m_db.visualsFor(md5s);
        QList<SourceImage> jobs;
        for (const Candidate& c : shortlist) {
            if (jobs.size() >= kMaxVisualsPerBatch)
                break;
            if (known.contains(c.image.md5))
                continue;
            if (!c.image.previewUrl.isEmpty() || !c.image.sampleUrl.isEmpty() || c.image.isInLibrary())
                jobs.append(c.image);
        }
        if (!jobs.isEmpty())
            funnel.visualsComputedThisBatch = m_visual->ensureMany(jobs);
    }

    batch = allocateAndDiversify(shortlist, count, model, &funnel);

    qCInfo(lcFeed).noquote() << "shortlist:" << shortlist.size() << "-> batch:" << batch.size()
                             << "| tangents:" << funnel.tangentSlots << "| blend:" << funnel.blend
                             << "| exploration:" << m_exploration;

    // Last resort: never hit a dead "no posts found" wall.
    if (batch.isEmpty()) {
        funnel.emergencyFallbackUsed = 1;
        qCWarning(lcFeed).noquote()
            << "funnel produced an EMPTY batch - falling back to unpersonalized random fetch";
        const QString& src = pickSource(sourceIds);
        const QList<SourceImage> fresh =
            fetchFromSource(src, QStringLiteral("order:random"), count * 2);
        for (const SourceImage& img : fresh) {
            if (img.md5.isEmpty() || (!m_allowAnimated.load() && isAnimated(img)) ||
                img.score < kMinPostScore)
                continue;
            batch.append(img);
            if (batch.size() >= count)
                break;
        }
    }

    funnel.batchSize = batch.size();
    m_lastFunnel = funnel;
    return batch;
}

// debug

FeedRecommender::ModelStats FeedRecommender::modelStats() const {
    ModelStats stats;
    const QList<FeedDatabase::Taste> tastes = m_db.tastes();
    const QHash<QString, double> specificity = m_db.tagSpecificities();
    stats.tasteCount = tastes.size();
    for (const FeedDatabase::Taste& t : tastes) {
        ModelStats::TasteDetail detail;
        detail.id = t.id;
        detail.strength = t.strength;
        detail.confidence = t.confidence;
        detail.exposure = t.exposure;
        detail.engagement = t.engagement;
        QList<ModelStats::TasteDetail::Tag> ranked;
        for (auto it = t.embedding.constBegin(); it != t.embedding.constEnd(); ++it) {
            ModelStats::TasteDetail::Tag tag;
            tag.name = it.key();
            tag.weight = it.value();
            tag.niche = queryNicheScore(it.key(), it.value(), specificity);
            ranked.append(tag);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const ModelStats::TasteDetail::Tag& a, const ModelStats::TasteDetail::Tag& b) {
                      return a.niche > b.niche;
                  });
        QStringList names;
        for (int i = 0; i < qMin(3, ranked.size()); ++i) names << ranked.at(i).name;
        detail.tags = ranked;
        stats.tasteDetails.append(detail);
        stats.tasteSummary << QStringLiteral("t%1 (%2) %3")
                                  .arg(t.id)
                                  .arg(t.strength, 0, 'f', 1)
                                  .arg(names.join(QLatin1Char('/')));
    }
    stats.totalShown = m_db.totalShown();
    const QStringList hardNeg = m_db.hardNegativeTags();
    stats.hardNegatives = hardNeg.size();
    stats.hardNegativeTags = hardNeg;
    stats.skipStreak = m_db.recentSkipStreak();
    stats.sessionBlend = sessionBlendFor(stats.skipStreak);

    {
        const auto rows = m_db.tagPreferenceRows();
        for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
            if (it.value().preference < 0.0)
                stats.dislikedTagsFull.append(
                    {it.key(), it.value().preference, it.value().confidence});
        }
        std::sort(stats.dislikedTagsFull.begin(), stats.dislikedTagsFull.end(),
                  [](const ModelStats::TagNeg& a, const ModelStats::TagNeg& b) {
                      return a.pref < b.pref;
                  });
    }
    {
        LibrarySource library;
        SourceQuery favQuery;
        favQuery.tags = QStringLiteral("usergroup:favorite");
        favQuery.limit = 500;
        const SourceResult favs = library.search(favQuery);
        stats.favPosts = favs.ok ? favs.images.size() : 0;
    }
    stats.totalImpressions = m_db.totalImpressions();
    {
        QList<QPair<double, QString>> lowest;
        for (auto it = specificity.constBegin(); it != specificity.constEnd(); ++it)
            lowest.append({it.value(), it.key()});
        std::sort(lowest.begin(), lowest.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first < b.first;
                  });
        for (int i = 0; i < qMin(10, lowest.size()); ++i)
            stats.lowSpecificityTags << QStringLiteral("%1 (%2)")
                                            .arg(lowest.at(i).second)
                                            .arg(lowest.at(i).first, 0, 'f', 2);
    }
    stats.swayTags = m_db.swayTags();

    // learned model
    const FeedModel model = m_db.modelSnapshot();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<ModelStats::LearnedTag> learned;
    for (auto it = model.beliefs().constBegin(); it != model.beliefs().constEnd(); ++it) {
        const FeedModel::Belief b = model.belief(it.key(), nowMs);
        if (it.key().startsWith(QLatin1String("t:"))) {
            if (b.positive + b.negative >= 1.0)
                learned.append(
                    {it.key().mid(2), b.mean, std::sqrt(b.variance), b.positive + b.negative});
        } else if (it.key().startsWith(QLatin1String("h:"))) {
            stats.signalWeights.append({it.key().mid(2), b.mean});
        }
    }
    std::sort(learned.begin(), learned.end(),
              [](const ModelStats::LearnedTag& a, const ModelStats::LearnedTag& b) {
                  return a.mean > b.mean;
              });
    for (int i = 0; i < qMin(10, learned.size()); ++i)
        if (learned.at(i).mean > 0.0)
            stats.learnedLiked.append(learned.at(i));
    for (int i = learned.size() - 1; i >= qMax(0, learned.size() - 10); --i)
        if (learned.at(i).mean < 0.0)
            stats.learnedDisliked.append(learned.at(i));
    std::sort(stats.signalWeights.begin(), stats.signalWeights.end());
    stats.exploration = m_db.explorationLevel();
    stats.tangentCandidates = tangentTags(model, stats.exploration, 8);
    stats.beliefCount = model.beliefs().size();
    stats.modelSamples = model.samples();
    stats.validationSamples = model.validationSamples();
    stats.blend = model.blendWeight();
    stats.modelLogLoss = model.modelLogLoss();
    stats.heuristicLogLoss = model.heuristicLogLoss();

    const ArmBandit arms = m_db.armSnapshot();
    for (const QString& arm : armNames()) stats.armOdds.append({arm, arms.expectedReward(arm)});
    std::sort(stats.armOdds.begin(), stats.armOdds.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    stats.tangentShown = m_db.metaValue(QStringLiteral("tangent_shown"));
    stats.tangentHits = m_db.metaValue(QStringLiteral("tangent_hits"));
    stats.warmStartDone = m_db.warmStartDone();
    stats.warmStartProgress = m_db.metaValue(QStringLiteral("warmstart_progress"));
    stats.warmStartTotal = m_db.metaValue(QStringLiteral("warmstart_total"));
    return stats;
}

QList<FeedRecommender::Explanation> FeedRecommender::explainPosts(
    const QList<SourceImage>& images) const {
    QList<Explanation> out;
    if (images.isEmpty())
        return out;
    const ScoringContext ctx = buildContext();
    const FeedModel model = m_db.modelSnapshot();
    QStringList md5s;
    for (const SourceImage& img : images) md5s << img.md5;
    const QHash<QString, QList<float>> visuals = m_db.visualsFor(md5s);

    for (const SourceImage& image : images) {
        Explanation e;
        const FeedDatabase::RecommendationRow logged = m_db.recommendationFor(image.md5);
        e.loggedSource = logged.candidateSource;
        e.loggedSlot = logged.slot;

        double signalValues[kSignalCount] = {};
        qint64 tasteId = 0;
        e.heuristicScore = computeSignals(ctx, model, image, logged.candidateSource, signalValues,
                                          &tasteId, nullptr);
        e.heuristic.md5 = image.md5;
        e.heuristic.userSimilarity = signalValues[kSignalTaste];
        e.heuristic.sessionSimilarity = signalValues[kSignalSession];
        e.heuristic.tagScore = signalValues[kSignalTagPref];
        e.heuristic.freshnessScore = signalValues[kSignalFresh];
        e.heuristic.noveltyScore = signalValues[kSignalNovelty];
        e.heuristic.explorationScore = signalValues[kSignalExplore];
        e.heuristic.tasteId = tasteId;
        e.heuristic.finalScore = e.heuristicScore;

        const QList<double> visual = model.standardizeVisual(visuals.value(image.md5));
        e.visualKnown = !visual.isEmpty();
        const FeatureVector x = buildModelFeatures(contentTags(image), visual, signalValues);
        e.modelProbability = model.predict(x);
        e.uncertainty = model.uncertainty(x);
        e.blend = model.blendWeight();
        out.append(e);
    }
    return out;
}

QStringList FeedRecommender::rankLibrary(const QList<SourceImage>& posts, double exploration,
                                         const SearchProgress& progress) const {
    QStringList ranked;
    if (posts.isEmpty())
        return ranked;
    const double level = qBound(0.0, exploration, 1.0);
    const qint64 total = posts.size();
    auto report = [&progress](const QString& stage, qint64 done, qint64 of) {
        if (progress)
            progress(stage, done, of);
    };

    // Per-stage timings in the log, to see where a slow ranking goes.
    QElapsedTimer stageTimer;
    stageTimer.start();
    QStringList timings;
    auto lap = [&stageTimer, &timings](const char* name) {
        timings << QStringLiteral("%1 %2ms").arg(QLatin1String(name)).arg(stageTimer.restart());
    };

    report(QStringLiteral("Loading your taste profile"), 0, -1);
    ScoringContext ctx = buildContext();
    ctx.exploration = level;  // the query's level, not the feed slider
    const FeedModel model = m_db.modelSnapshot();
    const double blend = model.blendWeight();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    lap("context");

    // Visual descriptors in slices so a large library reports progress.
    constexpr int kVisualSlice = 2000;
    const QString visualStage = QStringLiteral("Loading visual features");
    QHash<QString, QList<float>> rawVisuals;
    rawVisuals.reserve(posts.size());
    for (int start = 0; start < posts.size(); start += kVisualSlice) {
        report(visualStage, start, total);
        QStringList md5s;
        const int end = qMin<int>(posts.size(), start + kVisualSlice);
        md5s.reserve(end - start);
        for (int i = start; i < end; ++i) md5s << posts.at(i).md5;
        rawVisuals.insert(m_db.visualsFor(md5s));
    }
    report(visualStage, total, total);

    QSet<QString> hardNegatives;
    for (const QString& tag : m_db.hardNegativeTags()) hardNegatives.insert(tag.toLower());
    lap("visuals");

    const int n = posts.size();
    QList<double> heuristic(n), meanLogit(n), sampled(n), uncertainty(n), novelty(n);
    QList<bool> hardNegative(n, false);
    // Kept for the serial Thompson pass below (exploration only).
    QList<FeatureVector> features(level > 0.0 ? n : 0);

    // Per-post scoring is independent and read-only (context, model
    // snapshot and visuals are local copies), so it fans out over the CPU
    // pool in chunks; each chunk writes only its own indices. The caller
    // thread polls the shared counter to report progress.
    constexpr int kChunk = 64;
    QList<int> chunkStarts;
    for (int start = 0; start < n; start += kChunk) chunkStarts << start;
    std::atomic<int> scoredCount{0};
    const QString scoreStage = QStringLiteral("Ranking posts");
    report(scoreStage, 0, total);
    // Raw pointers: no QList detach checks from worker threads.
    double* const heuristicOut = heuristic.data();
    double* const meanOut = meanLogit.data();
    double* const uncertaintyOut = uncertainty.data();
    double* const noveltyOut = novelty.data();
    bool* const hardNegativeOut = hardNegative.data();
    FeatureVector* const featuresOut = features.data();
    auto scoreChunk = [&](int start) {
        const int end = qMin(n, start + kChunk);
        for (int i = start; i < end; ++i) {
            const SourceImage& img = posts.at(i);
            const QStringList tags = contentTags(img);
            double signalValues[kSignalCount] = {};
            heuristicOut[i] =
                computeSignals(ctx, model, img, QString(), signalValues, nullptr, nullptr);
            noveltyOut[i] = signalValues[kSignalNovelty];
            const QList<double> visual = model.standardizeVisual(rawVisuals.value(img.md5));
            FeatureVector x = buildModelFeatures(tags, visual, signalValues);
            meanOut[i] = model.meanScore(x, nowMs);
            uncertaintyOut[i] = model.uncertainty(x, nowMs);
            for (const QString& tag : tags) {
                if (hardNegatives.contains(tag)) {
                    hardNegativeOut[i] = true;
                    break;
                }
            }
            if (level > 0.0)
                featuresOut[i] = std::move(x);
        }
        scoredCount.fetch_add(end - start, std::memory_order_relaxed);
    };
    QFuture<void> scoring =
        QtConcurrent::map(QThreadPool::globalInstance(), chunkStarts, scoreChunk);
    while (!scoring.isFinished()) {
        QThread::msleep(40);
        report(scoreStage, scoredCount.load(std::memory_order_relaxed), total);
    }
    scoring.waitForFinished();
    lap("scoring");

    // One posterior draw for the whole ranking, as for a feed batch.
    // ThompsonDraw caches its draws lazily (not thread-safe): serial, but
    // only a dot product per post.
    if (level > 0.0) {
        ThompsonDraw draw(model, thompsonTemperature(level),
                          QRandomGenerator::global()->generate64());
        for (int i = 0; i < n; ++i) sampled[i] = draw.score(features.at(i));
        features.clear();
    } else {
        sampled = meanLogit;
    }
    report(scoreStage, total, total);
    lap("thompson");

    // Everything on a common scale: z-scores within this result set.
    auto zScores = [](QList<double>& values) {
        double mean, sd;
        meanStd(values, &mean, &sd);
        for (double& v : values) v = (v - mean) / sd;
    };
    zScores(heuristic);
    zScores(meanLogit);
    zScores(sampled);
    zScores(uncertainty);
    zScores(novelty);

    // Exploit: the feed's relevance blend on the posterior mean - the
    // model's share is exactly as large as it has earned.
    // Explore: a Thompson-sampled relevance plus credit for what the model
    // is unsure about / has never seen, with random spread so repeated
    // searches surface different corners of the library.
    std::mt19937_64 rng(QRandomGenerator::global()->generate64());
    std::normal_distribution<double> noise(0.0, 0.6);
    QList<QPair<double, int>> scores;
    scores.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double exploit = (1.0 - blend) * heuristic[i] + blend * meanLogit[i];
        double score = exploit;
        if (level > 0.0) {
            const double sampledRelevance = (1.0 - blend) * heuristic[i] + blend * sampled[i];
            const double explore = 0.4 * sampledRelevance + 0.35 * uncertainty[i] +
                                   0.25 * novelty[i] + noise(rng);
            score = (1.0 - level) * exploit + level * explore;
        }
        // Learned hard negatives sink but stay reachable (it's your library).
        if (hardNegative[i])
            score -= 3.0;
        scores.append({score, i});
    }
    std::stable_sort(scores.begin(), scores.end(),
                     [](const QPair<double, int>& a, const QPair<double, int>& b) {
                         return a.first > b.first;
                     });
    ranked.reserve(n);
    for (const auto& [score, index] : scores) ranked << posts.at(index).md5;
    lap("sort");
    qCInfo(lcFeed).noquote() << "library ranking |" << n << "posts | exploration" << level << "|"
                             << timings.join(QStringLiteral(", "));
    return ranked;
}

}  // namespace Mosaic
