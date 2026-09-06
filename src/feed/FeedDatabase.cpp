#include "FeedDatabase.h"

#include "EmbeddingBackend.h"
#include "FeedRecommender.h"
#include "../core/Paths.h"

#include <QDateTime>
#include <QDebug>
#include <QReadWriteLock>

#include <duckdb.h>

#include <cmath>

namespace Mosaic {

namespace {

duckdb_connection connectRaw(duckdb_database db) {
    duckdb_connection conn = nullptr;
    if (!db || duckdb_connect(db, &conn) != DuckDBSuccess)
        return nullptr;
    return conn;
}

QString quoted(const QString& s) {
    QString escaped = s;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// Embeddings are stored as fp16 (768 x 2 bytes) and expanded to fp32 on read.
float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h & 0x7C00u) >> 10;
    const uint32_t man = h & 0x03FFu;
    uint32_t bits;
    if (exp == 0) {
        bits = sign;  // zero / subnormal -> 0
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);  // inf / nan
    } else {
        const int e = static_cast<int>(exp) - 15 + 127;
        bits = sign | (static_cast<uint32_t>(e) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t floatToHalf(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t man = bits & 0x007FFFFFu;
    if (exp >= 0x1F)
        return static_cast<uint16_t>(sign | 0x7C00u);  // inf
    if (exp <= 0)
        return static_cast<uint16_t>(sign);  // zero
    // Round-to-nearest-even on the dropped 13 mantissa bits.
    uint16_t out = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13));
    if ((man & 0x1000u) && ((man & 0x0FFFu) || (out & 1)))
        out = static_cast<uint16_t>(out + 1);
    return out;
}

QByteArray packHalf(const QByteArray& fp32) {
    const int count = fp32.size() / static_cast<int>(sizeof(float));
    QByteArray out;
    out.resize(count * 2);
    const float* src = reinterpret_cast<const float*>(fp32.constData());
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.data());
    for (int i = 0; i < count; ++i) dst[i] = floatToHalf(src[i]);
    return out;
}

QByteArray unpackHalf(const QByteArray& fp16) {
    const int count = fp16.size() / 2;
    QByteArray out;
    out.resize(count * static_cast<int>(sizeof(float)));
    const uint16_t* src = reinterpret_cast<const uint16_t*>(fp16.constData());
    float* dst = reinterpret_cast<float*>(out.data());
    for (int i = 0; i < count; ++i) dst[i] = halfToFloat(src[i]);
    return out;
}

QReadWriteLock s_lock;

// Nothing repeats within the last N shown posts; older history is pruned.
constexpr qint64 kNoRepeatWindow = 5000;

constexpr qint64 kMaxImpressionEvents = 2000;
constexpr qint64 kMaxInteractionEvents = 5000;

// Interaction weights.
constexpr double kWeightFavorite = 1.00;
constexpr double kWeightLike = 0.70;
constexpr double kWeightLongView = 0.20;
constexpr double kWeightNormalView = 0.05;
constexpr double kWeightSkip = -0.03;
constexpr double kWeightQuickSkip = -0.08;
constexpr double kWeightDislike = -0.80;

constexpr double kLongViewSeconds = 8.0;
constexpr double kQuickSkipSeconds = 1.5;

// Taste model constants.
constexpr double kNewTasteThreshold = 0.30;
constexpr double kTasteLearningRate = 0.25;
constexpr int kMaxTastes = 12;
constexpr int kMinTasteTags = 3;
// Tag summary is just a hint/query seed; the visual vector is the main
// representation, so prune to the strongest K tags.
constexpr int kTasteTagLimit = 10;
// Positive strength deltas shrink toward this ceiling.
constexpr double kStrengthCeiling = 12.0;
constexpr double kTasteHalfLifeDays = 120.0;
constexpr double kExposureHalfLifeDays = 7.0;
constexpr double kConfidenceRamp = 3.0;

// Negative model constants. Hard negatives are deliberately SLOW to learn:
// reaching the hard filter requires ~15+ consistent explicit dislikes.
constexpr double kHardNegativePreference = -0.5;
constexpr double kHardNegativeConfidence = 15.0;
constexpr double kDislikeDelta = -0.20;
constexpr double kSkipDelta = -0.01;
// Near-universal tags are structurally barred from vetoing the feed.
constexpr double kMinNegativeSpecificity = 0.35;
constexpr double kDislikeConfidenceDelta = 0.4;
constexpr double kFavoriteDelta = 0.10;
constexpr double kLikeDelta = 0.06;
constexpr double kViewDelta = 0.01;
constexpr double kPrefHalfLifeDays = 90.0;

// Session EMA time constant (average over ~this many recent interactions).
constexpr double kSessionTau = 12.0;

/// Scoped connection helper (duckdb_connect per call, disconnect on scope exit).
class DbConnectionGuard {
   public:
    explicit DbConnectionGuard(duckdb_database db) : m_conn(connectRaw(db)) {}
    ~DbConnectionGuard() {
        if (m_conn)
            duckdb_disconnect(&m_conn);
    }
    bool ok() const { return m_conn != nullptr; }
    duckdb_connection raw() const { return m_conn; }

    bool execute(const QString& sql, QString* error = nullptr) const {
        duckdb_result res{};
        if (!query(sql, &res, error))
            return false;
        duckdb_destroy_result(&res);
        return true;
    }
    bool query(const QString& sql, duckdb_result* result, QString* error = nullptr) const {
        *result = {};
        if (!m_conn) {
            if (error)
                *error = QStringLiteral("No feed database connection");
            return false;
        }
        const QByteArray q = sql.toUtf8();
        if (duckdb_query(m_conn, q.constData(), result) != DuckDBSuccess) {
            const char* msg = duckdb_result_error(result);
            if (error)
                *error = msg ? QString::fromUtf8(msg) : QStringLiteral("Feed query failed");
            duckdb_destroy_result(result);
            return false;
        }
        return true;
    }

   private:
    duckdb_connection m_conn;
};

void normalize(QHash<QString, double>& vec) {
    double norm = 0.0;
    for (auto it = vec.constBegin(); it != vec.constEnd(); ++it) norm += it.value() * it.value();
    norm = std::sqrt(norm);
    if (norm < 1e-9)
        return;
    for (auto it = vec.begin(); it != vec.end(); ++it) it.value() /= norm;
}

QHash<QString, double> tagVector(const QStringList& tags) {
    QHash<QString, double> vec;
    for (const QString& raw : tags) {
        const QString tag = raw.trimmed().toLower();
        if (!tag.isEmpty())
            vec[tag] += 1.0;
    }
    normalize(vec);
    return vec;
}

double decayFactor(qint64 lastTouchedMs, double halfLifeDays) {
    if (lastTouchedMs <= 0)
        return 0.0;
    const double days =
        (QDateTime::currentMSecsSinceEpoch() - static_cast<double>(lastTouchedMs)) / 86400000.0;
    return std::exp2(-days / halfLifeDays);
}

// Positive deltas shrink as the value approaches its ceiling.
double dampedGrowth(double delta, double current) {
    if (delta <= 0.0)
        return delta;
    const double headroom = 1.0 / std::cosh(current / kStrengthCeiling);
    return delta * (0.25 + 0.75 * headroom);
}

// Pull strength shrinks with accumulated evidence.
double tastePullRate(double confidence) { return kTasteLearningRate / std::sqrt(confidence + 1.0); }

void pruneTagVector(QHash<QString, double>& vec, int limit) {
    if (vec.size() <= limit)
        return;
    QList<QPair<double, QString>> ranked;
    ranked.reserve(vec.size());
    for (auto it = vec.constBegin(); it != vec.constEnd(); ++it)
        ranked.append({it.value(), it.key()});
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                  return a.first > b.first;
              });
    QHash<QString, double> pruned;
    for (int i = 0; i < qMin(limit, ranked.size()); ++i)
        pruned.insert(ranked.at(i).second, ranked.at(i).first);
    vec = pruned;
}

}  // namespace

FeedDatabase::~FeedDatabase() { close(); }

bool FeedDatabase::open(QString* error) {
    if (m_db)
        return true;

    QWriteLocker lock(&s_lock);

    Paths::ensureCreated();
    const QString file = Paths::appDataDir() + QStringLiteral("/feed.duckdb");
    const QByteArray path = file.toUtf8();

    if (duckdb_open(path.constData(), &m_db) != DuckDBSuccess) {
        m_db = nullptr;
        if (error)
            *error = QStringLiteral("Could not open %1").arg(file);
        qCritical().noquote() << "FeedDatabase: open failed for" << file;
        return false;
    }

    qDebug().noquote() << "FeedDatabase: opened" << file;
    ensureSchema();
    repairSkipPoisonedConfidence();
    loadUserModel();
    return true;
}

void FeedDatabase::close() {
    QWriteLocker lock(&s_lock);
    if (m_db) {
        // Fold the WAL back into the main file so feed.duckdb does not
        // balloon between sessions.
        {
            duckdb_connection conn = connectRaw(m_db);
            if (conn) {
                duckdb_result res{};
                if (duckdb_query(conn, "CHECKPOINT", &res) == DuckDBSuccess)
                    duckdb_destroy_result(&res);
                duckdb_disconnect(&conn);
            }
        }
        duckdb_close(&m_db);
        m_db = nullptr;
    }
}

void FeedDatabase::ensureSchema() {
    // Full current layout; no migrations - delete feed.duckdb to rebuild.
    static const char* kSchema = R"(
        CREATE SEQUENCE IF NOT EXISTS feed_event_seq;
        CREATE TABLE IF NOT EXISTS feed_events (
            id              BIGINT PRIMARY KEY DEFAULT nextval('feed_event_seq'),
            md5             VARCHAR NOT NULL,
            source_id       VARCHAR NOT NULL,
            event_type      VARCHAR NOT NULL,
            "timestamp"     TIMESTAMP NOT NULL DEFAULT now(),
            position        INTEGER,
            watch_seconds   DOUBLE
        );
        CREATE TABLE IF NOT EXISTS feed_history (
            md5 VARCHAR PRIMARY KEY,
            source_id VARCHAR NOT NULL,
            shown_at TIMESTAMP DEFAULT now()
        );
        CREATE SEQUENCE IF NOT EXISTS feed_taste_seq;
        CREATE TABLE IF NOT EXISTS feed_tastes (
            id              BIGINT PRIMARY KEY DEFAULT nextval('feed_taste_seq'),
            visual          BLOB,               -- packed float32 vector (MAIN, CLIP space)
            visual_dim      INTEGER,            -- vector dimension
            embedding       VARCHAR,            -- "tag:weight;..." fallback (tag space)
            strength        DOUBLE NOT NULL DEFAULT 0,
            confidence      DOUBLE NOT NULL DEFAULT 0,
            exposure        DOUBLE NOT NULL DEFAULT 0,
            engagement      DOUBLE NOT NULL DEFAULT 0,
            last_used       BIGINT NOT NULL DEFAULT 0,
            created_at      BIGINT NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS feed_tag_preferences (
            tag             VARCHAR PRIMARY KEY,
            preference      DOUBLE NOT NULL,
            confidence      DOUBLE NOT NULL,
            updated_at      BIGINT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS feed_tag_stats (
            tag             VARCHAR PRIMARY KEY,
            doc_count       BIGINT NOT NULL DEFAULT 0   -- impressions carrying this tag
        );
        CREATE TABLE IF NOT EXISTS feed_meta (
            key             VARCHAR PRIMARY KEY,
            value           DOUBLE NOT NULL
        );
        CREATE SEQUENCE IF NOT EXISTS feed_neg_anchor_seq;
        CREATE TABLE IF NOT EXISTS feed_negative_anchors (
            id              BIGINT PRIMARY KEY DEFAULT nextval('feed_neg_anchor_seq'),
            visual          BLOB NOT NULL,      -- packed float32 vector
            visual_dim      INTEGER NOT NULL,
            strength        DOUBLE NOT NULL DEFAULT 0,
            last_used       BIGINT NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS feed_linger_stats (
            kind VARCHAR PRIMARY KEY,
            total_ms BIGINT NOT NULL DEFAULT 0,
            samples BIGINT NOT NULL DEFAULT 0
        );
        CREATE SEQUENCE IF NOT EXISTS feed_rec_seq;
        CREATE TABLE IF NOT EXISTS feed_recommendations (
            id                  BIGINT PRIMARY KEY DEFAULT nextval('feed_rec_seq'),
            md5                 VARCHAR NOT NULL,
            generated_at        TIMESTAMP NOT NULL DEFAULT now(),
            user_similarity     DOUBLE,
            session_similarity  DOUBLE,
            tag_score           DOUBLE,
            freshness_score     DOUBLE,
            novelty_score       DOUBLE,
            exploration_score   DOUBLE,
            taste_id            BIGINT,
            taste_priority      DOUBLE,
            pre_diversity_score DOUBLE,
            final_score         DOUBLE,
            candidate_source    VARCHAR,
            rejected_negative   BOOLEAN,
            features            VARCHAR      -- packed stage-2 model features ("f0 f1 ...")
        );
        CREATE TABLE IF NOT EXISTS image_embeddings (
            md5             VARCHAR PRIMARY KEY,
            embedding       BLOB,               -- packed float32 vector
            dim             INTEGER,
            model           VARCHAR,
            model_version   VARCHAR,
            created_at      TIMESTAMP NOT NULL DEFAULT now()
        );
    )";

    DbConnectionGuard conn(m_db);
    if (!conn.ok()) {
        qWarning().noquote() << "FeedDatabase: no connection for schema setup";
        return;
    }

    QString err;
    if (!conn.execute(QString::fromUtf8(kSchema), &err))
        qCritical().noquote() << "FeedDatabase: schema creation failed:" << err;
}

void FeedDatabase::repairSkipPoisonedConfidence() {
    // One-time repair; zero tag confidence once on open (runs inside
    // open() with the write lock held - must not take it again).
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("UPDATE feed_tag_preferences SET confidence = 0"));
}

// events

QString FeedDatabase::eventName(Event e) {
    switch (e) {
        case Event::Impression:
            return QStringLiteral("impression");
        case Event::View:
            return QStringLiteral("view");
        case Event::Skip:
            return QStringLiteral("skip");
        case Event::Like:
            return QStringLiteral("like");
        case Event::Favorite:
            return QStringLiteral("favorite");
        case Event::Dislike:
            return QStringLiteral("dislike");
    }
    return QStringLiteral("unknown");
}

void FeedDatabase::recordEvent(Event kind, const QString& sourceId, const QString& md5,
                               const QStringList& tags, double watchSeconds, int position) {
    if (!m_db || md5.isEmpty())
        return;

    // Content tags only. Generic filtering is mechanical; near-universal
    // content tags are handled by self-learned specificity weighting below.
    QStringList contentTags;
    for (const QString& raw : tags) {
        const QString tag = raw.trimmed().toLower();
        if (tag.isEmpty() || FeedRecommender::isGenericTag(tag))
            continue;
        contentTags << tag;
    }

    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    const QString type = eventName(kind);
    conn.execute(QStringLiteral(
                     "INSERT INTO feed_events(md5, source_id, event_type, position, watch_seconds) "
                     "VALUES (%1, %2, %3, %4, %5)")
                     .arg(quoted(md5), quoted(sourceId), quoted(type))
                     .arg(position)
                     .arg(watchSeconds, 0, 'f', 2));

    // Shown-history for dedup.
    if (kind == Event::Impression) {
        conn.execute(QStringLiteral("INSERT INTO feed_history(md5, source_id) VALUES (%1, %2) "
                                    "ON CONFLICT (md5) DO NOTHING")
                         .arg(quoted(md5), quoted(sourceId)));
        // Document-frequency bookkeeping from impressions only - immune to
        // dislike pollution, and lets the model learn near-universal tags
        // without a hardcoded list.
        for (const QString& tag : contentTags) {
            conn.execute(
                QStringLiteral(
                    "INSERT INTO feed_tag_stats(tag, doc_count) VALUES (%1, 1) "
                    "ON CONFLICT (tag) DO UPDATE SET doc_count = feed_tag_stats.doc_count + 1")
                    .arg(quoted(tag)));
        }
        conn.execute(
            QStringLiteral("INSERT INTO feed_meta(key, value) VALUES ('total_impressions', 1) "
                           "ON CONFLICT (key) DO UPDATE SET value = feed_meta.value + 1"));
        // Prune history and raw events so the DB stays bounded.
        conn.execute(
            QStringLiteral("DELETE FROM feed_history WHERE md5 IN (SELECT md5 FROM feed_history "
                           "ORDER BY shown_at DESC OFFSET %1)")
                .arg(kNoRepeatWindow));
        // Prune raw events too: impressions dominate and are pure bookkeeping
        // (no taste signal), so keep only a bounded recent window. Non-
        // impression events are tiny but also bounded for the same reason.
        conn.execute(
            QStringLiteral("DELETE FROM feed_events WHERE event_type = 'impression' AND id IN "
                           "(SELECT id FROM feed_events WHERE event_type = 'impression' "
                           "ORDER BY id DESC OFFSET %1)")
                .arg(kMaxImpressionEvents));
        conn.execute(
            QStringLiteral("DELETE FROM feed_events WHERE event_type != 'impression' AND id IN "
                           "(SELECT id FROM feed_events WHERE event_type != 'impression' "
                           "ORDER BY id DESC OFFSET %1)")
                .arg(kMaxInteractionEvents));
    }

    // taste updates
    // Only positive events update tastes; visual-space when an embedding
    // exists, tag vector as fallback. Uses UNLOCKED helpers (write lock
    // already held, QReadWriteLock is non-recursive).
    const QByteArray visual = embeddingForUnlocked(md5);
    switch (kind) {
        case Event::Favorite:
            if (!visual.isEmpty())
                updateTastesOnPositiveVisual(visual, kWeightFavorite);
            else
                updateTastesOnPositive(contentTags, kWeightFavorite);
            break;
        case Event::Like:
            if (!visual.isEmpty())
                updateTastesOnPositiveVisual(visual, kWeightLike);
            else
                updateTastesOnPositive(contentTags, kWeightLike);
            break;
        case Event::View: {
            const double w = watchSeconds >= kLongViewSeconds ? kWeightLongView : kWeightNormalView;
            if (!visual.isEmpty())
                updateTastesOnPositiveVisual(visual, w);
            else
                updateTastesOnPositive(contentTags, w);
            break;
        }
        case Event::Dislike:
            if (!visual.isEmpty()) {
                updateTastesOnNegativeVisual(visual, 1.0);
                // Negative visual anchors: only when the dislike does not
                // strongly match an existing positive taste (otherwise it's
                // a specific instance, and tag negatives cover it).
                double bestPositiveSim = 0.0;
                for (const Taste& t : tastesUnlocked()) {
                    if (t.visual.isEmpty())
                        continue;
                    bestPositiveSim = qMax(bestPositiveSim, similarityVisual(t.visual, visual));
                }
                if (bestPositiveSim < 0.35)
                    updateNegativeAnchors(visual, 1.0);
            } else {
                updateTastesOnNegative(contentTags, 1.0);
            }
            break;
        case Event::Skip:
        case Event::Impression:
            break;
    }

    // tag preferences
    // Explicit dislikes move far more than skips; positives accumulate.
    double prefDelta = 0.0;
    switch (kind) {
        case Event::Favorite:
            prefDelta = kFavoriteDelta;
            break;
        case Event::Like:
            prefDelta = kLikeDelta;
            break;
        case Event::View:
            prefDelta = kViewDelta;
            break;
        case Event::Skip:
            prefDelta = watchSeconds < kQuickSkipSeconds ? kWeightQuickSkip : kWeightSkip;
            break;
        case Event::Dislike:
            prefDelta = kDislikeDelta;
            break;
        case Event::Impression:
            prefDelta = 0.0;
            break;
    }
    if (prefDelta != 0.0 && !contentTags.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        // Credit attribution: divide by sqrt(tag count).
        const double attribution =
            1.0 / std::sqrt(static_cast<double>(qMax(1, contentTags.size())));
        // Specificity-weighted evidence: a near-universal tag earns a tiny
        // fraction of the evidence a specific tag earns, so it can never
        // arm the hard filter.
        const QHash<QString, double> specificity = tagSpecificitiesUnlocked(conn.raw());
        auto specificityOf = [&specificity](const QString& tag) {
            auto it = specificity.constFind(tag);
            // Unseen tags: assume moderately specific.
            return it != specificity.constEnd() ? it.value() : 0.6;
        };
        const double scaledPrefDelta = prefDelta * attribution;
        // Confidence toward the hard filter grows ONLY from dislikes.
        const double confDelta =
            kind == Event::Dislike ? (kDislikeConfidenceDelta * attribution) : 0.0;
        for (const QString& tag : contentTags) {
            const double spec = specificityOf(tag);
            const double tagPrefDelta = scaledPrefDelta * spec * spec;
            const double tagConfDelta = confDelta * spec * spec;
            if (tagPrefDelta == 0.0 && tagConfDelta == 0.0)
                continue;
            // Lazy decay of the existing row, then apply the delta.
            duckdb_result res{};
            double pref = 0.0, conf = 0.0;
            qint64 updated = 0;
            if (conn.query(QStringLiteral("SELECT preference, confidence, updated_at FROM "
                                          "feed_tag_preferences WHERE tag = %1")
                               .arg(quoted(tag)),
                           &res)) {
                if (duckdb_row_count(&res) > 0) {
                    pref = duckdb_value_double(&res, 0, 0);
                    conf = duckdb_value_double(&res, 1, 0);
                    updated = duckdb_value_int64(&res, 2, 0);
                }
                duckdb_destroy_result(&res);
            }
            const double decay = updated > 0 ? decayFactor(updated, kPrefHalfLifeDays) : 1.0;
            pref *= decay;
            conf *= decay;
            pref += tagPrefDelta;
            conf += tagConfDelta;
            conn.execute(QStringLiteral(
                             "INSERT INTO feed_tag_preferences(tag, preference, confidence, "
                             "updated_at) VALUES (%1, %2, %3, %4) ON CONFLICT (tag) DO UPDATE SET "
                             "  preference = %2, confidence = %3, updated_at = %4")
                             .arg(quoted(tag))
                             .arg(pref, 0, 'f', 6)
                             .arg(conf, 0, 'f', 6)
                             .arg(nowMs));
        }
    }

    // session vector
    // Fast EMA over recent interactions.
    double sessionDelta = 0.0;
    switch (kind) {
        case Event::Favorite:
            sessionDelta = 1.0;
            break;
        case Event::Like:
            sessionDelta = 0.8;
            break;
        case Event::View:
            sessionDelta = 0.3;
            break;
        case Event::Skip:
            sessionDelta = -0.1;
            break;
        case Event::Dislike:
            sessionDelta = -1.0;
            break;
        case Event::Impression:
            sessionDelta = 0.0;
            break;
    }
    if (sessionDelta != 0.0 && !contentTags.isEmpty()) {
        const double decay = std::exp(-1.0 / kSessionTau);
        m_sessionMass *= decay;
        for (auto it = m_sessionVec.begin(); it != m_sessionVec.end(); ++it) it.value() *= decay;
        const QHash<QString, double> img = tagVector(contentTags);
        for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
            m_sessionVec[it.key()] += sessionDelta * it.value();
            m_sessionMass += sessionDelta * it.value();
        }
    }

    // stage-2 user model
    if (kind != Event::Impression) {
        double label = 0.0;
        double sampleWeight = 0.0;
        switch (kind) {
            case Event::Favorite:
                label = 1.0;
                sampleWeight = 1.0;
                break;
            case Event::Like:
                label = 1.0;
                sampleWeight = 0.7;
                break;
            case Event::View:
                label = 1.0;
                sampleWeight = watchSeconds >= kLongViewSeconds ? 0.4 : 0.15;
                break;
            case Event::Skip:
                label = 0.0;
                sampleWeight = watchSeconds < kQuickSkipSeconds ? 0.3 : 0.1;
                break;
            case Event::Dislike:
                label = 0.0;
                sampleWeight = 1.0;
                break;
            case Event::Impression:
                break;  // unreachable (guarded above)
        }
        if (sampleWeight > 0.0) {
            const QString featuresStr = rankedFeaturesForUnlocked(conn.raw(), md5);
            if (!featuresStr.isEmpty()) {
                double features[UserModel::kFeatureCount] = {};
                bool ok = true;
                int idx = 0;
                for (const QString& tok : featuresStr.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    if (idx >= UserModel::kFeatureCount) {
                        ok = false;
                        break;
                    }
                    bool numOk = false;
                    const double v = tok.toDouble(&numOk);
                    if (!numOk) {
                        ok = false;
                        break;
                    }
                    features[idx++] = v;
                }
                if (ok && idx == UserModel::kFeatureCount)
                    m_userModel.train(features, label, sampleWeight);
            }
        }
    }
    saveUserModelUnlocked();
}

bool FeedDatabase::wasShown(const QString& md5) const {
    if (!m_db || md5.isEmpty())
        return false;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return false;

    duckdb_result res{};
    if (!conn.query(
            QStringLiteral("SELECT 1 FROM (SELECT md5 FROM feed_history ORDER BY shown_at DESC "
                           "LIMIT %1) WHERE md5 = %2")
                .arg(kNoRepeatWindow)
                .arg(quoted(md5)),
            &res))
        return false;
    const bool shown = duckdb_row_count(&res) > 0;
    duckdb_destroy_result(&res);
    return shown;
}

// tastes

QList<FeedDatabase::Taste> FeedDatabase::tastes() const {
    QReadLocker lock(&s_lock);
    return tastesUnlocked();
}

QList<FeedDatabase::Taste> FeedDatabase::tastesUnlocked() const {
    QList<Taste> out;
    if (!m_db)
        return out;

    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT id, visual, embedding, strength, confidence, exposure, "
                                   "engagement, last_used, created_at FROM feed_tastes"),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        Taste t;
        t.id = duckdb_value_int64(&res, 0, row);
        duckdb_blob visBlob = duckdb_value_blob(&res, 1, row);
        if (visBlob.data && visBlob.size > 0)
            t.visual =
                QByteArray(static_cast<const char*>(visBlob.data), static_cast<int>(visBlob.size));
        char* emb = duckdb_value_varchar(&res, 2, row);
        if (emb) {
            const QString blob = QString::fromUtf8(emb);
            duckdb_free(emb);
            const QStringList pairs = blob.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString& pair : pairs) {
                const int colon = pair.indexOf(QLatin1Char(':'));
                if (colon <= 0)
                    continue;
                t.embedding[pair.left(colon)] = pair.mid(colon + 1).toDouble();
            }
        }
        t.strength = duckdb_value_double(&res, 3, row);
        t.confidence = duckdb_value_double(&res, 4, row);
        t.exposure = duckdb_value_double(&res, 5, row);
        t.engagement = duckdb_value_double(&res, 6, row);
        t.lastUsedMs = duckdb_value_int64(&res, 7, row);
        t.createdAtMs = duckdb_value_int64(&res, 8, row);
        out.append(t);
    }
    duckdb_destroy_result(&res);

    std::sort(out.begin(), out.end(),
              [](const Taste& a, const Taste& b) { return a.strength > b.strength; });
    return out;
}

double FeedDatabase::similarity(const QHash<QString, double>& tasteEmbedding,
                                const QStringList& tags) {
    if (tasteEmbedding.isEmpty() || tags.isEmpty())
        return 0.0;
    const QHash<QString, double> img = tagVector(tags);
    double dot = 0.0;
    for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
        auto tit = tasteEmbedding.constFind(it.key());
        if (tit != tasteEmbedding.constEnd())
            dot += it.value() * tit.value();
    }
    return dot;  // both vectors are unit length
}

double FeedDatabase::similarityVisual(const QByteArray& tasteVisual,
                                      const QByteArray& imageVisual) {
    return EmbeddingBackend::cosineSimilarity(tasteVisual, imageVisual);
}

void FeedDatabase::updateTastesOnPositiveVisual(const QByteArray& imageVisual, double weight) {
    if (!m_db || imageVisual.isEmpty() || weight <= 0.0)
        return;

    QList<Taste> all = tastesUnlocked();

    qint64 bestId = 0;
    double bestSim = 0.0;
    for (const Taste& t : all) {
        if (t.visual.isEmpty())
            continue;
        const double sim = similarityVisual(t.visual, imageVisual);
        if (sim > bestSim) {
            bestSim = sim;
            bestId = t.id;
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (bestId == 0 || bestSim < kNewTasteThreshold) {
        if (all.size() >= kMaxTastes) {
            const Taste& weakest = all.last();
            if (weakest.strength > weight)
                return;
            bestId = weakest.id;
        } else {
            createTasteVisual(imageVisual, weight, nowMs);
            return;
        }
    }

    // taste = normalize(taste + lr * weight * image)
    QByteArray bestVisual;
    double strength = 0.0, confidence = 0.0, engagement = 0.0;
    for (const Taste& t : all) {
        if (t.id == bestId) {
            bestVisual = t.visual;
            strength = t.strength;
            confidence = t.confidence;
            engagement = t.engagement;
            break;
        }
    }
    if (bestVisual.isEmpty()) {
        createTasteVisual(imageVisual, weight, nowMs);
        return;
    }

    const int dim = bestVisual.size() / static_cast<int>(sizeof(float));
    const int imgDim = imageVisual.size() / static_cast<int>(sizeof(float));
    if (dim != imgDim || dim <= 0)
        return;  // model changed

    const float* imgPtr = reinterpret_cast<const float*>(imageVisual.constData());
    float* tastePtr = reinterpret_cast<float*>(bestVisual.data());
    const double lr = tastePullRate(confidence);
    double norm = 0.0;
    for (int i = 0; i < dim; ++i) {
        tastePtr[i] += static_cast<float>(lr * weight * imgPtr[i]);
        norm += double(tastePtr[i]) * tastePtr[i];
    }
    norm = std::sqrt(norm);
    if (norm > 1e-9) {
        for (int i = 0; i < dim; ++i) tastePtr[i] = static_cast<float>(tastePtr[i] / norm);
    }

    // Strength grows gradually, damped by confidence ramp and ceiling.
    const double ramp = confidence / (confidence + kConfidenceRamp);
    strength += dampedGrowth(weight * (0.3 + 0.7 * ramp), strength);
    confidence += weight;
    engagement = engagement * 0.9 + weight * 0.1;

    writeTasteVisual(bestId, bestVisual, strength, confidence, engagement, nowMs);
}

void FeedDatabase::updateTastesOnNegativeVisual(const QByteArray& imageVisual, double weight) {
    if (!m_db || imageVisual.isEmpty())
        return;

    for (const Taste& t : tastesUnlocked()) {
        if (t.visual.isEmpty())
            continue;
        const double sim = similarityVisual(t.visual, imageVisual);
        if (sim > 0.2) {
            const double newStrength = qMax(0.0, t.strength - weight * sim * 0.5);
            writeTasteStrength(t.id, newStrength);
        }
    }
}

void FeedDatabase::updateTastesOnPositive(const QStringList& tags, double weight) {
    if (!m_db || tags.isEmpty() || weight <= 0.0)
        return;

    QList<Taste> all = tastesUnlocked();
    if (contentTagsTooFew(tags))
        return;

    qint64 bestId = 0;
    double bestSim = 0.0;
    for (const Taste& t : all) {
        const double sim = similarity(t.embedding, tags);
        if (sim > bestSim) {
            bestSim = sim;
            bestId = t.id;
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (bestId == 0 || bestSim < kNewTasteThreshold) {
        if (all.size() >= kMaxTastes) {
            const Taste& weakest = all.last();
            if (weakest.strength > weight)
                return;  // keep existing tastes; this signal is too weak
            bestId = weakest.id;
        } else {
            createTaste(tags, weight, nowMs);
            return;
        }
    }

    // Pull the taste toward the image; mature tastes move slowly.
    const QHash<QString, double> img = tagVector(tags);
    QHash<QString, double> emb;
    double strength = 0.0, confidence = 0.0, engagement = 0.0;
    for (const Taste& t : all) {
        if (t.id == bestId) {
            emb = t.embedding;
            strength = t.strength;
            confidence = t.confidence;
            engagement = t.engagement;
            break;
        }
    }
    const double lr = tastePullRate(confidence);
    for (auto it = img.constBegin(); it != img.constEnd(); ++it)
        emb[it.key()] += lr * weight * it.value();
    normalize(emb);
    pruneTagVector(emb, kTasteTagLimit);

    // Strength grows gradually, damped by confidence ramp and ceiling.
    const double ramp = confidence / (confidence + kConfidenceRamp);
    strength += dampedGrowth(weight * (0.3 + 0.7 * ramp), strength);
    confidence += weight;
    engagement = engagement * 0.9 + weight * 0.1;

    writeTaste(bestId, emb, strength, confidence, engagement, nowMs);
}

void FeedDatabase::updateTastesOnNegative(const QStringList& tags, double weight) {
    if (!m_db || tags.isEmpty())
        return;

    for (const Taste& t : tastesUnlocked()) {
        const double sim = FeedDatabase::similarity(t.embedding, tags);
        if (sim > 0.2) {
            const double newStrength = qMax(0.0, t.strength - weight * sim * 0.5);
            writeTasteStrength(t.id, newStrength);
        }
    }
}

void FeedDatabase::recordTasteExposure(qint64 tasteId, double amount) {
    if (!m_db || tasteId <= 0)
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("UPDATE feed_tastes SET exposure = exposure + %2 WHERE id = %1")
                     .arg(tasteId)
                     .arg(amount, 0, 'f', 4));
}

void FeedDatabase::recordTasteEngagement(qint64 tasteId, double weight) {
    if (!m_db || tasteId <= 0)
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("UPDATE feed_tastes SET engagement = engagement * 0.9 + %2 * 0.1 "
                                "WHERE id = %1")
                     .arg(tasteId)
                     .arg(weight, 0, 'f', 4));
}

void FeedDatabase::decayTastes() {
    if (!m_db)
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT id, strength, confidence, exposure, engagement, "
                                   "last_used FROM feed_tastes"),
                    &res))
        return;
    struct Row {
        qint64 id;
        double strength, confidence, exposure, engagement;
        qint64 lastUsed;
    };
    QList<Row> rows;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        rows.append({duckdb_value_int64(&res, 0, row), duckdb_value_double(&res, 1, row),
                     duckdb_value_double(&res, 2, row), duckdb_value_double(&res, 3, row),
                     duckdb_value_double(&res, 4, row), duckdb_value_int64(&res, 5, row)});
    }
    duckdb_destroy_result(&res);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const Row& r : rows) {
        const double strengthDecay =
            r.lastUsed > 0 ? decayFactor(r.lastUsed, kTasteHalfLifeDays) : 1.0;
        const double exposureDecay =
            r.lastUsed > 0 ? decayFactor(r.lastUsed, kExposureHalfLifeDays) : 1.0;
        conn.execute(
            QStringLiteral("UPDATE feed_tastes SET strength = %2, confidence = %3, exposure = %4, "
                           "engagement = %5 WHERE id = %1")
                .arg(r.id)
                .arg(r.strength * strengthDecay, 0, 'f', 6)
                .arg(r.confidence * strengthDecay, 0, 'f', 6)
                .arg(r.exposure * exposureDecay, 0, 'f', 6)
                .arg(r.engagement * exposureDecay, 0, 'f', 6));
    }
    Q_UNUSED(nowMs);
}

// negative visual anchors

QList<FeedDatabase::NegativeAnchor> FeedDatabase::negativeAnchors() const {
    QList<NegativeAnchor> out;
    if (!m_db)
        return out;
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;
    duckdb_result res{};
    if (!conn.query(
            QStringLiteral("SELECT id, visual, strength, last_used FROM feed_negative_anchors"),
            &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        NegativeAnchor a;
        a.id = duckdb_value_int64(&res, 0, row);
        duckdb_blob blob = duckdb_value_blob(&res, 1, row);
        if (blob.data && blob.size > 0)
            a.visual = QByteArray(static_cast<const char*>(blob.data), static_cast<int>(blob.size));
        a.strength = duckdb_value_double(&res, 2, row);
        a.lastUsedMs = duckdb_value_int64(&res, 3, row);
        out.append(a);
    }
    duckdb_destroy_result(&res);
    return out;
}

void FeedDatabase::updateNegativeAnchors(const QByteArray& imageVisual, double weight) {
    if (!m_db || imageVisual.isEmpty())
        return;
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    duckdb_result res{};
    qint64 bestId = 0;
    double bestSim = 0.0;
    if (conn.query(QStringLiteral("SELECT id, visual, strength FROM feed_negative_anchors"),
                   &res)) {
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
            duckdb_blob blob = duckdb_value_blob(&res, 1, row);
            if (!blob.data || blob.size == 0)
                continue;
            const QByteArray vec(static_cast<const char*>(blob.data), static_cast<int>(blob.size));
            const double sim = similarityVisual(vec, imageVisual);
            if (sim > bestSim) {
                bestSim = sim;
                bestId = duckdb_value_int64(&res, 0, row);
            }
        }
        duckdb_destroy_result(&res);
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr double kAnchorFitThreshold = 0.55;
    constexpr int kMaxAnchors = 8;

    if (bestId != 0 && bestSim >= kAnchorFitThreshold) {
        conn.execute(QStringLiteral("UPDATE feed_negative_anchors SET strength = strength + %2, "
                                    "last_used = %3 WHERE id = %1")
                         .arg(bestId)
                         .arg(weight, 0, 'f', 6)
                         .arg(nowMs));
        return;
    }

    // Once full, replace the weakest anchor when the new evidence is not
    // a one-off.
    duckdb_result countRes{};
    qint64 count = 0;
    if (conn.query(QStringLiteral("SELECT count(*) FROM feed_negative_anchors"), &countRes)) {
        if (duckdb_row_count(&countRes) > 0)
            count = duckdb_value_int64(&countRes, 0, 0);
        duckdb_destroy_result(&countRes);
    }
    if (count >= kMaxAnchors) {
        duckdb_result weakRes{};
        if (conn.query(
                QStringLiteral(
                    "SELECT id, strength FROM feed_negative_anchors ORDER BY strength ASC LIMIT 1"),
                &weakRes)) {
            if (duckdb_row_count(&weakRes) > 0) {
                const qint64 weakId = duckdb_value_int64(&weakRes, 0, 0);
                const double weakStrength = duckdb_value_double(&weakRes, 1, 0);
                duckdb_destroy_result(&weakRes);
                if (weight < weakStrength)
                    return;
                conn.execute(
                    QStringLiteral("DELETE FROM feed_negative_anchors WHERE id = %1").arg(weakId));
            } else {
                duckdb_destroy_result(&weakRes);
            }
        }
    }

    const QString hex = QString::fromLatin1(imageVisual.toHex());
    conn.execute(QStringLiteral("INSERT INTO feed_negative_anchors(visual, visual_dim, strength, "
                                "last_used) VALUES (unhex('%1'), %2, %3, %4)")
                     .arg(hex)
                     .arg(imageVisual.size() / static_cast<int>(sizeof(float)))
                     .arg(weight, 0, 'f', 6)
                     .arg(nowMs));
}

double FeedDatabase::maxNegativeAnchorSimilarity(const QByteArray& imageVisual) const {
    if (imageVisual.isEmpty())
        return 0.0;
    double best = 0.0;
    for (const NegativeAnchor& a : negativeAnchors()) {
        if (a.visual.isEmpty())
            continue;
        best = qMax(best, similarityVisual(a.visual, imageVisual));
    }
    return best;
}

// tag specificity

void FeedDatabase::recordTagDocFrequency(const QStringList& tags) {
    // Kept for API symmetry / potential external callers; recordEvent()
    // already does this inline for impressions to avoid a second
    // lock+connect round trip.
    if (!m_db || tags.isEmpty())
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    for (const QString& raw : tags) {
        const QString tag = raw.trimmed().toLower();
        if (tag.isEmpty())
            continue;
        conn.execute(QStringLiteral(
                         "INSERT INTO feed_tag_stats(tag, doc_count) VALUES (%1, 1) "
                         "ON CONFLICT (tag) DO UPDATE SET doc_count = feed_tag_stats.doc_count + 1")
                         .arg(quoted(tag)));
    }
    conn.execute(
        QStringLiteral("INSERT INTO feed_meta(key, value) VALUES ('total_impressions', 1) "
                       "ON CONFLICT (key) DO UPDATE SET value = feed_meta.value + 1"));
}

double FeedDatabase::totalImpressions() const {
    if (!m_db)
        return 0.0;
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return 0.0;
    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT value FROM feed_meta WHERE key = 'total_impressions'"),
                    &res))
        return 0.0;
    double v = 0.0;
    if (duckdb_row_count(&res) > 0)
        v = duckdb_value_double(&res, 0, 0);
    duckdb_destroy_result(&res);
    return v;
}

QHash<QString, double> FeedDatabase::tagSpecificities() const {
    QHash<QString, double> out;
    if (!m_db)
        return out;
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;
    return tagSpecificitiesUnlocked(conn.raw());
}

QHash<QString, double> FeedDatabase::tagSpecificitiesUnlocked(duckdb_connection conn) const {
    QHash<QString, double> out;
    if (!conn)
        return out;

    double total = 0.0;
    {
        duckdb_result res{};
        if (duckdb_query(conn,
                         QStringLiteral("SELECT value FROM feed_meta WHERE key = "
                                        "'total_impressions'")
                             .toUtf8()
                             .constData(),
                         &res) == DuckDBSuccess) {
            if (duckdb_row_count(&res) > 0)
                total = duckdb_value_double(&res, 0, 0);
            duckdb_destroy_result(&res);
        }
    }
    if (total < 1.0)
        return out;  // not enough data yet: caller should treat as "unknown"

    // Minimum impressions before a tag's frequency is trusted; below this,
    // the tag is simply omitted (caller defaults to a neutral value) so a
    // brand-new/rare tag isn't punished for having only 1-2 samples.
    constexpr double kMinDocSamples = 5.0;

    duckdb_result res{};
    if (duckdb_query(
            conn, QStringLiteral("SELECT tag, doc_count FROM feed_tag_stats").toUtf8().constData(),
            &res) != DuckDBSuccess)
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        char* tagPtr = duckdb_value_varchar(&res, 0, row);
        if (!tagPtr)
            continue;
        const QString tag = QString::fromUtf8(tagPtr);
        duckdb_free(tagPtr);
        const double docCount = static_cast<double>(duckdb_value_int64(&res, 1, row));
        if (docCount < kMinDocSamples)
            continue;
        // Classic IDF, squashed to [0,1]: a tag on 100% of impressions ->
        // 0 specificity; a tag on a tiny fraction -> close to 1. log-based
        // so the curve is generous in the middle (most tags aren't at
        // either extreme) instead of collapsing everything below 50%.
        const double freq = qBound(1e-6, docCount / total, 1.0);
        const double idf = -std::log2(freq);
        // Normalize against a reasonable ceiling (a tag seen once in 1000
        // impressions maxes out at idf ~10) instead of letting rare tags
        // blow past 1.0 and dominate everything.
        out.insert(tag, qBound(0.0, idf / 10.0, 1.0));
    }
    duckdb_destroy_result(&res);
    return out;
}

// learned user model

void FeedDatabase::loadUserModel() {
    // NOTE: called from open() which already holds the write lock.
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    // Tiny side table: feed_meta.value is DOUBLE and cannot hold the
    // serialized model text.
    conn.execute(
        QStringLiteral("CREATE TABLE IF NOT EXISTS feed_user_model ("
                       "id INTEGER PRIMARY KEY DEFAULT 0, blob VARCHAR)"));
    duckdb_result res{};
    if (conn.query(QStringLiteral("SELECT blob FROM feed_user_model WHERE id = 0"), &res)) {
        if (duckdb_row_count(&res) > 0) {
            char* txt = duckdb_value_varchar(&res, 0, 0);
            if (txt) {
                m_userModel.deserialize(QByteArray(txt));
                duckdb_free(txt);
            }
        }
        duckdb_destroy_result(&res);
    }
}

void FeedDatabase::saveUserModelUnlocked() const {
    // NOTE: caller must hold the write lock (QReadWriteLock is
    // non-recursive - never take it again here).
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    const QByteArray blob = m_userModel.serialize();
    conn.execute(QStringLiteral("INSERT INTO feed_user_model(id, blob) VALUES (0, %1) "
                                "ON CONFLICT (id) DO UPDATE SET blob = %1")
                     .arg(quoted(QString::fromUtf8(blob))));
}

void FeedDatabase::trainUserModel(const double* features, double label, double sampleWeight) {
    if (!m_db)
        return;
    QWriteLocker lock(&s_lock);
    m_userModel.train(features, label, sampleWeight);
    saveUserModelUnlocked();
}

double FeedDatabase::userModelPredict(const double* features) const {
    QReadLocker lock(&s_lock);
    return m_userModel.predict(features);
}

double FeedDatabase::userModelBlend() const {
    QReadLocker lock(&s_lock);
    return m_userModel.blendWeight();
}

double FeedDatabase::userModelSamples() const {
    QReadLocker lock(&s_lock);
    return m_userModel.samples();
}

QList<double> FeedDatabase::userModelWeights() const {
    QReadLocker lock(&s_lock);
    QList<double> out;
    out.reserve(UserModel::kFeatureCount);
    for (int i = 0; i < UserModel::kFeatureCount; ++i) out.append(m_userModel.weight(i));
    return out;
}

// negative preferences

QHash<QString, double> FeedDatabase::tagPreferences() const {
    QHash<QString, double> out;
    const auto rows = tagPreferenceRows();
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it)
        out.insert(it.key(), it.value().preference);
    return out;
}

QHash<QString, FeedDatabase::TagPreference> FeedDatabase::tagPreferenceRows() const {
    QHash<QString, TagPreference> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT tag, preference, confidence, updated_at FROM "
                                   "feed_tag_preferences"),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        TagPreference p;
        char* tag = duckdb_value_varchar(&res, 0, row);
        if (!tag)
            continue;
        p.tag = QString::fromUtf8(tag);
        duckdb_free(tag);
        p.preference = duckdb_value_double(&res, 1, row);
        p.confidence = duckdb_value_double(&res, 2, row);
        p.updatedAtMs = duckdb_value_int64(&res, 3, row);
        out.insert(p.tag, p);
    }
    duckdb_destroy_result(&res);
    return out;
}

QStringList FeedDatabase::hardNegativeTags() const {
    QStringList out;
    const auto rows = tagPreferenceRows();
    // Specificity gate: a near-universal tag (female, mammal, anthro...)
    // can NEVER become a hard negative, no matter how much negative
    // evidence lands on it - vetoing it would empty the catalog. Only a
    // tag that actually narrows the candidate space may arm the veto.
    const QHash<QString, double> specificity = tagSpecificities();
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        // Generic tags must NEVER become hard negatives
        if (FeedRecommender::isGenericTag(it.key()))
            continue;
        const auto sit = specificity.constFind(it.key());
        if (sit != specificity.constEnd() && sit.value() < kMinNegativeSpecificity)
            continue;
        // Hard filter requires BOTH a strong negative preference AND enough
        // confidence (evidence mass) - one accidental dislike never
        // blacklists a tag.
        if (it.value().preference <= kHardNegativePreference &&
            it.value().confidence >= kHardNegativeConfidence)
            out << it.key();
    }
    return out;
}

QStringList FeedDatabase::negativeQueryTags(int limit) const {
    QStringList out;
    const auto rows = tagPreferenceRows();
    const QHash<QString, double> specificity = tagSpecificities();
    QList<QPair<double, QString>> ranked;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        // Exclude generic tags from search exclusions: blocking "anthro" or "clothing"
        // in query strings would kill the entire catalog.
        if (FeedRecommender::isGenericTag(it.key()))
            continue;
        // Same specificity gate as the hard filter: a near-universal tag
        // must never become a "-tag" query exclusion either.
        const auto sit = specificity.constFind(it.key());
        if (sit != specificity.constEnd() && sit.value() < kMinNegativeSpecificity)
            continue;
        if (it.value().preference < -0.4 && it.value().confidence >= 3.0) {
            ranked.append({it.value().preference * it.value().confidence, it.key()});
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<double, QString>& a, const QPair<double, QString>& b) {
                  return a.first < b.first;
              });
    for (int i = 0; i < qMin(limit, ranked.size()); ++i) {
        out << ranked.at(i).second;
    }
    return out;
}

// session model

QHash<QString, double> FeedDatabase::sessionWeights() const { return m_sessionVec; }

double FeedDatabase::sessionSimilarity(const QStringList& tags) const {
    if (m_sessionVec.isEmpty() || tags.isEmpty())
        return 0.0;
    const QHash<QString, double> img = tagVector(tags);
    double dot = 0.0, imgNorm = 0.0, sesNorm = 0.0;
    for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
        imgNorm += it.value() * it.value();
        auto sit = m_sessionVec.constFind(it.key());
        if (sit != m_sessionVec.constEnd())
            dot += it.value() * sit.value();
    }
    for (auto it = m_sessionVec.constBegin(); it != m_sessionVec.constEnd(); ++it)
        sesNorm += it.value() * it.value();
    imgNorm = std::sqrt(imgNorm);
    sesNorm = std::sqrt(sesNorm);
    if (imgNorm < 1e-9 || sesNorm < 1e-9)
        return 0.0;
    return dot / (imgNorm * sesNorm);
}

// linger

void FeedDatabase::recordLinger(const QString& kind, qint64 durationMs) {
    if (!m_db || kind.isEmpty() || durationMs <= 0)
        return;

    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    conn.execute(QStringLiteral("INSERT INTO feed_linger_stats(kind, total_ms, samples) "
                                "VALUES (%1, %2, 1) "
                                "ON CONFLICT (kind) DO UPDATE SET "
                                "  total_ms = feed_linger_stats.total_ms + %2, "
                                "  samples = feed_linger_stats.samples + 1")
                     .arg(quoted(kind))
                     .arg(durationMs));
}

QHash<QString, qint64> FeedDatabase::averageLingers() const {
    QHash<QString, qint64> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT kind, total_ms, samples FROM feed_linger_stats "
                                   "WHERE samples > 0"),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        char* kindValue = duckdb_value_varchar(&res, 0, row);
        if (!kindValue)
            continue;
        const QString kind = QString::fromUtf8(kindValue);
        duckdb_free(kindValue);
        const qint64 totalMs = duckdb_value_int64(&res, 1, row);
        const qint64 samples = duckdb_value_int64(&res, 2, row);
        out.insert(kind, totalMs / qMax<qint64>(samples, 1));
    }
    duckdb_destroy_result(&res);
    return out;
}

QHash<QString, int> FeedDatabase::interactionCounts() const {
    QHash<QString, int> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(
            QStringLiteral("SELECT event_type, count(*) FROM feed_events GROUP BY event_type"),
            &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        char* kindValue = duckdb_value_varchar(&res, 0, row);
        if (!kindValue)
            continue;
        const QString kind = QString::fromUtf8(kindValue);
        duckdb_free(kindValue);
        out.insert(kind, static_cast<int>(duckdb_value_int64(&res, 1, row)));
    }
    duckdb_destroy_result(&res);
    return out;
}

// stats

int FeedDatabase::totalShown() const {
    if (!m_db)
        return 0;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return 0;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT count(*) FROM feed_history"), &res))
        return 0;
    qint64 count = 0;
    if (duckdb_row_count(&res) > 0)
        count = duckdb_value_int64(&res, 0, 0);
    duckdb_destroy_result(&res);
    return static_cast<int>(count);
}

int FeedDatabase::recentSkipStreak() const {
    if (!m_db)
        return 0;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return 0;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT event_type FROM feed_events "
                                   "WHERE event_type != 'impression' "
                                   "ORDER BY \"timestamp\" DESC LIMIT 20"),
                    &res))
        return 0;
    int streak = 0;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        char* typeValue = duckdb_value_varchar(&res, 0, row);
        const QString type = typeValue ? QString::fromUtf8(typeValue) : QString();
        duckdb_free(typeValue);
        if (type == QLatin1String("skip") || type == QLatin1String("dislike"))
            ++streak;
        else
            break;  // streak broken by any non-skip interaction
    }
    duckdb_destroy_result(&res);
    return streak;
}

QList<QPair<QString, QString>> FeedDatabase::recentInteractions(int limit) const {
    QList<QPair<QString, QString>> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT event_type, md5 FROM feed_events "
                                   "WHERE event_type != 'impression' "
                                   "ORDER BY \"timestamp\" DESC LIMIT %1")
                        .arg(limit),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        char* typeValue = duckdb_value_varchar(&res, 0, row);
        char* md5Value = duckdb_value_varchar(&res, 1, row);
        const QString type = typeValue ? QString::fromUtf8(typeValue) : QString();
        const QString md5 = md5Value ? QString::fromUtf8(md5Value) : QString();
        duckdb_free(typeValue);
        duckdb_free(md5Value);
        out.append({type, md5});
    }
    duckdb_destroy_result(&res);
    return out;
}

// recommendation debug

void FeedDatabase::recordRecommendations(const QList<RecommendationRow>& rows) {
    if (!m_db || rows.isEmpty())
        return;

    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    for (const RecommendationRow& r : rows) {
        conn.execute(
            QStringLiteral("INSERT INTO feed_recommendations(md5, user_similarity, "
                           "session_similarity, tag_score, freshness_score, novelty_score, "
                           "exploration_score, taste_id, taste_priority, pre_diversity_score, "
                           "final_score, candidate_source, rejected_negative, features) "
                           "VALUES (%1, %2, %3, %4, %5, %6, %7, %8, %9, %10, %11, %12, %13, %14)")
                .arg(quoted(r.md5))
                .arg(r.userSimilarity, 0, 'f', 4)
                .arg(r.sessionSimilarity, 0, 'f', 4)
                .arg(r.tagScore, 0, 'f', 4)
                .arg(r.freshnessScore, 0, 'f', 4)
                .arg(r.noveltyScore, 0, 'f', 4)
                .arg(r.explorationScore, 0, 'f', 4)
                .arg(r.tasteId)
                .arg(r.tastePriority, 0, 'f', 4)
                .arg(r.preDiversityScore, 0, 'f', 4)
                .arg(r.finalScore, 0, 'f', 4)
                .arg(quoted(r.candidateSource))
                .arg(r.rejectedNegative ? QStringLiteral("TRUE") : QStringLiteral("FALSE"))
                .arg(quoted(r.features)));
    }
    // Keep the debug table bounded: only the most recent rows matter.
    conn.execute(QStringLiteral(
        "DELETE FROM feed_recommendations WHERE id IN "
        "(SELECT id FROM feed_recommendations ORDER BY generated_at DESC OFFSET 2000)"));
}

QList<FeedDatabase::RecommendationRow> FeedDatabase::recentRecommendations(int limit) const {
    QList<RecommendationRow> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    duckdb_result res{};
    if (!conn.query(
            QStringLiteral("SELECT md5, user_similarity, session_similarity, tag_score, "
                           "freshness_score, novelty_score, exploration_score, taste_id, "
                           "taste_priority, pre_diversity_score, final_score, candidate_source, "
                           "rejected_negative FROM feed_recommendations "
                           "ORDER BY generated_at DESC LIMIT %1")
                .arg(limit),
            &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        RecommendationRow r;
        char* md5 = duckdb_value_varchar(&res, 0, row);
        if (md5) {
            r.md5 = QString::fromUtf8(md5);
            duckdb_free(md5);
        }
        r.userSimilarity = duckdb_value_double(&res, 1, row);
        r.sessionSimilarity = duckdb_value_double(&res, 2, row);
        r.tagScore = duckdb_value_double(&res, 3, row);
        r.freshnessScore = duckdb_value_double(&res, 4, row);
        r.noveltyScore = duckdb_value_double(&res, 5, row);
        r.explorationScore = duckdb_value_double(&res, 6, row);
        r.tasteId = duckdb_value_int64(&res, 7, row);
        r.tastePriority = duckdb_value_double(&res, 8, row);
        r.preDiversityScore = duckdb_value_double(&res, 9, row);
        r.finalScore = duckdb_value_double(&res, 10, row);
        char* src = duckdb_value_varchar(&res, 11, row);
        if (src) {
            r.candidateSource = QString::fromUtf8(src);
            duckdb_free(src);
        }
        r.rejectedNegative = duckdb_value_boolean(&res, 12, row);
        out.append(r);
    }
    duckdb_destroy_result(&res);
    return out;
}

QString FeedDatabase::rankedFeaturesFor(const QString& md5) const {
    if (!m_db || md5.isEmpty())
        return {};
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return {};
    return rankedFeaturesForUnlocked(conn.raw(), md5);
}

QString FeedDatabase::rankedFeaturesForUnlocked(duckdb_connection conn, const QString& md5) const {
    if (!conn || md5.isEmpty())
        return {};
    duckdb_result res{};
    QString out;
    if (duckdb_query(conn,
                     QStringLiteral("SELECT features FROM feed_recommendations WHERE md5 = %1 "
                                    "ORDER BY generated_at DESC LIMIT 1")
                         .arg(quoted(md5))
                         .toUtf8()
                         .constData(),
                     &res) == DuckDBSuccess) {
        if (duckdb_row_count(&res) > 0) {
            char* txt = duckdb_value_varchar(&res, 0, 0);
            if (txt) {
                out = QString::fromUtf8(txt);
                duckdb_free(txt);
            }
        }
        duckdb_destroy_result(&res);
    }
    return out;
}

// image embeddings

QByteArray FeedDatabase::embeddingFor(const QString& md5, const QString& model,
                                      const QString& modelVersion) const {
    QReadLocker lock(&s_lock);
    return embeddingForUnlocked(md5, model, modelVersion);
}

QByteArray FeedDatabase::embeddingForUnlocked(const QString& md5, const QString& model,
                                              const QString& modelVersion) const {
    if (!m_db || md5.isEmpty())
        return {};

    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return {};

    // When a model/version filter is given, an embedding produced by a
    // different model is treated as missing - the caller will
    // regenerate it rather than mixing incompatible vector spaces.
    QString sql =
        QStringLiteral("SELECT embedding FROM image_embeddings WHERE md5 = %1").arg(quoted(md5));
    if (!model.isEmpty())
        sql += QStringLiteral(" AND model = %1").arg(quoted(model));
    if (!modelVersion.isEmpty())
        sql += QStringLiteral(" AND model_version = %1").arg(quoted(modelVersion));

    duckdb_result res{};
    if (!conn.query(sql, &res))
        return {};
    QByteArray out;
    if (duckdb_row_count(&res) > 0) {
        duckdb_blob blob = duckdb_value_blob(&res, 0, 0);
        if (blob.data && blob.size > 0) {
            // Stored as fp16 (half the disk); expand to the packed fp32
            // representation every consumer expects.
            out = unpackHalf(
                QByteArray(static_cast<const char*>(blob.data), static_cast<int>(blob.size)));
        }
    }
    duckdb_destroy_result(&res);
    return out;
}

void FeedDatabase::storeImageEmbedding(const QString& md5, const QByteArray& packed,
                                       const QString& model, const QString& modelVersion) {
    if (!m_db || md5.isEmpty() || packed.isEmpty())
        return;

    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;

    // Stored as fp16: 768 dims = 1536 bytes/image instead of 3072.
    // Expanded back to fp32 on read.
    const QByteArray half = packHalf(packed);
    const QString hex = QString::fromLatin1(half.toHex());
    conn.execute(
        QStringLiteral("INSERT INTO image_embeddings(md5, embedding, dim, model, model_version) "
                       "VALUES (%1, unhex('%2'), %3, %4, %5) "
                       "ON CONFLICT (md5) DO UPDATE SET embedding = unhex('%2'), dim = %3, "
                       "model = %4, model_version = %5, created_at = now()")
            .arg(quoted(md5))
            .arg(hex)
            .arg(packed.size() / static_cast<int>(sizeof(float)))
            .arg(quoted(model))
            .arg(quoted(modelVersion)));
}

// tuning

void FeedDatabase::reset(QString* error) {
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok()) {
        if (error)
            *error = QStringLiteral("Feed database is not open");
        return;
    }
    QString dbError;
    conn.execute(QStringLiteral("DELETE FROM feed_events"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_history"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_tastes"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_tag_preferences"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_linger_stats"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_recommendations"), &dbError);
    conn.execute(QStringLiteral("DELETE FROM feed_negative_anchors"), &dbError);
    // Tag doc-frequency (feed_tag_stats/feed_meta) is catalog knowledge,
    // not user preference - a profile reset does NOT wipe it; specificity
    // stays learned across resets.
    m_sessionVec.clear();
    m_sessionMass = 0.0;
    m_userModel.reset();
    saveUserModelUnlocked();
}

// taste helpers

bool FeedDatabase::contentTagsTooFew(const QStringList& tags) const {
    return tags.size() < kMinTasteTags;
}

void FeedDatabase::createTaste(const QStringList& tags, double weight, qint64 nowMs) {
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    const QHash<QString, double> img = tagVector(tags);
    QString blob;
    for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
        if (!blob.isEmpty())
            blob += QLatin1Char(';');
        blob += it.key() + QLatin1Char(':') + QString::number(it.value(), 'f', 6);
    }
    conn.execute(QStringLiteral("INSERT INTO feed_tastes(embedding, strength, confidence, "
                                "exposure, engagement, last_used, created_at) "
                                "VALUES (%1, %2, %3, 0, %4, %5, %5)")
                     .arg(quoted(blob))
                     .arg(weight, 0, 'f', 6)
                     .arg(weight, 0, 'f', 6)
                     .arg(weight * 0.1, 0, 'f', 6)
                     .arg(nowMs));
}

void FeedDatabase::writeTaste(qint64 id, const QHash<QString, double>& embedding, double strength,
                              double confidence, double engagement, qint64 nowMs) {
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    QString blob;
    for (auto it = embedding.constBegin(); it != embedding.constEnd(); ++it) {
        if (!blob.isEmpty())
            blob += QLatin1Char(';');
        blob += it.key() + QLatin1Char(':') + QString::number(it.value(), 'f', 6);
    }
    conn.execute(QStringLiteral("UPDATE feed_tastes SET embedding = %2, strength = %3, "
                                "confidence = %4, engagement = %5, last_used = %6 WHERE id = %1")
                     .arg(id)
                     .arg(quoted(blob))
                     .arg(strength, 0, 'f', 6)
                     .arg(confidence, 0, 'f', 6)
                     .arg(engagement, 0, 'f', 6)
                     .arg(nowMs));
}

void FeedDatabase::writeTasteStrength(qint64 id, double strength) {
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("UPDATE feed_tastes SET strength = %2 WHERE id = %1")
                     .arg(id)
                     .arg(strength, 0, 'f', 6));
}

void FeedDatabase::createTasteVisual(const QByteArray& imageVisual, double weight, qint64 nowMs) {
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    // Embeddings are a few KB - a hex literal is simple and safe here.
    const QString hex = QString::fromLatin1(imageVisual.toHex());
    conn.execute(QStringLiteral("INSERT INTO feed_tastes(visual, visual_dim, embedding, strength, "
                                "confidence, exposure, engagement, last_used, created_at) "
                                "VALUES (unhex('%1'), %2, '', %3, %4, 0, %5, %6, %6)")
                     .arg(hex)
                     .arg(imageVisual.size() / static_cast<int>(sizeof(float)))
                     .arg(weight, 0, 'f', 6)
                     .arg(weight, 0, 'f', 6)
                     .arg(weight * 0.1, 0, 'f', 6)
                     .arg(nowMs));
}

void FeedDatabase::writeTasteVisual(qint64 id, const QByteArray& visual, double strength,
                                    double confidence, double engagement, qint64 nowMs) {
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    const QString hex = QString::fromLatin1(visual.toHex());
    conn.execute(QStringLiteral("UPDATE feed_tastes SET visual = unhex('%2'), visual_dim = %3, "
                                "strength = %4, confidence = %5, engagement = %6, last_used = %7 "
                                "WHERE id = %1")
                     .arg(id)
                     .arg(hex)
                     .arg(visual.size() / static_cast<int>(sizeof(float)))
                     .arg(strength, 0, 'f', 6)
                     .arg(confidence, 0, 'f', 6)
                     .arg(engagement, 0, 'f', 6)
                     .arg(nowMs));
}

}  // namespace Mosaic
