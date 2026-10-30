#include "FeedDatabase.h"

#include "FeedRecommender.h"
#include "VisualFeatures.h"
#include "../core/Paths.h"

#include <QDateTime>
#include <QDebug>
#include <QReadWriteLock>
#include <QSet>

#include <duckdb.h>

#include <cmath>
#include <cstring>

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

QString num(double v) { return QString::number(v, 'g', 12); }

QReadWriteLock s_lock;

// Nothing repeats within the last N shown posts; older history is pruned.
constexpr qint64 kNoRepeatWindow = 100000;

constexpr qint64 kMaxImpressionEvents = 25000;
constexpr qint64 kMaxInteractionEvents = 100000;

// Interaction weights (taste updates).
constexpr double kWeightFavorite = 1.00;
constexpr double kWeightLike = 0.70;
constexpr double kWeightLongView = 0.20;
constexpr double kWeightNormalView = 0.05;
constexpr double kWeightSkip = -0.03;
constexpr double kWeightQuickSkip = -0.08;

constexpr double kLongViewSeconds = 8.0;
constexpr double kQuickSkipSeconds = 1.5;

// Taste model constants.
constexpr double kNewTasteThreshold = 0.30;
// A liked tangent becomes its own taste unless it is clearly part of an
// existing one: the bar for "new interest" is raised to this similarity,
// and its evidence counts double.
constexpr double kTangentTasteThreshold = 0.50;
constexpr double kTangentPromotion = 2.0;
constexpr double kTasteLearningRate = 0.27;
constexpr int kMaxTastes = 12;
constexpr int kMinTasteTags = 3;
constexpr int kTasteTagLimit = 15;
// Positive strength deltas shrink toward this ceiling.
constexpr double kStrengthCeiling = 12.0;
constexpr double kTasteHalfLifeDays = 5.0;
constexpr double kExposureHalfLifeDays = 3.0;
constexpr double kConfidenceRamp = 3.0;
// Tastes below this strength are deleted by decayTastes() so new ones can
// replace them instead of a stale taste squatting a slot forever. Must sit
// well below the weakest creation weight (a normal view creates strength
// 0.05) or fresh tastes get retired before they can grow.
constexpr double kTasteRetireStrength = 0.15;

// Negative model constants. Dislikes accumulate confidence fast: the hard
// filter arms after ~6-10 consistent dislikes of a specific tag.
constexpr double kHardNegativePreference = -0.5;
constexpr double kHardNegativeConfidence = 6.0;
constexpr double kDislikeDelta = -0.35;
// Near-universal tags are structurally barred from vetoing the feed.
constexpr double kMinNegativeSpecificity = 0.35;
constexpr double kDislikeConfidenceDelta = 1.5;
constexpr double kFavoriteDelta = 0.10;
constexpr double kLikeDelta = 0.06;
constexpr double kViewDelta = 0.01;
// Positives fade; negatives never decay - a tag you always dislike must
// stay vetoed, not resurface just because suppressing it means it rarely
// gets re-sampled.
constexpr double kPrefHalfLifeDays = 15.0;

// Session EMA time constant (average over ~this many recent interactions).
constexpr double kSessionTau = 12.0;
// Impressions remembered for the boredom signal.
constexpr int kRecentImpressions = 24;

// Logged signal layout version (see packSignals).
constexpr int kSignalLayoutVersion = 2;

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

// Raw-connection variants for the *Unlocked helpers that receive one.
bool rawExecute(duckdb_connection conn, const QString& sql) {
    if (!conn)
        return false;
    duckdb_result res{};
    const QByteArray q = sql.toUtf8();
    const bool ok = duckdb_query(conn, q.constData(), &res) == DuckDBSuccess;
    if (!ok)
        qWarning().noquote() << "FeedDatabase: query failed:" << duckdb_result_error(&res);
    duckdb_destroy_result(&res);
    return ok;
}

bool rawQuery(duckdb_connection conn, const QString& sql, duckdb_result* res) {
    *res = {};
    if (!conn)
        return false;
    const QByteArray q = sql.toUtf8();
    if (duckdb_query(conn, q.constData(), res) != DuckDBSuccess) {
        duckdb_destroy_result(res);
        return false;
    }
    return true;
}

QString valueString(duckdb_result* res, idx_t col, idx_t row) {
    char* v = duckdb_value_varchar(res, col, row);
    if (!v)
        return {};
    const QString out = QString::fromUtf8(v);
    duckdb_free(v);
    return out;
}

QList<float> blobToFloats(duckdb_result* res, idx_t col, idx_t row) {
    QList<float> out;
    duckdb_blob blob = duckdb_value_blob(res, col, row);
    if (blob.data && blob.size >= sizeof(float)) {
        const int count = static_cast<int>(blob.size / sizeof(float));
        out.resize(count);
        std::memcpy(out.data(), blob.data, count * sizeof(float));
    }
    if (blob.data)
        duckdb_free(blob.data);
    return out;
}

QString floatsToHex(const QList<float>& values) {
    const QByteArray bytes(reinterpret_cast<const char*>(values.constData()),
                           static_cast<int>(values.size() * sizeof(float)));
    return QString::fromLatin1(bytes.toHex());
}

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

QStringList contentTagsOf(const QStringList& tags) {
    QStringList out;
    for (const QString& raw : tags) {
        // Tags are stored space-joined; a space inside one would split it.
        const QString tag = raw.simplified().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
        if (tag.isEmpty() || FeedRecommender::isGenericTag(tag))
            continue;
        out << tag;
    }
    out.removeDuplicates();
    return out;
}

// Model label for a reaction: engaged or not, and how much evidence it is.
bool modelLabel(FeedDatabase::Event kind, double watchSeconds, bool* engaged, double* weight) {
    switch (kind) {
        case FeedDatabase::Event::Favorite:
            *engaged = true;
            *weight = 1.0;
            return true;
        case FeedDatabase::Event::Like:
            *engaged = true;
            *weight = 0.8;
            return true;
        case FeedDatabase::Event::View:
            *engaged = true;
            *weight = watchSeconds >= kLongViewSeconds ? 0.4 : 0.12;
            return true;
        case FeedDatabase::Event::Skip:
            *engaged = false;
            *weight = watchSeconds < kQuickSkipSeconds ? 0.3 : 0.15;
            return true;
        case FeedDatabase::Event::Dislike:
            *engaged = false;
            *weight = 1.0;
            return true;
        case FeedDatabase::Event::Impression:
            break;
    }
    return false;
}

// Candidate-arm reward for a reaction to one of its posts.
bool armReward(FeedDatabase::Event kind, double watchSeconds, double* reward, double* weight) {
    switch (kind) {
        case FeedDatabase::Event::Favorite:
        case FeedDatabase::Event::Like:
            *reward = 1.0;
            *weight = 1.0;
            return true;
        case FeedDatabase::Event::View:
            *reward = watchSeconds >= kLongViewSeconds ? 0.7 : 0.4;
            *weight = 0.5;
            return true;
        case FeedDatabase::Event::Skip:
            *reward = 0.0;
            *weight = watchSeconds < kQuickSkipSeconds ? 0.6 : 0.4;
            return true;
        case FeedDatabase::Event::Dislike:
            *reward = 0.0;
            *weight = 1.0;
            return true;
        case FeedDatabase::Event::Impression:
            break;
    }
    return false;
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
    migrateLegacy();
    repairSkipPoisonedConfidence();
    loadModel();
    return true;
}

void FeedDatabase::close() {
    QWriteLocker lock(&s_lock);
    if (m_db) {
        // Fold the WAL back into the main file so feed.duckdb does not
        // balloon between sessions.
        {
            DbConnectionGuard conn(m_db);
            if (conn.ok()) {
                saveModelStateUnlocked(conn.raw());
                conn.execute(QStringLiteral("CHECKPOINT"));
            }
        }
        duckdb_close(&m_db);
        m_db = nullptr;
    }
}

void FeedDatabase::ensureSchema() {
    // Full current layout. Older databases are upgraded in migrateLegacy().
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
            remote_id BIGINT,
            shown_at TIMESTAMP DEFAULT now()
        );
        CREATE SEQUENCE IF NOT EXISTS feed_taste_seq;
        CREATE TABLE IF NOT EXISTS feed_tastes (
            id              BIGINT PRIMARY KEY DEFAULT nextval('feed_taste_seq'),
            embedding       VARCHAR,            -- "tag:weight;..." taste vector
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
            features            VARCHAR,     -- packed heuristic score + signals
            slot                VARCHAR,     -- 'main' | 'tangent'
            model_prob          DOUBLE,
            uncertainty         DOUBLE
        );
        CREATE TABLE IF NOT EXISTS feed_model (
            key             VARCHAR PRIMARY KEY,   -- "t:tag" / "v:3" / "h:taste" / "b"
            mean            DOUBLE NOT NULL,
            variance        DOUBLE NOT NULL,
            positive        DOUBLE NOT NULL DEFAULT 0,
            negative        DOUBLE NOT NULL DEFAULT 0,
            updated_at      BIGINT NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS feed_model_state (
            id              INTEGER PRIMARY KEY,
            blob            VARCHAR
        );
        CREATE TABLE IF NOT EXISTS feed_arm_stats (
            arm             VARCHAR PRIMARY KEY,
            alpha           DOUBLE NOT NULL,
            beta            DOUBLE NOT NULL
        );
        CREATE TABLE IF NOT EXISTS feed_post_tags (
            md5             VARCHAR PRIMARY KEY,
            source_id       VARCHAR,
            tags            VARCHAR NOT NULL       -- space-separated content tags
        );
        CREATE TABLE IF NOT EXISTS feed_visual (
            md5             VARCHAR PRIMARY KEY,
            vec             BLOB NOT NULL,         -- packed float32 descriptor
            version         INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS feed_warmstart_done (
            md5             VARCHAR PRIMARY KEY
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

void FeedDatabase::migrateLegacy() {
    // Upgrades a pre-learned-model database in place: the ranking log gains
    // the model columns, and the SigLIP-era visual state is dropped. Tag
    // tastes, tag preferences and the full event history are kept - the
    // warm start rebuilds the learned model from them.
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(
        QStringLiteral("ALTER TABLE feed_recommendations ADD COLUMN IF NOT EXISTS slot VARCHAR"));
    conn.execute(QStringLiteral(
        "ALTER TABLE feed_recommendations ADD COLUMN IF NOT EXISTS model_prob DOUBLE"));
    conn.execute(QStringLiteral(
        "ALTER TABLE feed_recommendations ADD COLUMN IF NOT EXISTS uncertainty DOUBLE"));
    // Post-id dedup alongside md5 (rows from before this stay md5-only).
    conn.execute(
        QStringLiteral("ALTER TABLE feed_history ADD COLUMN IF NOT EXISTS remote_id BIGINT"));

    duckdb_result res{};
    bool done = false;
    if (conn.query(QStringLiteral("SELECT value FROM feed_meta WHERE key = 'schema_v2'"), &res)) {
        done = duckdb_row_count(&res) > 0;
        duckdb_destroy_result(&res);
    }
    if (done)
        return;

    // Visual-only tastes have no tag vector, so nothing can query them now.
    // ALTER on a missing column is harmless (logged and ignored).
    bool hasVisualColumn = false;
    if (conn.query(QStringLiteral("SELECT 1 FROM information_schema.columns WHERE "
                                  "table_name = 'feed_tastes' AND column_name = 'visual'"),
                   &res)) {
        hasVisualColumn = duckdb_row_count(&res) > 0;
        duckdb_destroy_result(&res);
    }
    if (hasVisualColumn) {
        conn.execute(
            QStringLiteral("DELETE FROM feed_tastes WHERE embedding IS NULL OR embedding = ''"));
        conn.execute(QStringLiteral("ALTER TABLE feed_tastes DROP COLUMN visual"));
        conn.execute(QStringLiteral("ALTER TABLE feed_tastes DROP COLUMN visual_dim"));
    }
    conn.execute(QStringLiteral("DROP TABLE IF EXISTS image_embeddings"));
    conn.execute(QStringLiteral("DROP TABLE IF EXISTS feed_negative_anchors"));
    conn.execute(QStringLiteral("DROP SEQUENCE IF EXISTS feed_neg_anchor_seq"));
    conn.execute(QStringLiteral("DROP TABLE IF EXISTS feed_user_model"));
    conn.execute(
        QStringLiteral("INSERT INTO feed_meta(key, value) VALUES ('schema_v2', 1) "
                       "ON CONFLICT (key) DO UPDATE SET value = 1"));
    qInfo().noquote() << "FeedDatabase: migrated to the learned-model schema";
}

void FeedDatabase::repairSkipPoisonedConfidence() {
    // One-time repair, guarded by a feed_meta flag: old builds let skips
    // accumulate veto confidence. Clamped rather than zeroed so legitimate
    // dislike evidence survives. Runs inside open() with the write lock
    // held - must not take it again.
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    duckdb_result res{};
    bool alreadyDone = false;
    if (conn.query(QStringLiteral("SELECT value FROM feed_meta WHERE key = "
                                  "'skip_confidence_repaired'"),
                   &res)) {
        alreadyDone = duckdb_row_count(&res) > 0;
        duckdb_destroy_result(&res);
    }
    if (alreadyDone)
        return;
    // The hard filter needs confidence >= 6; old skip-poisoned rows sat far
    // above what real dislike evidence could produce. Cap at 4 - enough to
    // keep a tag eligible as a query exclusion, never enough to veto.
    conn.execute(
        QStringLiteral("UPDATE feed_tag_preferences SET confidence = min(confidence, 4.0) "
                       "WHERE confidence > 4.0"));
    conn.execute(
        QStringLiteral("INSERT INTO feed_meta(key, value) VALUES ('skip_confidence_repaired', 1) "
                       "ON CONFLICT (key) DO UPDATE SET value = 1"));
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

bool FeedDatabase::eventFromName(const QString& name, Event* out) {
    for (Event e : {Event::Impression, Event::View, Event::Skip, Event::Like, Event::Favorite,
                    Event::Dislike}) {
        if (eventName(e) == name) {
            *out = e;
            return true;
        }
    }
    return false;
}

void FeedDatabase::recordEvent(Event kind, const QString& sourceId, const QString& md5,
                               const QStringList& tags, double watchSeconds, int position,
                               qint64 remoteId) {
    if (!m_db || md5.isEmpty())
        return;

    // Content tags only. Generic filtering is mechanical; near-universal
    // content tags are handled by self-learned specificity weighting below.
    const QStringList contentTags = contentTagsOf(tags);

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

    // How this post was ranked: the model trains on the logged signals, the
    // arm bandit credits the arm that found it, tangent slots get promoted.
    const RecommendationRow logged = recommendationForUnlocked(conn.raw(), md5);
    const bool fromTangent = logged.slot == QLatin1String("tangent") ||
                             logged.candidateSource == QLatin1String("tangent");

    if (kind == Event::Impression) {
        // Shown-history for dedup.
        conn.execute(QStringLiteral("INSERT INTO feed_history(md5, source_id, remote_id) "
                                    "VALUES (%1, %2, %3) ON CONFLICT (md5) DO NOTHING")
                         .arg(quoted(md5), quoted(sourceId))
                         .arg(remoteId > 0 ? QString::number(remoteId) : QStringLiteral("NULL")));
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
        bumpMetaUnlocked(conn.raw(), QStringLiteral("total_impressions"), 1.0);
        if (fromTangent)
            bumpMetaUnlocked(conn.raw(), QStringLiteral("tangent_shown"), 1.0);
        // Prune history and raw events so the DB stays bounded.
        conn.execute(
            QStringLiteral("DELETE FROM feed_history WHERE md5 IN (SELECT md5 FROM feed_history "
                           "ORDER BY shown_at DESC OFFSET %1)")
                .arg(kNoRepeatWindow));
        // Impressions dominate and are pure bookkeeping (no taste signal),
        // so keep only a bounded recent window. Non-impression events are
        // tiny but also bounded for the same reason.
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

        m_recentImpressionTags.prepend(contentTags);
        while (m_recentImpressionTags.size() > kRecentImpressions)
            m_recentImpressionTags.removeLast();
    } else {
        // Reacted-to posts keep their tags so the model can be rebuilt
        // from history later.
        storePostTagsUnlocked(conn.raw(), md5, sourceId, contentTags);
    }

    // taste updates
    // Only positive events grow tastes. A liked tangent is promoted: it
    // counts double and founds its own taste unless it clearly belongs to
    // an existing one.
    switch (kind) {
        case Event::Favorite:
        case Event::Like: {
            double w = kind == Event::Favorite ? kWeightFavorite : kWeightLike;
            double threshold = kNewTasteThreshold;
            if (fromTangent) {
                w *= kTangentPromotion;
                threshold = kTangentTasteThreshold;
                bumpMetaUnlocked(conn.raw(), QStringLiteral("tangent_hits"), 1.0);
            }
            updateTastesOnPositive(contentTags, w, threshold);
            break;
        }
        case Event::View: {
            const double w = watchSeconds >= kLongViewSeconds ? kWeightLongView : kWeightNormalView;
            updateTastesOnPositive(contentTags, w, kNewTasteThreshold);
            break;
        }
        case Event::Dislike:
            updateTastesOnNegative(contentTags, 1.0);
            break;
        case Event::Skip:
        case Event::Impression:
            break;
    }

    // tag preferences (heuristic scorer + hard-negative filter)
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
            // Decay only positive preferences; negatives persist forever.
            const double decay =
                updated > 0 && pref > 0.0 ? decayFactor(updated, kPrefHalfLifeDays) : 1.0;
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

    if (kind == Event::Impression)
        return;

    // learned model
    trainModelUnlocked(conn.raw(), md5, contentTags, kind, watchSeconds,
                       logged.md5.isEmpty() ? nullptr : &logged, true);

    // candidate-arm bandit: credit the arm whose query found this post
    double reward = 0.0, rewardWeight = 0.0;
    if (!logged.candidateSource.isEmpty() &&
        armReward(kind, watchSeconds, &reward, &rewardWeight)) {
        m_arms.observe(logged.candidateSource, reward, rewardWeight);
        saveArmUnlocked(conn.raw(), logged.candidateSource);
    }
}

void FeedDatabase::trainModelUnlocked(duckdb_connection conn, const QString& md5,
                                      const QStringList& contentTags, Event kind,
                                      double watchSeconds, const RecommendationRow* logged,
                                      bool updateBias) {
    bool engaged = false;
    double weight = 0.0;
    if (!modelLabel(kind, watchSeconds, &engaged, &weight))
        return;

    const QList<double> visual = m_model.standardizeVisual(visualForUnlocked(conn, md5));
    double signalValues[kSignalCount] = {};
    double heuristicScore = 0.0;
    const bool hasSignals =
        logged && unpackSignals(logged->features, &heuristicScore, signalValues);
    const FeatureVector x =
        buildModelFeatures(contentTags, visual, hasSignals ? signalValues : nullptr);

    // Progressive validation: both scorers predict this reaction BEFORE
    // learning from it. Only live, ranked posts count - their heuristic
    // score is known.
    if (hasSignals) {
        const double modelProb = m_model.predict(x);
        const double heuristicProb = m_model.heuristicProbability(heuristicScore);
        m_model.recordValidation(modelProb, heuristicProb, engaged, weight);
        m_model.trainHeuristicCalibration(heuristicScore, engaged, weight);
    }

    QStringList touched;
    m_model.train(x, engaged, weight, &touched, updateBias);
    saveBeliefsUnlocked(conn, touched);
    saveModelStateUnlocked(conn);
}

bool FeedDatabase::wasShown(const QString& md5, const QString& sourceId, qint64 remoteId) const {
    if (!m_db || md5.isEmpty())
        return false;

    QString match = QStringLiteral("md5 = %1").arg(quoted(md5));
    if (remoteId > 0 && !sourceId.isEmpty())
        match += QStringLiteral(" OR (source_id = %1 AND remote_id = %2)")
                     .arg(quoted(sourceId))
                     .arg(remoteId);

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return false;

    duckdb_result res{};
    if (!conn.query(
            QStringLiteral("SELECT 1 FROM (SELECT md5, source_id, remote_id FROM feed_history "
                           "ORDER BY shown_at DESC LIMIT %1) WHERE %2 LIMIT 1")
                .arg(kNoRepeatWindow)
                .arg(match),
            &res))
        return false;
    const bool shown = duckdb_row_count(&res) > 0;
    duckdb_destroy_result(&res);
    return shown;
}

// visual descriptors

bool FeedDatabase::hasVisual(const QString& md5) const { return !visualFor(md5).isEmpty(); }

QList<float> FeedDatabase::visualFor(const QString& md5) const {
    if (!m_db || md5.isEmpty())
        return {};
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return {};
    return visualForUnlocked(conn.raw(), md5);
}

QList<float> FeedDatabase::visualForUnlocked(duckdb_connection conn, const QString& md5) const {
    QList<float> out;
    duckdb_result res{};
    if (!rawQuery(conn,
                  QStringLiteral("SELECT vec FROM feed_visual WHERE md5 = %1 AND version = %2")
                      .arg(quoted(md5))
                      .arg(VisualFeatures::kVersion),
                  &res))
        return out;
    if (duckdb_row_count(&res) > 0)
        out = blobToFloats(&res, 0, 0);
    duckdb_destroy_result(&res);
    return out;
}

QHash<QString, QList<float>> FeedDatabase::visualsFor(const QStringList& md5s) const {
    QHash<QString, QList<float>> out;
    if (!m_db || md5s.isEmpty())
        return out;
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;
    constexpr int kChunk = 200;
    for (int start = 0; start < md5s.size(); start += kChunk) {
        QStringList quotedIds;
        for (const QString& md5 : md5s.mid(start, kChunk))
            if (!md5.isEmpty())
                quotedIds << quoted(md5);
        if (quotedIds.isEmpty())
            continue;
        duckdb_result res{};
        if (!conn.query(QStringLiteral("SELECT md5, vec FROM feed_visual WHERE version = %1 AND "
                                       "md5 IN (%2)")
                            .arg(VisualFeatures::kVersion)
                            .arg(quotedIds.join(QLatin1Char(','))),
                        &res))
            continue;
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row)
            out.insert(valueString(&res, 0, row), blobToFloats(&res, 1, row));
        duckdb_destroy_result(&res);
    }
    return out;
}

void FeedDatabase::storeVisual(const QString& md5, const QList<float>& raw) {
    if (!m_db || md5.isEmpty() || raw.isEmpty())
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("INSERT INTO feed_visual(md5, vec, version) VALUES (%1, "
                                "unhex('%2'), %3) ON CONFLICT (md5) DO UPDATE SET "
                                "vec = unhex('%2'), version = %3")
                     .arg(quoted(md5), floatsToHex(raw))
                     .arg(VisualFeatures::kVersion));
    m_model.observeVisual(raw);
    // Standardization stats change slowly; persisting every descriptor
    // would be wasted writes.
    if (++m_visualSinceSave >= 25) {
        m_visualSinceSave = 0;
        saveModelStateUnlocked(conn.raw());
    }
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
    if (!conn.query(QStringLiteral("SELECT id, embedding, strength, confidence, exposure, "
                                   "engagement, last_used, created_at FROM feed_tastes"),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        Taste t;
        t.id = duckdb_value_int64(&res, 0, row);
        const QStringList pairs =
            valueString(&res, 1, row).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& pair : pairs) {
            const int colon = pair.indexOf(QLatin1Char(':'));
            if (colon <= 0)
                continue;
            t.embedding[pair.left(colon)] = pair.mid(colon + 1).toDouble();
        }
        t.strength = duckdb_value_double(&res, 2, row);
        t.confidence = duckdb_value_double(&res, 3, row);
        t.exposure = duckdb_value_double(&res, 4, row);
        t.engagement = duckdb_value_double(&res, 5, row);
        t.lastUsedMs = duckdb_value_int64(&res, 6, row);
        t.createdAtMs = duckdb_value_int64(&res, 7, row);
        if (!t.embedding.isEmpty())
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

void FeedDatabase::updateTastesOnPositive(const QStringList& tags, double weight,
                                          double newTasteThreshold) {
    if (!m_db || tags.size() < kMinTasteTags || weight <= 0.0)
        return;

    QList<Taste> all = tastesUnlocked();

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

    if (bestId == 0 || bestSim < newTasteThreshold) {
        if (all.size() >= kMaxTastes) {
            const Taste& weakest = all.last();
            if (weakest.strength > weight)
                return;  // keep existing tastes; this signal is too weak
            // Replace the weakest taste outright. Merging the new interest
            // into an unrelated taste would let tag pruning discard it.
            DbConnectionGuard conn(m_db);
            if (conn.ok())
                conn.execute(
                    QStringLiteral("DELETE FROM feed_tastes WHERE id = %1").arg(weakest.id));
        }
        createTaste(tags, weight, nowMs);
        return;
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
    // Retire tastes that decayed into noise so their slot frees up for a
    // new taste; otherwise a stale taste blocks kMaxTastes forever.
    conn.execute(QStringLiteral("DELETE FROM feed_tastes WHERE strength < %1")
                     .arg(kTasteRetireStrength, 0, 'f', 4));
}

// tag specificity

double FeedDatabase::totalImpressions() const {
    return metaValue(QStringLiteral("total_impressions"));
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
        if (rawQuery(conn,
                     QStringLiteral("SELECT value FROM feed_meta WHERE key = 'total_impressions'"),
                     &res)) {
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
    if (!rawQuery(conn, QStringLiteral("SELECT tag, doc_count FROM feed_tag_stats"), &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        const QString tag = valueString(&res, 0, row);
        if (tag.isEmpty())
            continue;
        const double docCount = static_cast<double>(duckdb_value_int64(&res, 1, row));
        if (docCount < kMinDocSamples)
            continue;
        // Classic IDF, squashed to [0,1]: a tag on 100% of impressions ->
        // 0 specificity; a tag on a tiny fraction -> close to 1.
        const double freq = qBound(1e-6, docCount / total, 1.0);
        const double idf = -std::log2(freq);
        out.insert(tag, qBound(0.0, idf / 10.0, 1.0));
    }
    duckdb_destroy_result(&res);
    return out;
}

// learned model

void FeedDatabase::loadModel() {
    // Called from open() which already holds the write lock.
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    duckdb_result res{};
    if (conn.query(QStringLiteral("SELECT blob FROM feed_model_state WHERE id = 0"), &res)) {
        if (duckdb_row_count(&res) > 0)
            m_model.deserializeState(valueString(&res, 0, 0).toUtf8());
        duckdb_destroy_result(&res);
    }
    if (conn.query(QStringLiteral("SELECT key, mean, variance, positive, negative, updated_at "
                                  "FROM feed_model"),
                   &res)) {
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
            FeedModel::Belief b;
            b.mean = duckdb_value_double(&res, 1, row);
            b.variance = duckdb_value_double(&res, 2, row);
            b.positive = duckdb_value_double(&res, 3, row);
            b.negative = duckdb_value_double(&res, 4, row);
            b.updatedMs = duckdb_value_int64(&res, 5, row);
            m_model.setBelief(valueString(&res, 0, row), b);
        }
        duckdb_destroy_result(&res);
    }
    if (conn.query(QStringLiteral("SELECT arm, alpha, beta FROM feed_arm_stats"), &res)) {
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
            ArmBandit::Arm a;
            a.alpha = duckdb_value_double(&res, 1, row);
            a.beta = duckdb_value_double(&res, 2, row);
            m_arms.setArm(valueString(&res, 0, row), a);
        }
        duckdb_destroy_result(&res);
    }
    if (conn.query(QStringLiteral("SELECT value FROM feed_meta WHERE key = 'exploration_level'"),
                   &res)) {
        if (duckdb_row_count(&res) > 0)
            m_exploration = qBound(0.0, duckdb_value_double(&res, 0, 0), 1.0);
        duckdb_destroy_result(&res);
    }
    qDebug().noquote() << "FeedDatabase: model loaded |" << m_model.beliefs().size() << "beliefs |"
                       << m_model.samples() << "samples | blend" << m_model.blendWeight();
}

void FeedDatabase::saveBeliefsUnlocked(duckdb_connection conn, const QStringList& keys) const {
    if (!conn || keys.isEmpty())
        return;
    QStringList unique = keys;
    unique.removeDuplicates();
    // One multi-row upsert per chunk: a reaction touches ~70 keys, and one
    // statement per key would hold the write lock for no reason.
    constexpr int kChunk = 250;
    for (int start = 0; start < unique.size(); start += kChunk) {
        QStringList values;
        for (const QString& key : unique.mid(start, kChunk)) {
            const FeedModel::Belief b = m_model.beliefs().value(key);
            values << QStringLiteral("(%1, %2, %3, %4, %5, %6)")
                          .arg(quoted(key), num(b.mean),
                               num(b.variance > 0.0 ? b.variance : FeedModel::priorVariance(key)),
                               num(b.positive), num(b.negative))
                          .arg(b.updatedMs);
        }
        rawExecute(conn,
                   QStringLiteral("INSERT INTO feed_model(key, mean, variance, positive, negative, "
                                  "updated_at) VALUES %1 ON CONFLICT (key) DO UPDATE SET "
                                  "mean = excluded.mean, variance = excluded.variance, "
                                  "positive = excluded.positive, negative = excluded.negative, "
                                  "updated_at = excluded.updated_at")
                       .arg(values.join(QLatin1Char(','))));
    }
}

void FeedDatabase::saveModelStateUnlocked(duckdb_connection conn) const {
    if (!conn)
        return;
    rawExecute(conn, QStringLiteral("INSERT INTO feed_model_state(id, blob) VALUES (0, %1) "
                                    "ON CONFLICT (id) DO UPDATE SET blob = %1")
                         .arg(quoted(QString::fromUtf8(m_model.serializeState()))));
}

void FeedDatabase::saveArmUnlocked(duckdb_connection conn, const QString& arm) const {
    const ArmBandit::Arm a = m_arms.arms().value(arm);
    rawExecute(conn, QStringLiteral("INSERT INTO feed_arm_stats(arm, alpha, beta) VALUES "
                                    "(%1, %2, %3) ON CONFLICT (arm) DO UPDATE SET "
                                    "alpha = %2, beta = %3")
                         .arg(quoted(arm), num(a.alpha), num(a.beta)));
}

FeedModel FeedDatabase::modelSnapshot() const {
    QReadLocker lock(&s_lock);
    return m_model;
}

ArmBandit FeedDatabase::armSnapshot() const {
    QReadLocker lock(&s_lock);
    return m_arms;
}

double FeedDatabase::explorationLevel() const {
    QReadLocker lock(&s_lock);
    return m_exploration;
}

void FeedDatabase::setExplorationLevel(double level) {
    {
        QWriteLocker lock(&s_lock);
        m_exploration = qBound(0.0, level, 1.0);
    }
    setMetaValue(QStringLiteral("exploration_level"), qBound(0.0, level, 1.0));
}

double FeedDatabase::metaValue(const QString& key, double fallback) const {
    if (!m_db)
        return fallback;
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return fallback;
    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT value FROM feed_meta WHERE key = %1").arg(quoted(key)),
                    &res))
        return fallback;
    double v = fallback;
    if (duckdb_row_count(&res) > 0)
        v = duckdb_value_double(&res, 0, 0);
    duckdb_destroy_result(&res);
    return v;
}

void FeedDatabase::setMetaValue(const QString& key, double value) {
    if (!m_db)
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("INSERT INTO feed_meta(key, value) VALUES (%1, %2) "
                                "ON CONFLICT (key) DO UPDATE SET value = %2")
                     .arg(quoted(key), num(value)));
}

void FeedDatabase::bumpMetaUnlocked(duckdb_connection conn, const QString& key, double delta) {
    rawExecute(conn, QStringLiteral("INSERT INTO feed_meta(key, value) VALUES (%1, %2) "
                                    "ON CONFLICT (key) DO UPDATE SET value = feed_meta.value + %2")
                         .arg(quoted(key), num(delta)));
}

// warm start

bool FeedDatabase::warmStartDone() const {
    return metaValue(QStringLiteral("warmstart_done")) > 0.5;
}

void FeedDatabase::setWarmStartDone(bool done) {
    setMetaValue(QStringLiteral("warmstart_done"), done ? 1.0 : 0.0);
}

void FeedDatabase::seedModelFromTagPreferences() {
    if (!m_db)
        return;
    const QHash<QString, TagPreference> rows = tagPreferenceRows();
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    QStringList touched;
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        if (FeedRecommender::isGenericTag(it.key()))
            continue;
        const QString key = QStringLiteral("t:") + it.key();
        if (m_model.hasBelief(key))
            continue;
        // The heuristic's per-tag preference becomes the prior mean, with a
        // slightly tightened variance: an informed guess, not a conclusion.
        FeedModel::Belief b;
        b.mean = qBound(-1.5, 3.0 * it.value().preference, 1.5);
        b.variance = 0.7 * FeedModel::priorVariance(key);
        b.updatedMs = QDateTime::currentMSecsSinceEpoch();
        m_model.setBelief(key, b);
        touched << key;
    }
    saveBeliefsUnlocked(conn.raw(), touched);
    qInfo().noquote() << "FeedDatabase: seeded" << touched.size()
                      << "model priors from tag preferences";
}

QList<FeedDatabase::HistoryEvent> FeedDatabase::warmStartHistory(int maxViews, int maxSkips) const {
    QList<HistoryEvent> out;
    if (!m_db)
        return out;

    // Only events from before the warm start began: anything newer was
    // already trained live. The cutoff is fixed on the first call so a
    // resumed warm start never replays a live reaction.
    double cutoff = metaValue(QStringLiteral("warmstart_cutoff"));
    if (cutoff <= 0.0) {
        QReadLocker lock(&s_lock);
        DbConnectionGuard conn(m_db);
        duckdb_result res{};
        if (conn.ok() && conn.query(QStringLiteral("SELECT coalesce(max(id), 0) + 1 FROM "
                                                   "feed_events"),
                                    &res)) {
            if (duckdb_row_count(&res) > 0)
                cutoff = static_cast<double>(duckdb_value_int64(&res, 0, 0));
            duckdb_destroy_result(&res);
        }
        lock.unlock();
        const_cast<FeedDatabase*>(this)->setMetaValue(QStringLiteral("warmstart_cutoff"), cutoff);
    }

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    const QString pending =
        QStringLiteral("id < %1 AND md5 NOT IN (SELECT md5 FROM feed_warmstart_done)")
            .arg(static_cast<qint64>(cutoff));

    // Strongest explicit reaction per post (favorite > like > dislike).
    QHash<QString, HistoryEvent> explicitByMd5;
    auto rank = [](Event e) {
        return e == Event::Favorite ? 3 : e == Event::Like ? 2 : e == Event::Dislike ? 1 : 0;
    };
    duckdb_result res{};
    if (conn.query(QStringLiteral("SELECT md5, any_value(source_id), event_type, "
                                  "max(watch_seconds) FROM feed_events WHERE event_type IN "
                                  "('like', 'favorite', 'dislike') AND %1 GROUP BY md5, "
                                  "event_type")
                       .arg(pending),
                   &res)) {
        for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
            HistoryEvent e;
            e.md5 = valueString(&res, 0, row);
            e.sourceId = valueString(&res, 1, row);
            if (!eventFromName(valueString(&res, 2, row), &e.kind))
                continue;
            e.watchSeconds = duckdb_value_double(&res, 3, row);
            auto it = explicitByMd5.find(e.md5);
            if (it == explicitByMd5.end() || rank(e.kind) > rank(it->kind))
                explicitByMd5.insert(e.md5, e);
        }
        duckdb_destroy_result(&res);
    }
    out = explicitByMd5.values();

    // Implicit reactions, sampled: long views (mild positives) and skips
    // (the bulk of the negatives - without them the model would learn
    // that everything is liked).
    auto sample = [&](const QString& type, const QString& extra, const QString& agg, int limit) {
        if (limit <= 0)
            return;
        duckdb_result r{};
        if (!conn.query(QStringLiteral("SELECT md5, any_value(source_id), %1(watch_seconds) "
                                       "FROM feed_events WHERE event_type = '%2' AND %3 %4 "
                                       "GROUP BY md5 ORDER BY random() LIMIT %5")
                            .arg(agg, type, pending, extra)
                            .arg(limit),
                        &r))
            return;
        Event kind = Event::View;
        eventFromName(type, &kind);
        for (idx_t row = 0; row < duckdb_row_count(&r); ++row) {
            HistoryEvent e;
            e.md5 = valueString(&r, 0, row);
            if (explicitByMd5.contains(e.md5))
                continue;
            e.sourceId = valueString(&r, 1, row);
            e.kind = kind;
            e.watchSeconds = duckdb_value_double(&r, 2, row);
            out.append(e);
        }
        duckdb_destroy_result(&r);
    };
    sample(QStringLiteral("view"), QStringLiteral("AND watch_seconds >= %1").arg(kLongViewSeconds),
           QStringLiteral("max"), maxViews);
    sample(QStringLiteral("skip"), QString(), QStringLiteral("min"), maxSkips);
    return out;
}

void FeedDatabase::markWarmStartProcessed(const QString& md5) {
    if (!m_db || md5.isEmpty())
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    conn.execute(QStringLiteral("INSERT INTO feed_warmstart_done(md5) VALUES (%1) "
                                "ON CONFLICT (md5) DO NOTHING")
                     .arg(quoted(md5)));
}

void FeedDatabase::trainFromHistory(const QString& md5, const QStringList& tags, Event kind,
                                    double watchSeconds, bool updateBias) {
    if (!m_db || md5.isEmpty())
        return;
    const QStringList contentTags = contentTagsOf(tags);
    if (contentTags.isEmpty())
        return;

    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    storePostTagsUnlocked(conn.raw(), md5, QString(), contentTags);
    // Replays never count toward validation: the heuristic score they were
    // ranked under is long gone.
    trainModelUnlocked(conn.raw(), md5, contentTags, kind, watchSeconds, nullptr, updateBias);

    switch (kind) {
        case Event::Favorite:
            updateTastesOnPositive(contentTags, kWeightFavorite, kNewTasteThreshold);
            break;
        case Event::Like:
            updateTastesOnPositive(contentTags, kWeightLike, kNewTasteThreshold);
            break;
        case Event::View:
            updateTastesOnPositive(contentTags, kWeightLongView, kNewTasteThreshold);
            break;
        case Event::Dislike:
            updateTastesOnNegative(contentTags, 1.0);
            break;
        case Event::Skip:
        case Event::Impression:
            break;
    }
}

// post tags

QStringList FeedDatabase::postTags(const QString& md5) const {
    if (!m_db || md5.isEmpty())
        return {};
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return {};
    duckdb_result res{};
    QStringList out;
    if (conn.query(
            QStringLiteral("SELECT tags FROM feed_post_tags WHERE md5 = %1").arg(quoted(md5)),
            &res)) {
        if (duckdb_row_count(&res) > 0)
            out = valueString(&res, 0, 0).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        duckdb_destroy_result(&res);
    }
    return out;
}

void FeedDatabase::storePostTags(const QString& md5, const QString& sourceId,
                                 const QStringList& tags) {
    if (!m_db || md5.isEmpty())
        return;
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return;
    storePostTagsUnlocked(conn.raw(), md5, sourceId, contentTagsOf(tags));
}

void FeedDatabase::storePostTagsUnlocked(duckdb_connection conn, const QString& md5,
                                         const QString& sourceId, const QStringList& tags) {
    if (tags.isEmpty())
        return;
    rawExecute(conn, QStringLiteral("INSERT INTO feed_post_tags(md5, source_id, tags) VALUES "
                                    "(%1, %2, %3) ON CONFLICT (md5) DO UPDATE SET tags = %3, "
                                    "source_id = coalesce(nullif(%2, ''), "
                                    "feed_post_tags.source_id)")
                         .arg(quoted(md5), quoted(sourceId), quoted(tags.join(QLatin1Char(' ')))));
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
        p.tag = valueString(&res, 0, row);
        if (p.tag.isEmpty())
            continue;
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

QHash<QString, double> FeedDatabase::sessionWeights() const {
    QReadLocker lock(&s_lock);
    return m_sessionVec;
}

double FeedDatabase::sessionSimilarity(const QStringList& tags) const {
    if (tags.isEmpty())
        return 0.0;
    const QHash<QString, double> img = tagVector(tags);
    double imgNorm = 0.0;
    for (auto it = img.constBegin(); it != img.constEnd(); ++it) imgNorm += it.value() * it.value();
    imgNorm = std::sqrt(imgNorm);
    if (imgNorm < 1e-9)
        return 0.0;

    QReadLocker lock(&s_lock);
    double sim = 0.0;
    if (!m_sessionVec.isEmpty()) {
        double dot = 0.0, sesNorm = 0.0;
        for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
            auto sit = m_sessionVec.constFind(it.key());
            if (sit != m_sessionVec.constEnd())
                dot += it.value() * sit.value();
        }
        for (auto it = m_sessionVec.constBegin(); it != m_sessionVec.constEnd(); ++it)
            sesNorm += it.value() * it.value();
        sesNorm = std::sqrt(sesNorm);
        if (sesNorm > 1e-9)
            sim = dot / (imgNorm * sesNorm);
    }

    // Active sway tags dominate: matching posts get boosted to at least the
    // sway similarity, non-matching posts are pulled down.
    if (!m_swayTags.isEmpty()) {
        const QHash<QString, double> sway = tagVector(m_swayTags);
        double dot = 0.0, swayNorm = 0.0;
        for (auto it = img.constBegin(); it != img.constEnd(); ++it) {
            auto sit = sway.constFind(it.key());
            if (sit != sway.constEnd())
                dot += it.value() * sit.value();
        }
        for (auto it = sway.constBegin(); it != sway.constEnd(); ++it)
            swayNorm += it.value() * it.value();
        swayNorm = std::sqrt(swayNorm);
        if (swayNorm > 1e-9) {
            const double swaySim = dot / (imgNorm * swayNorm);
            sim = qMax(sim, swaySim) - 0.25 * (1.0 - swaySim);
        }
    }
    return sim;
}

QList<QStringList> FeedDatabase::recentImpressionTags() const {
    QReadLocker lock(&s_lock);
    return m_recentImpressionTags;
}

void FeedDatabase::setSwayTags(const QStringList& tags) {
    QWriteLocker lock(&s_lock);
    m_swayTags.clear();
    for (const QString& raw : tags) {
        const QString tag = raw.trimmed().toLower();
        if (!tag.isEmpty() && !FeedRecommender::isGenericTag(tag))
            m_swayTags << tag;
    }
}

QStringList FeedDatabase::swayTags() const {
    QReadLocker lock(&s_lock);
    return m_swayTags;
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
        const QString kind = valueString(&res, 0, row);
        if (kind.isEmpty())
            continue;
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
        const QString kind = valueString(&res, 0, row);
        if (kind.isEmpty())
            continue;
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
        const QString type = valueString(&res, 0, row);
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
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row)
        out.append({valueString(&res, 0, row), valueString(&res, 1, row)});
    duckdb_destroy_result(&res);
    return out;
}

QList<QPair<QString, QString>> FeedDatabase::likedInteractions(int limit, int offset,
                                                               quint32 seed) const {
    QList<QPair<QString, QString>> out;
    if (!m_db)
        return out;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return out;

    // A non-zero seed shuffles the set deterministically: hash(md5 || seed)
    // is stable across queries, so paging stays consistent while a new seed
    // gives a different walk through the same posts. Seed 0 keeps the
    // newest-first order.
    const QString order =
        seed == 0 ? QStringLiteral("max(\"timestamp\") DESC, md5")
                  : QStringLiteral("hash(md5 || %1), md5").arg(quoted(QString::number(seed)));

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT md5, max(source_id) FROM feed_events "
                                   "WHERE event_type IN ('like', 'favorite') "
                                   "GROUP BY md5 ORDER BY %1 "
                                   "LIMIT %2 OFFSET %3")
                        .arg(order)
                        .arg(qMax(1, limit))
                        .arg(qMax(0, offset)),
                    &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        const QString md5 = valueString(&res, 0, row);
        if (!md5.isEmpty())
            out.append({md5, valueString(&res, 1, row)});
    }
    duckdb_destroy_result(&res);
    return out;
}

int FeedDatabase::likedInteractionCount() const {
    if (!m_db)
        return 0;

    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return 0;

    duckdb_result res{};
    if (!conn.query(QStringLiteral("SELECT count(DISTINCT md5) FROM feed_events "
                                   "WHERE event_type IN ('like', 'favorite')"),
                    &res))
        return 0;
    int count = 0;
    if (duckdb_row_count(&res) > 0)
        count = valueString(&res, 0, 0).toInt();
    duckdb_destroy_result(&res);
    return count;
}

// recommendation log

QString FeedDatabase::packSignals(double heuristicScore, const double* signalValues) {
    QStringList parts;
    parts.reserve(kSignalCount);
    for (int i = 0; i < kSignalCount; ++i) parts << QString::number(signalValues[i], 'f', 4);
    return QStringLiteral("%1|%2|%3")
        .arg(kSignalLayoutVersion)
        .arg(heuristicScore, 0, 'f', 5)
        .arg(parts.join(QLatin1Char(' ')));
}

bool FeedDatabase::unpackSignals(const QString& packed, double* heuristicScore,
                                 double* signalValues) {
    const QStringList sections = packed.split(QLatin1Char('|'));
    if (sections.size() != 3 || sections.at(0).toInt() != kSignalLayoutVersion)
        return false;
    bool ok = false;
    const double score = sections.at(1).toDouble(&ok);
    if (!ok)
        return false;
    const QStringList values = sections.at(2).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (values.size() != kSignalCount)
        return false;
    for (int i = 0; i < kSignalCount; ++i) {
        signalValues[i] = values.at(i).toDouble(&ok);
        if (!ok)
            return false;
    }
    *heuristicScore = score;
    return true;
}

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
                           "final_score, candidate_source, rejected_negative, features, slot, "
                           "model_prob, uncertainty) VALUES (%1, %2, %3, %4, %5, %6, %7, %8, "
                           "%9, %10, %11, %12, %13, %14, %15, %16, %17)")
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
                .arg(quoted(r.features))
                .arg(quoted(r.slot))
                .arg(r.modelProbability, 0, 'f', 4)
                .arg(r.uncertainty, 0, 'f', 4));
    }
    // Keep the log bounded: only recent rows can still receive a reaction.
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
                           "rejected_negative, slot, model_prob, uncertainty "
                           "FROM feed_recommendations ORDER BY generated_at DESC LIMIT %1")
                .arg(limit),
            &res))
        return out;
    for (idx_t row = 0; row < duckdb_row_count(&res); ++row) {
        RecommendationRow r;
        r.md5 = valueString(&res, 0, row);
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
        r.candidateSource = valueString(&res, 11, row);
        r.rejectedNegative = duckdb_value_boolean(&res, 12, row);
        r.slot = valueString(&res, 13, row);
        r.modelProbability = duckdb_value_double(&res, 14, row);
        r.uncertainty = duckdb_value_double(&res, 15, row);
        out.append(r);
    }
    duckdb_destroy_result(&res);
    return out;
}

FeedDatabase::RecommendationRow FeedDatabase::recommendationFor(const QString& md5) const {
    if (!m_db || md5.isEmpty())
        return {};
    QReadLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok())
        return {};
    return recommendationForUnlocked(conn.raw(), md5);
}

FeedDatabase::RecommendationRow FeedDatabase::recommendationForUnlocked(duckdb_connection conn,
                                                                        const QString& md5) const {
    RecommendationRow r;
    duckdb_result res{};
    if (!rawQuery(conn,
                  QStringLiteral("SELECT candidate_source, slot, features, model_prob, "
                                 "uncertainty, final_score, taste_id FROM feed_recommendations "
                                 "WHERE md5 = %1 ORDER BY generated_at DESC LIMIT 1")
                      .arg(quoted(md5)),
                  &res))
        return r;
    if (duckdb_row_count(&res) > 0) {
        r.md5 = md5;
        r.candidateSource = valueString(&res, 0, 0);
        r.slot = valueString(&res, 1, 0);
        r.features = valueString(&res, 2, 0);
        r.modelProbability = duckdb_value_double(&res, 3, 0);
        r.uncertainty = duckdb_value_double(&res, 4, 0);
        r.finalScore = duckdb_value_double(&res, 5, 0);
        r.tasteId = duckdb_value_int64(&res, 6, 0);
    }
    duckdb_destroy_result(&res);
    return r;
}

// tuning

void FeedDatabase::reset(bool rerunWarmStart, QString* error) {
    QWriteLocker lock(&s_lock);
    DbConnectionGuard conn(m_db);
    if (!conn.ok()) {
        if (error)
            *error = QStringLiteral("Feed database is not open");
        return;
    }
    for (const char* table : {"feed_events", "feed_history", "feed_tastes", "feed_tag_preferences",
                              "feed_linger_stats", "feed_recommendations", "feed_model",
                              "feed_arm_stats", "feed_post_tags", "feed_warmstart_done"})
        conn.execute(QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)));
    conn.execute(QStringLiteral(
        "DELETE FROM feed_meta WHERE key IN ('warmstart_cutoff', 'warmstart_progress', "
        "'warmstart_total', 'warmstart_priors', 'warmstart_favorites', 'tangent_shown', "
        "'tangent_hits')"));
    conn.execute(QStringLiteral("INSERT INTO feed_meta(key, value) VALUES ('warmstart_done', %1) "
                                "ON CONFLICT (key) DO UPDATE SET value = %1")
                     .arg(rerunWarmStart ? 0 : 1));
    // Tag doc-frequency, visual descriptors and the exploration level are
    // catalog knowledge / settings, not preference - a reset keeps them.
    m_sessionVec.clear();
    m_sessionMass = 0.0;
    m_recentImpressionTags.clear();
    m_model.reset();
    m_arms.reset();
    saveModelStateUnlocked(conn.raw());
}

// taste helpers

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

}  // namespace Mosaic
