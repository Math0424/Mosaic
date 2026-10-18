#include "FeedRecommender.h"
#include "FeedDatabase.h"
#include "UserModel.h"
#include "EmbeddingBackend.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"

#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

// Dedicated logging category; enable per-batch traces with
// QT_LOGGING_RULES="mosaic.feed.debug=true".
Q_LOGGING_CATEGORY(lcFeed, "mosaic.feed")

namespace Mosaic {

namespace {

// Ranking coefficients.
constexpr double kUserSimWeight = 0.35;
constexpr double kSessionSimWeight = 0.2;
constexpr double kTagPrefWeight = 0.10;
constexpr double kFavOverlapWeight = 0.15;
constexpr double kFreshnessWeight = 0.05;
constexpr double kNoveltyWeight = 0.04;
constexpr double kExplorationWeight = 0.03;
constexpr double kPopularityWeight = 0.03;
// Soft penalty for resembling a learned negative visual anchor.
constexpr double kNegAnchorWeight = 0.30;

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

// Exploration floor: minimum fraction of exploration candidates per batch.
constexpr double kExplorationFloor = 0.15;

// Quality floor: minimum site score.
constexpr int kMinPostScore = 0;

// Candidate funnel sizes.
constexpr int kFunnelTasteCount = 150;
constexpr int kFunnelVisualCount = 50;
// Max thumbnails embedded per batch - keeps the first post appearing fast.
constexpr int kMaxEmbeddingsPerBatch = 24;

// Animated content is excluded from the feed entirely.
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
// mishandle ">=" inside compound queries.
const QString kMinScoreFilter = QStringLiteral("score:>2 -animated");

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

// Jaccard-style tag overlap, used by MMR as a cheap similarity proxy.
double tagOverlap(const SourceImage& a, const SourceImage& b) {
    QSet<QString> tagsA, tagsB;
    for (const SourceTag& t : a.tags) {
        if (!FeedRecommender::isGenericTag(t.name))
            tagsA.insert(t.name.toLower());
    }
    for (const SourceTag& t : b.tags) {
        if (!FeedRecommender::isGenericTag(t.name))
            tagsB.insert(t.name.toLower());
    }
    if (tagsA.isEmpty() || tagsB.isEmpty())
        return 0.0;
    int inter = 0;
    for (const QString& t : tagsA) {
        if (tagsB.contains(t))
            ++inter;
    }
    const int unionSize = tagsA.size() + tagsB.size() - inter;
    return unionSize > 0 ? static_cast<double>(inter) / unionSize : 0.0;
}

}  // namespace

FeedRecommender::FeedRecommender(FeedDatabase& db, SourceManager& sources,
                                 const QStringList& feedSourceIds, EmbeddingBackend* embeddings)
    : m_db(db), m_sources(sources), m_feedSourceIds(feedSourceIds), m_embeddings(embeddings) {}

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

// Positive tag prefs are tanh-compressed; negatives stay linear.
constexpr double kTagPrefCeiling = 8.0;

// Bayesian shrinkage prior for tag preferences.
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

double saturateTagPref(double w) {
    if (w <= 0.0)
        return w;
    return kTagPrefCeiling * std::tanh(w / kTagPrefCeiling);
}

// Bayesian shrinkage toward zero by evidence mass: weak opinions stay
// weak until consistent evidence accumulates.
double shrinkTagPref(double preference, double confidence) {
    return preference * (confidence / (confidence + kTagPrefPrior));
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

// candidate arms

QList<SourceImage> FeedRecommender::fetchFromSource(const QString& sourceId, const QString& query,
                                                    int limit) const {
    Source* source = m_sources.source(sourceId);
    if (!source || !source->isEnabled())
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

        QString fullTags = r34Query.isEmpty() ? QStringLiteral("-animated")
                                              : r34Query + QStringLiteral(" -animated");
        if (!negExclusions.isEmpty())
            fullTags += negExclusions;
        q.tags = fullTags.trimmed();
    } else {
        QString filter = kMinScoreFilter;
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
    return result.ok ? result.images : QList<SourceImage>{};
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromTastes(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (tastes.isEmpty() || sourceIds.isEmpty())
        return out;

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
    for (int rank = 0; rank < qMin(5, ranked.size()); ++rank) {
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
        QString query = anchor + optional;
        if (tagsRanked.size() >= 2) {
            const QString strict = anchor + QLatin1Char(' ') + tagsRanked.at(1).second + optional;
            const QString& strictSrc =
                sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
            for (const SourceImage& img : fetchFromSource(strictSrc, strict, 20))
                out.append({img, QStringLiteral("taste-strict"), taste.id});
        }

        const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
        const QList<SourceImage> imgs = fetchFromSource(src, query, 30);
        for (const SourceImage& img : imgs) out.append({img, QStringLiteral("taste"), taste.id});
    }
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesFromCombos(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (sourceIds.isEmpty())
        return out;

    const QHash<QString, double> specificity = m_db.tagSpecificities();
    const auto prefRows = m_db.tagPreferenceRows();
    QList<QPair<double, QString>> strong;
    for (auto it = prefRows.constBegin(); it != prefRows.constEnd(); ++it) {
        const double w = shrinkTagPref(it.value().preference, it.value().confidence);
        if (w > 0.5 && !isGenericTag(it.key()))
            strong.append({queryNicheScore(it.key(), w, specificity), it.key()});
    }
    if (strong.size() < 2)
        return out;
    std::sort(strong.begin(), strong.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                  return a.first > b.first;
              });

    // Random pick among the top 6 so queries vary between batches.
    const int pool = qMin(6, strong.size());
    const int a = QRandomGenerator::global()->bounded(pool);
    int b = QRandomGenerator::global()->bounded(pool);
    if (b == a)
        b = (b + 1) % pool;
    const QString query = strong.at(a).second + QLatin1Char(' ') + strong.at(b).second;
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
    const QList<SourceImage> imgs = fetchFromSource(src, query, 20);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("combo"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesBroad(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (tastes.isEmpty() || sourceIds.isEmpty())
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

        const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
        const QList<SourceImage> imgs = fetchFromSource(src, query, 30);
        for (const SourceImage& img : imgs) out.append({img, QStringLiteral("broad"), taste.id});
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
    if (sourceIds.isEmpty())
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

    // AND the two strongest seed tags for a high-precision query.
    std::shuffle(seedTags.begin(), seedTags.end(), *QRandomGenerator::global());
    const QString query = seedTags.mid(0, 2).join(QLatin1Char(' '));
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
    const QList<SourceImage> imgs = fetchFromSource(src, query, 25);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("similar-to-liked"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesRecent(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (sourceIds.isEmpty())
        return out;
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
    const QList<SourceImage> imgs = fetchFromSource(src, QStringLiteral("order:score"), 15);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("recent"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesPopular(
    const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (sourceIds.isEmpty())
        return out;
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
    const QList<SourceImage> imgs = fetchFromSource(src, QStringLiteral("order:hot"), 15);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("popular"), 0});
    return out;
}

QList<FeedRecommender::Candidate> FeedRecommender::candidatesExploration(
    const QList<FeedDatabase::Taste>& tastes, const QStringList& sourceIds) const {
    QList<Candidate> out;
    if (sourceIds.isEmpty())
        return out;

    // Exploration must not be random garbage: wildcard-expand a
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
                const QString& src =
                    sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
                const QList<SourceImage> imgs =
                    fetchFromSource(src, tag.left(underscore) + QStringLiteral("_*"), 15);
                for (const SourceImage& img : imgs)
                    out.append({img, QStringLiteral("exploration"), 0});
                return out;
            }
        }
    }

    // Fallback: a small amount of pure random.
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
    const QList<SourceImage> imgs = fetchFromSource(src, QStringLiteral("order:random"), 10);
    for (const SourceImage& img : imgs) out.append({img, QStringLiteral("exploration"), 0});
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
    const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
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

// allocation + MMR

QList<SourceImage> FeedRecommender::allocateAndDiversify(QList<Candidate>& candidates,
                                                         int count) const {
    QList<SourceImage> batch;
    if (candidates.isEmpty() || count <= 0)
        return batch;

    const QList<FeedDatabase::Taste> tastes = m_db.tastes();
    const int totalExposure = [tastes]() {
        int total = 0;
        for (const FeedDatabase::Taste& t : tastes) total += static_cast<int>(t.exposure);
        return qMax(1, total);
    }();

    // Score every candidate once (pre-diversity relevance).
    const auto prefRows = m_db.tagPreferenceRows();
    QHash<QString, double> prefs;
    prefs.reserve(prefRows.size());
    for (auto it = prefRows.constBegin(); it != prefRows.constEnd(); ++it)
        prefs.insert(it.key(),
                     saturateTagPref(shrinkTagPref(it.value().preference, it.value().confidence)));
    const int skipStreak = m_db.recentSkipStreak();
    const double blend = sessionBlendFor(skipStreak);

    const QSet<QString> hardNegs = [this]() {
        const QStringList list = m_db.hardNegativeTags();
        return QSet<QString>(list.begin(), list.end());
    }();
    const QHash<QString, int> favTagFreqs = favoriteTagFrequencies();
    // Self-learned specificity; neutral 0.6 for unseen tags (cold start).
    const QHash<QString, double> specificity = m_db.tagSpecificities();
    auto specificityOf = [&specificity](const QString& tag) {
        auto it = specificity.constFind(tag);
        return it != specificity.constEnd() ? it.value() : 0.6;
    };
    const QList<FeedDatabase::NegativeAnchor> negAnchors = m_db.negativeAnchors();

    struct Scored {
        Candidate candidate;
        double userSim = 0.0;
        double sessionSim = 0.0;
        double tagScore = 0.0;
        double favOverlapScore = 0.0;
        double freshness = 0.0;
        double novelty = 0.0;
        double exploration = 0.0;
        double negAnchorSim = 0.0;
        double popularity = 0.0;
        double relevance = 0.0;
        double learnedProb = 0.5;
        double features[UserModel::kFeatureCount] = {};
        qint64 tasteId = 0;
        double tastePriority = 0.0;
    };

    // Embeddings cached once for both the anchor penalty and MMR.
    QHash<QString, QByteArray> embeddings;
    if (m_embeddings && m_embeddings->isReady()) {
        for (const Candidate& c : candidates) {
            if (!embeddings.contains(c.image.md5)) {
                embeddings.insert(c.image.md5,
                                  m_db.embeddingFor(c.image.md5, m_embeddings->modelName(),
                                                    m_embeddings->modelVersion()));
            }
        }
    }

    QList<Scored> scored;
    scored.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        if (!hardNegs.isEmpty()) {
            bool hasHardNeg = false;
            for (const SourceTag& t : c.image.tags) {
                if (hardNegs.contains(t.name.trimmed().toLower())) {
                    hasHardNeg = true;
                    break;
                }
            }
            if (hasHardNeg)
                continue;
        }

        Scored s;
        s.candidate = c;
        const QStringList tags = contentTags(c.image);

        // User similarity: max over tastes of
        // similarity × strength - the strongest relevant taste drives the
        // score, interests never dilute each other by averaging.
        double bestSim = 0.0;
        qint64 bestTaste = 0;
        double bestPriority = 0.0;
        for (const FeedDatabase::Taste& t : tastes) {
            const double sim = FeedDatabase::similarity(t.embedding, tags);
            const double weighted = sim * (0.5 + qMin(1.0, t.strength) * 0.5);
            if (weighted > bestSim) {
                bestSim = weighted;
                bestTaste = t.id;
                bestPriority = tastePriority(t, totalExposure);
            }
        }
        s.userSim = bestSim;
        s.tasteId = bestTaste;
        s.tastePriority = bestPriority;

        // Session similarity (what the user wants right now).
        s.sessionSim = m_db.sessionSimilarity(tags);

        // Tag preference: supporting signal, weighted by tag category.
        double tagSum = 0.0;
        double weightSum = 0.0;
        double favMatches = 0.0;
        for (const SourceTag& t : c.image.tags) {
            if (isGenericTag(t.name))
                continue;
            const QString tag = t.name.trimmed().toLower();
            // Category boost x self-learned specificity.
            const double boost = categoryBoost(t.category) * specificityOf(tag);
            auto it = prefs.constFind(tag);
            if (it != prefs.constEnd())
                tagSum += it.value() * boost;
            weightSum += boost;

            if (favTagFreqs.contains(tag)) {
                favMatches += std::log1p(static_cast<double>(favTagFreqs.value(tag))) * boost;
            }
        }
        s.tagScore = weightSum > 0.0 ? tagSum / weightSum : 0.0;
        s.favOverlapScore = weightSum > 0.0 ? qMin(1.0, favMatches / (weightSum * 1.5)) : 0.0;

        if (c.image.createdAt > 0) {
            const double ageDays =
                (QDateTime::currentSecsSinceEpoch() - c.image.createdAt) / 86400.0;
            s.freshness = std::exp(-ageDays / 30.0);
        }

        int unknown = 0;
        for (const QString& tag : tags) {
            if (!prefs.contains(tag))
                ++unknown;
        }
        s.novelty = tags.isEmpty() ? 0.0 : static_cast<double>(unknown) / tags.size();

        // Exploration: candidates from the exploration arm get a boost.
        s.exploration = c.source == QLatin1String("exploration") ? 1.0 : 0.0;

        s.popularity =
            c.image.score > 0 ? std::log1p(static_cast<double>(c.image.score)) / 10.0 : 0.0;

        // Negative visual anchors: soft look-based penalty.
        if (!negAnchors.isEmpty()) {
            const QByteArray& emb = embeddings.value(c.image.md5);
            if (!emb.isEmpty())
                s.negAnchorSim = m_db.maxNegativeAnchorSimilarity(emb);
        }

        // Blend long-term taste with session taste.
        const double blendedTasteSim = (1.0 - blend) * s.userSim + blend * s.sessionSim;
        const double bayesian =
            kUserSimWeight * blendedTasteSim + kSessionSimWeight * s.sessionSim +
            kTagPrefWeight * s.tagScore + kFavOverlapWeight * s.favOverlapScore +
            kFreshnessWeight * s.freshness + kNoveltyWeight * s.novelty +
            kExplorationWeight * s.exploration + kPopularityWeight * s.popularity -
            kNegAnchorWeight * s.negAnchorSim;

        // Stage-2 learned model over the same signals.
        s.features[0] = s.userSim;
        s.features[1] = s.sessionSim;
        s.features[2] = s.tagScore;
        s.features[3] = s.favOverlapScore;
        s.features[4] = s.freshness;
        s.features[5] = s.novelty;
        s.features[6] = s.exploration;
        s.features[7] = s.popularity;
        s.features[8] = s.negAnchorSim;
        s.features[UserModel::kBiasIndex] = 1.0;
        s.learnedProb = m_db.userModelPredict(s.features);
        const double learnedBlend = m_db.userModelBlend();
        s.relevance = (1.0 - learnedBlend) * bayesian + learnedBlend * (s.learnedProb - 0.5);

        scored.append(s);
    }

    // taste allocation
    // Quotas per taste from dynamic priority; exploration keeps a floor.
    QHash<qint64, double> prioritySum;
    double totalPriority = 0.0;
    for (const Scored& s : scored) {
        if (s.tasteId > 0) {
            prioritySum[s.tasteId] += s.tastePriority;
            totalPriority += s.tastePriority;
        }
    }

    QHash<qint64, int> quota;
    if (totalPriority > 0 && !tastes.isEmpty()) {
        int allocated = 0;
        for (const FeedDatabase::Taste& t : tastes) {
            const double share = prioritySum.value(t.id, 0.0) / totalPriority;
            const int q = static_cast<int>(share * count * 1.2);  // headroom
            quota[t.id] = q;
            allocated += q;
        }
        const int explorationQuota = qMax(1, static_cast<int>(count * kExplorationFloor));
        Q_UNUSED(explorationQuota);
        Q_UNUSED(allocated);
    }

    QHash<qint64, int> used;
    used.reserve(quota.size());

    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b) { return a.relevance > b.relevance; });

    // Embeddings built above are reused as the MMR redundancy signal.

    QList<Scored> selected;
    QList<Scored> deferred;
    for (Scored& s : scored) {
        if (selected.size() >= count)
            break;

        // Taste quota check (only when quotas exist).
        if (s.tasteId > 0 && !quota.isEmpty()) {
            const int q = quota.value(s.tasteId, 0);
            if (used.value(s.tasteId, 0) >= q && q > 0) {
                deferred.append(s);
                continue;
            }
        }

        // Per-taste batch cap, independent of the quota system above.
        if (s.tasteId > 0) {
            const int hardCap = qMax(2, static_cast<int>(count * kMaxShareOfBatchPerTaste));
            if (used.value(s.tasteId, 0) >= hardCap) {
                deferred.append(s);
                continue;
            }
        }

        // MMR: embedding cosine primary, tag Jaccard secondary.
        double maxSim = 0.0;
        const QByteArray& emb = embeddings.value(s.candidate.image.md5);
        for (const Scored& sel : selected) {
            double sim = 0.0;
            const QByteArray& selEmb = embeddings.value(sel.candidate.image.md5);
            if (!emb.isEmpty() && !selEmb.isEmpty()) {
                sim = EmbeddingBackend::cosineSimilarity(emb, selEmb);
            } else {
                sim = tagOverlap(s.candidate.image, sel.candidate.image);
            }
            sim = qMax(sim, 0.3 * tagOverlap(s.candidate.image, sel.candidate.image));
            maxSim = qMax(maxSim, sim);
        }
        const double mmrScore = kMmrLambda * s.relevance - (1.0 - kMmrLambda) * maxSim;
        s.relevance = mmrScore;

        selected.append(s);
        if (s.tasteId > 0)
            used[s.tasteId] += 1;
    }

    // Backfill from deferred if quotas left us short.
    for (const Scored& s : deferred) {
        if (selected.size() >= count)
            break;
        selected.append(s);
    }

    // debug logging
    QList<FeedDatabase::RecommendationRow> rows;
    for (const Scored& s : selected) {
        FeedDatabase::RecommendationRow r;
        r.md5 = s.candidate.image.md5;
        r.userSimilarity = s.userSim;
        r.sessionSimilarity = s.sessionSim;
        r.tagScore = s.tagScore;
        r.freshnessScore = s.freshness;
        r.noveltyScore = s.novelty;
        r.explorationScore = s.exploration;
        r.tasteId = s.tasteId;
        r.tastePriority = s.tastePriority;
        r.preDiversityScore = s.relevance;
        r.finalScore = s.relevance;
        r.candidateSource = s.candidate.source;
        r.rejectedNegative = false;
        // Stage-2 model trains on this when the reaction arrives.
        QStringList featureToks;
        featureToks.reserve(UserModel::kFeatureCount);
        for (int i = 0; i < UserModel::kFeatureCount; ++i)
            featureToks << QString::number(s.features[i], 'f', 4);
        r.features = featureToks.join(QLatin1Char(' '));
        rows.append(r);
    }
    m_db.recordRecommendations(rows);

    // Record exposure for the tastes that got slots.
    for (const Scored& s : selected) {
        if (s.tasteId > 0)
            m_db.recordTasteExposure(s.tasteId, 1.0);
    }

    for (const Scored& s : selected) batch.append(s.candidate.image);
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

    const QList<FeedDatabase::Taste> tastes = m_db.tastes();

    // Funnel: server queries -> taste filter -> thumbnail embeddings ->
    // diversity. Cold start skips stages 2-3 so the first post appears
    // after one quick query.

    QList<Candidate> candidates;
    candidates += candidatesFromTastes(tastes, sourceIds);
    candidates += candidatesBroad(tastes, sourceIds);
    candidates += candidatesFromCombos(sourceIds);
    candidates += candidatesFromFavorites(sourceIds);
    candidates += candidatesRecent(sourceIds);
    candidates += candidatesPopular(sourceIds);
    candidates += candidatesExploration(tastes, sourceIds);
    candidates += candidatesFromSway(sourceIds);

    // Skip-streak rescue: fetch extra trending posts when the mix goes stale.
    const int rescueStreak = m_db.recentSkipStreak();
    if (rescueStreak >= 3) {
        const int extra = qMin(4, rescueStreak - 2);
        for (int i = 0; i < extra; ++i) {
            const QString& src =
                sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
            const QList<SourceImage> imgs = fetchFromSource(src, QStringLiteral("order:hot"), 15);
            for (const SourceImage& img : imgs)
                candidates.append({img, QStringLiteral("rescue"), 0});
        }
        qCInfo(lcFeed).noquote() << "exploration rescue | skip streak:" << rescueStreak
                                 << "| extra hot fetches:" << extra;
    }

    FunnelStats funnel;
    funnel.coldStart = tastes.isEmpty() ? 1 : 0;
    funnel.stage1Candidates = candidates.size();
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
        if (seen.contains(img.md5)) {
            ++dropDup;
            continue;
        }
        if (m_db.wasShown(img.md5)) {
            ++dropShown;
            continue;
        }
        if (isAnimated(img)) {
            ++dropAnimated;
            continue;
        }
        if (img.score < kMinPostScore) {
            ++dropScore;
            recordDrop(img, QStringLiteral("lowScore (%1 < %2)").arg(img.score).arg(kMinPostScore));
            continue;
        }
        if (!hardNegs.isEmpty()) {
            bool hasNeg = false;
            for (const SourceTag& t : img.tags) {
                if (hardNegs.contains(t.name.trimmed().toLower())) {
                    hasNeg = true;
                    break;
                }
            }
            if (hasNeg) {
                ++dropHardNeg;
                QStringList hit;
                for (const SourceTag& t : img.tags) {
                    const QString name = t.name.trimmed().toLower();
                    if (hardNegs.contains(name))
                        hit << name;
                }
                recordDrop(img, QStringLiteral("hardNeg: %1").arg(hit.join(QStringLiteral(", "))));
                continue;
            }
        }
        seen.insert(img.md5);
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

    // Stage 2 + 3: taste filter + visual relevance
    // Skipped entirely on cold start (no tastes = nothing to compare).
    QList<Candidate> finalists;
    if (tastes.isEmpty()) {
        finalists = filtered.mid(0, kFunnelVisualCount);
    } else {
        // Stage 2: taste-vector filter
        QList<Candidate> shortlist;
        if (filtered.size() > kFunnelTasteCount) {
            QList<QPair<double, int>> ranked;
            ranked.reserve(filtered.size());
            for (int i = 0; i < filtered.size(); ++i) {
                const QStringList tags = contentTags(filtered.at(i).image);
                double best = 0.0;
                for (const FeedDatabase::Taste& t : tastes)
                    best = qMax(best, FeedDatabase::similarity(t.embedding, tags));
                ranked.append({best, i});
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const QPair<double, int>& a, const QPair<double, int>& b) {
                          return a.first > b.first;
                      });
            for (int i = 0; i < qMin(kFunnelTasteCount, ranked.size()); ++i)
                shortlist.append(filtered.at(ranked.at(i).second));
        } else {
            shortlist = filtered;
        }
        funnel.stage2Shortlist = shortlist.size();

        // Stage-2 drops are real rejections (taste mismatch), so they show
        // up in the dropped-images view.
        if (filtered.size() > kFunnelTasteCount) {
            QSet<QString> kept;
            for (const Candidate& c : shortlist) kept.insert(c.image.md5);
            for (const Candidate& c : filtered)
                if (!kept.contains(c.image.md5))
                    recordDrop(c.image, QStringLiteral("tasteFilter"));
        }

        // Stage 3: visual relevance via thumbnails
        if (m_embeddings && m_embeddings->isReady() && !shortlist.isEmpty()) {
            QList<SourceImage> jobs;
            for (const Candidate& c : shortlist) {
                if (jobs.size() >= kMaxEmbeddingsPerBatch)
                    break;
                if (m_db.embeddingFor(c.image.md5, m_embeddings->modelName(),
                                      m_embeddings->modelVersion())
                        .isEmpty()) {
                    // Remote posts need a thumbnail URL; local library posts
                    // are embedded from the stored file (empty URL is fine).
                    if (!c.image.previewUrl.isEmpty() || !c.image.sampleUrl.isEmpty() ||
                        c.image.isLocal())
                        jobs.append(c.image);
                }
            }
            if (!jobs.isEmpty())
                m_embeddings->ensureEmbeddings(jobs);
            funnel.embeddingsComputedThisBatch = jobs.size();

            QList<QPair<double, int>> visualRanked;
            visualRanked.reserve(shortlist.size());
            for (int i = 0; i < shortlist.size(); ++i) {
                const QByteArray emb =
                    m_db.embeddingFor(shortlist.at(i).image.md5, m_embeddings->modelName(),
                                      m_embeddings->modelVersion());
                double best = 0.0;
                if (!emb.isEmpty()) {
                    for (const FeedDatabase::Taste& t : tastes) {
                        if (t.visual.isEmpty())
                            continue;
                        best = qMax(best, FeedDatabase::similarityVisual(t.visual, emb));
                    }
                }
                visualRanked.append({best, i});
            }
            std::sort(visualRanked.begin(), visualRanked.end(),
                      [](const QPair<double, int>& a, const QPair<double, int>& b) {
                          return a.first > b.first;
                      });
            for (int i = 0; i < qMin(kFunnelVisualCount, visualRanked.size()); ++i)
                finalists.append(shortlist.at(visualRanked.at(i).second));

            QSet<QString> finalistMd5s;
            for (const Candidate& c : finalists) finalistMd5s.insert(c.image.md5);
            for (const Candidate& c : shortlist)
                if (!finalistMd5s.contains(c.image.md5))
                    recordDrop(c.image, QStringLiteral("visualRank"));
        } else {
            finalists = shortlist.mid(0, kFunnelVisualCount);
        }
    }

    if (finalists.isEmpty() && !filtered.isEmpty())
        finalists = filtered.mid(0, count);

    funnel.stage3Finalists = finalists.size();
    batch = allocateAndDiversify(finalists, count);

    qCInfo(lcFeed).noquote() << "finalists:" << finalists.size() << "-> batch:" << batch.size();

    // Last resort: never hit a dead "no posts found" wall.
    if (batch.isEmpty()) {
        funnel.emergencyFallbackUsed = 1;
        qCWarning(lcFeed).noquote()
            << "funnel produced an EMPTY batch - falling back to unpersonalized random fetch";
        const QString& src = sourceIds.at(QRandomGenerator::global()->bounded(sourceIds.size()));
        const QList<SourceImage> fresh =
            fetchFromSource(src, QStringLiteral("order:random"), count * 2);
        for (const SourceImage& img : fresh) {
            if (img.md5.isEmpty() || isAnimated(img) || img.score < kMinPostScore)
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
    stats.tasteCount = tastes.size();
    for (const FeedDatabase::Taste& t : tastes) {
        ModelStats::TasteDetail detail;
        QString topTags;
        detail.id = t.id;
        detail.strength = t.strength;
        detail.confidence = t.confidence;
        detail.exposure = t.exposure;
        detail.engagement = t.engagement;
        detail.visual = !t.visual.isEmpty();
        if (!t.embedding.isEmpty()) {
            const QHash<QString, double> specificity = m_db.tagSpecificities();
            QList<ModelStats::TasteDetail::Tag> ranked;
            for (auto it = t.embedding.constBegin(); it != t.embedding.constEnd(); ++it) {
                ModelStats::TasteDetail::Tag tag;
                tag.name = it.key();
                tag.weight = it.value();
                tag.niche = queryNicheScore(it.key(), it.value(), specificity);
                ranked.append(tag);
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const ModelStats::TasteDetail::Tag& a,
                         const ModelStats::TasteDetail::Tag& b) { return a.niche > b.niche; });
            QStringList names;
            for (int i = 0; i < qMin(3, ranked.size()); ++i) names << ranked.at(i).name;
            topTags = names.join(QLatin1Char('/'));
            detail.tags = ranked;
        } else {
            topTags = QStringLiteral("[visual]");
        }
        stats.tasteDetails.append(detail);
        stats.tasteSummary
            << QStringLiteral("t%1 (%2) %3").arg(t.id).arg(t.strength, 0, 'f', 1).arg(topTags);
    }
    stats.totalShown = m_db.totalShown();
    const QStringList hardNeg = m_db.hardNegativeTags();
    stats.hardNegatives = hardNeg.size();
    stats.hardNegativeTags = hardNeg;
    stats.skipStreak = m_db.recentSkipStreak();

    {
        const auto rows = m_db.tagPreferenceRows();
        QList<QPair<double, QString>> pos, neg;
        for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
            const double w = shrinkTagPref(it.value().preference, it.value().confidence);
            if (w > 0.05)
                pos.append({w, it.key()});
            else if (w < -0.05)
                neg.append({w, it.key()});
        }
        std::sort(pos.begin(), pos.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first > b.first;
                  });
        std::sort(neg.begin(), neg.end(),
                  [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                      return a.first < b.first;
                  });
        for (int i = 0; i < qMin(10, pos.size()); ++i)
            stats.likedTags.append({pos.at(i).second, pos.at(i).first});
        for (int i = 0; i < qMin(10, neg.size()); ++i)
            stats.dislikedTags.append({neg.at(i).second, neg.at(i).first});
        // Full negative list with confidence, for debugging resurfacing tags.
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
    stats.sessionBlend = sessionBlendFor(stats.skipStreak);
    {
        LibrarySource library;
        SourceQuery favQuery;
        favQuery.tags = QStringLiteral("usergroup:favorite");
        favQuery.limit = 500;
        const SourceResult favs = library.search(favQuery);
        stats.favPosts = favs.ok ? favs.images.size() : 0;
    }
    stats.negativeAnchors = m_db.negativeAnchors().size();
    stats.totalImpressions = m_db.totalImpressions();
    {
        const auto spec = m_db.tagSpecificities();
        QList<QPair<double, QString>> lowest;
        for (auto it = spec.constBegin(); it != spec.constEnd(); ++it)
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
    stats.learnedSamples = m_db.userModelSamples();
    stats.learnedBlend = m_db.userModelBlend();
    stats.learnedWeights = m_db.userModelWeights();
    stats.swayTags = m_db.swayTags();
    return stats;
}

FeedDatabase::RecommendationRow FeedRecommender::explainPost(const SourceImage& image) const {
    FeedDatabase::RecommendationRow r;
    r.md5 = image.md5;
    const QStringList tags = contentTags(image);
    const QList<FeedDatabase::Taste> tastes = m_db.tastes();

    double bestSim = 0.0;
    for (const FeedDatabase::Taste& t : tastes) {
        const double sim = FeedDatabase::similarity(t.embedding, tags);
        if (sim > bestSim) {
            bestSim = sim;
            r.tasteId = t.id;
        }
    }
    r.userSimilarity = bestSim;
    r.sessionSimilarity = m_db.sessionSimilarity(tags);

    const auto prefRows = m_db.tagPreferenceRows();
    QHash<QString, double> prefs;
    prefs.reserve(prefRows.size());
    for (auto it = prefRows.constBegin(); it != prefRows.constEnd(); ++it)
        prefs.insert(it.key(),
                     saturateTagPref(shrinkTagPref(it.value().preference, it.value().confidence)));
    double tagSum = 0.0;
    double weightSum = 0.0;
    double favMatches = 0.0;
    const QHash<QString, int> favTagFreqs = favoriteTagFrequencies();
    for (const SourceTag& t : image.tags) {
        if (isGenericTag(t.name))
            continue;
        const QString tag = t.name.trimmed().toLower();
        const double boost = categoryBoost(t.category);
        auto it = prefs.constFind(tag);
        if (it != prefs.constEnd())
            tagSum += it.value() * boost;
        weightSum += boost;

        if (favTagFreqs.contains(tag)) {
            favMatches += std::log1p(static_cast<double>(favTagFreqs.value(tag))) * boost;
        }
    }
    r.tagScore = weightSum > 0.0 ? tagSum / weightSum : 0.0;
    const double favOverlapScore =
        weightSum > 0.0 ? qMin(1.0, favMatches / (weightSum * 1.5)) : 0.0;

    if (image.createdAt > 0) {
        const double ageDays = (QDateTime::currentSecsSinceEpoch() - image.createdAt) / 86400.0;
        r.freshnessScore = std::exp(-ageDays / 30.0);
    }

    const int skipStreak = m_db.recentSkipStreak();
    const double blend = sessionBlendFor(skipStreak);
    const double blendedTasteSim = (1.0 - blend) * r.userSimilarity + blend * r.sessionSimilarity;
    r.finalScore = kUserSimWeight * blendedTasteSim + kSessionSimWeight * r.sessionSimilarity +
                   kTagPrefWeight * r.tagScore + kFavOverlapWeight * favOverlapScore +
                   kFreshnessWeight * r.freshnessScore;
    return r;
}

FeedRecommender::VisualVerdict FeedRecommender::explainVisual(const SourceImage& image) const {
    VisualVerdict v;
    if (!m_embeddings || !m_embeddings->isReady())
        return v;
    const QByteArray emb =
        m_db.embeddingFor(image.md5, m_embeddings->modelName(), m_embeddings->modelVersion());
    if (emb.isEmpty())
        return v;
    v.available = true;

    // Best positive-taste visual similarity = "how much does this look like
    // what you already like".
    const QList<FeedDatabase::Taste> tastes = m_db.tastes();
    for (const FeedDatabase::Taste& t : tastes) {
        if (t.visual.isEmpty())
            continue;
        const double sim = FeedDatabase::similarityVisual(t.visual, emb);
        if (sim > v.bestSim) {
            v.bestSim = sim;
            v.bestTasteId = t.id;
        }
    }
    v.negAnchorSim = m_db.maxNegativeAnchorSimilarity(emb);

    // Stage-2 learned model over the same features the ranker uses.
    const QStringList tags = contentTags(image);
    const auto prefRows = m_db.tagPreferenceRows();
    QHash<QString, double> prefs;
    prefs.reserve(prefRows.size());
    for (auto it = prefRows.constBegin(); it != prefRows.constEnd(); ++it)
        prefs.insert(it.key(),
                     saturateTagPref(shrinkTagPref(it.value().preference, it.value().confidence)));
    double tagSum = 0.0, weightSum = 0.0;
    for (const SourceTag& t : image.tags) {
        if (isGenericTag(t.name))
            continue;
        const QString tag = t.name.trimmed().toLower();
        const double boost = categoryBoost(t.category);
        auto it = prefs.constFind(tag);
        if (it != prefs.constEnd())
            tagSum += it.value() * boost;
        weightSum += boost;
    }
    const double tagScore = weightSum > 0.0 ? tagSum / weightSum : 0.0;
    const double sessionSim = m_db.sessionSimilarity(tags);

    double features[UserModel::kFeatureCount] = {};
    features[0] = v.bestSim;
    features[1] = sessionSim;
    features[2] = tagScore;
    features[3] = 0.0;  // fav overlap omitted for the quick verdict
    features[7] = image.score > 0 ? std::log1p(static_cast<double>(image.score)) / 10.0 : 0.0;
    features[8] = v.negAnchorSim;
    features[UserModel::kBiasIndex] = 1.0;
    v.learnedProb = m_db.userModelPredict(features);
    return v;
}

}  // namespace Mosaic
