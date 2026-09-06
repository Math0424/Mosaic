#include "DuplicateDetector.h"

#include "LibraryCatalog.h"
#include "LibrarySql.h"
#include "../../core/Database.h"
#include "../../core/Paths.h"
#include "../../media/ImageFingerprint.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <bitset>

namespace Mosaic {

using namespace LibrarySql;

namespace {

/** Fingerprintable entry for one library image. */
struct Entry {
    QString md5;
    QString path;
    qint64 phash = 0;
    QByteArray sift;
    qint64 createdAt = 0;  ///< epoch seconds (0 = unknown) - older wins on tie
    qint64 tagCount = 0;   ///< number of tags - more tags wins
};

bool isVideoFormat(const QString& format) {
    return format == QLatin1String("webm") || format == QLatin1String("mp4") ||
           format == QLatin1String("mov") || format == QLatin1String("mkv") ||
           format == QLatin1String("avi") || format == QLatin1String("gif");
}

}  // namespace

QList<DuplicatePair> DuplicateDetector::findDuplicates(
    const std::function<void(int, int)>& progress, QString* error) {
    QList<DuplicatePair> pairs;

    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Database is not open");
        return pairs;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open database connection");
        return pairs;
    }

    // Collect the fingerprintable files first (cheap catalog query).
    QList<QPair<QString, QString>> files;  // md5, on-disk path
    try {
        duckdb_result res{};
        QString dbError;
        const QString sql = QStringLiteral("SELECT md5, format FROM images ORDER BY md5");
        if (!conn.query(sql, &res, &dbError)) {
            if (error)
                *error = dbError;
            return pairs;
        }
        const idx_t rowCount = duckdb_row_count(&res);
        for (idx_t row = 0; row < rowCount; ++row) {
            char* md5Value = duckdb_value_varchar(&res, 0, row);
            char* formatValue = duckdb_value_varchar(&res, 1, row);
            if (!md5Value || !formatValue) {
                if (md5Value)
                    duckdb_free(md5Value);
                if (formatValue)
                    duckdb_free(formatValue);
                continue;
            }
            const QString md5 = QString::fromUtf8(md5Value);
            const QString format = QString::fromUtf8(formatValue);
            duckdb_free(md5Value);
            duckdb_free(formatValue);
            // Videos can't be fingerprinted with SIFT/pHash.
            if (isVideoFormat(format))
                continue;
            const QString path = LibraryCatalog::filePathFor(md5, format);
            if (path.isEmpty())
                continue;
            files.append({md5, path});
        }
        duckdb_destroy_result(&res);
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return pairs;
    }

    // Phase 1: fingerprint every image in parallel (decode + pHash + SIFT
    // is the expensive part; each worker only touches its own file/data).

    std::atomic<int> fingerprinted{0};
    const int totalFiles = files.size();
    QList<Entry> all = QtConcurrent::blockingMapped(
        files, [&fingerprinted, totalFiles, &progress](const QPair<QString, QString>& file) {
            Entry entry;
            entry.md5 = file.first;
            entry.path = file.second;
            QFile f(file.second);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = f.readAll();
                f.close();
                entry.phash = computePhash(bytes);
                entry.sift = computeSiftBlob(bytes);
            }
            if (progress)
                progress(fingerprinted.fetch_add(1) + 1, totalFiles);
            return entry;
        });
    if (progress)
        progress(totalFiles, totalFiles);

    QList<Entry> entries;
    entries.reserve(all.size());
    for (Entry& e : all) {
        if (e.phash != 0 || !e.sift.isEmpty())
            entries.append(std::move(e));
    }

    // Enrich entries with metadata used to pick the favored copy per pair:
    // more tags wins, and an older post wins when tag counts tie.
    QHash<QString, Entry*> entryIndex;
    for (Entry& e : entries) entryIndex.insert(e.md5, &e);
    try {
        duckdb_result meta{};
        const QString metaSql = QStringLiteral(
            "SELECT i.md5, epoch(i.created_at), "
            "       (SELECT COUNT(*) FROM image_tags it WHERE it.image_id = i.id) "
            "FROM images i "
            "ORDER BY i.md5");
        QString dbError;
        if (conn.query(metaSql, &meta, &dbError)) {
            const idx_t mRows = duckdb_row_count(&meta);
            for (idx_t row = 0; row < mRows; ++row) {
                char* md5Value = duckdb_value_varchar(&meta, 0, row);
                if (!md5Value)
                    continue;
                const QString md5 = QString::fromUtf8(md5Value);
                duckdb_free(md5Value);
                Entry* e = entryIndex.value(md5, nullptr);
                if (!e)
                    continue;
                e->createdAt = duckdb_value_int64(&meta, 1, row);
                e->tagCount = duckdb_value_int64(&meta, 2, row);
            }
            duckdb_destroy_result(&meta);
        }
    } catch (const std::exception&) {
        // Metadata enrichment is best-effort; missing it just degrades the
        // favoring heuristic (falls back to the older post).
    }

    // Phase 2: cheap candidate selection (hamming distance on pHash).

    constexpr int kHammingThreshold = 12;
    QList<QPair<int, int>> candidates;
    const int n = entries.size();
    for (int i = 0; i < n; ++i) {
        const Entry& a = entries.at(i);
        for (int j = i + 1; j < n; ++j) {
            const Entry& b = entries.at(j);
            // Pre-filter by hamming distance when both hashes exist.
            if (a.phash != 0 && b.phash != 0) {
                const int hamming = static_cast<int>(
                    std::bitset<64>(static_cast<quint64>(a.phash ^ b.phash)).count());
                if (hamming > kHammingThreshold)
                    continue;
            }
            candidates.append({i, j});
        }
    }

    // Phase 3: verify candidates in parallel with SIFT + RANSAC.
    // Two stages: a fast pass with the standard keypoint budget; when the
    // verdict is borderline (where alternate versions of an image tend to
    // land), a detailed pass re-extracts many more keypoints on the full
    // resolution files for a sharper yes/no.

    constexpr double kConfirmRatio = 0.55;  ///< clearly a duplicate
    constexpr double kRefineRatio = 0.25;   ///< below this: clearly not
    std::atomic<int> compared{0};
    const int totalCandidates = qMax(1, candidates.size());
    QList<DuplicatePair> verified = QtConcurrent::blockingMapped(
        candidates,
        [&entries, &compared, totalCandidates, totalFiles, &progress](const QPair<int, int>& c) {
            DuplicatePair pair;
            const Entry& a = entries.at(c.first);
            const Entry& b = entries.at(c.second);

            if (!a.sift.isEmpty() && !b.sift.isEmpty()) {
                int inliers = 0;
                double ratio = matchSiftBlobs(a.sift, b.sift, &inliers);
                QString method = QStringLiteral("sift");

                if (ratio >= kRefineRatio && ratio < kConfirmRatio) {
                    // Borderline: rerun with many more keypoints (lower
                    // contrast threshold, no downscale) for a finer verdict.
                    QByteArray detA, detB;
                    QFile fa(a.path);
                    if (fa.open(QIODevice::ReadOnly)) {
                        detA = computeSiftBlob(fa.readAll(), 16000, 0.01, 0);
                        fa.close();
                    }
                    QFile fb(b.path);
                    if (fb.open(QIODevice::ReadOnly)) {
                        detB = computeSiftBlob(fb.readAll(), 16000, 0.01, 0);
                        fb.close();
                    }
                    if (!detA.isEmpty() && !detB.isEmpty()) {
                        int detailedInliers = 0;
                        const double detailed = matchSiftBlobs(detA, detB, &detailedInliers);
                        if (detailedInliers >= 8) {
                            ratio = detailed;
                            inliers = detailedInliers;
                            method = QStringLiteral("sift+detailed");
                        }
                    }
                }

                if (ratio >= kConfirmRatio)
                    pair = DuplicatePair{a.md5, b.md5, ratio, inliers, method};
            } else if (a.phash != 0 && b.phash != 0) {
                // pHash-only match: score by normalized hamming distance.
                const int hamming = static_cast<int>(
                    std::bitset<64>(static_cast<quint64>(a.phash ^ b.phash)).count());
                pair =
                    DuplicatePair{a.md5, b.md5, 1.0 - hamming / 64.0, 0, QStringLiteral("phash")};
            }

            if (progress)
                progress(totalFiles + compared.fetch_add(1) + 1, totalFiles + totalCandidates);
            return pair;
        });

    // Result cleanup (read-only, the catalog is not modified):
    //  - pairs already organized in one parent/child chain are not conflicts
    //  - symmetric pairs ([a,b] == [b,a]) collapse into a single entry
    LibraryCatalog catalog;
    QSet<QString> seenPairs;
    for (const DuplicatePair& p : verified) {
        if (p.md5A.isEmpty() || p.md5B.isEmpty())
            continue;
        if (catalog.inParentChildChain(p.md5A, p.md5B))
            continue;
        QString lo = p.md5A;
        QString hi = p.md5B;
        if (lo > hi)
            std::swap(lo, hi);
        // Pairs the user has already resolved or dismissed (Conflict page)
        // are remembered and are never re-reported.
        if (isResolved(lo, hi))
            continue;
        const QString key = lo + QLatin1Char('|') + hi;
        if (seenPairs.contains(key))
            continue;
        seenPairs.insert(key);

        // Favor the better copy and place it on the left (md5). A copy with
        // more tags wins; on a tie the older post wins (lower created_at).
        Entry* a = entryIndex.value(lo, nullptr);
        Entry* b = entryIndex.value(hi, nullptr);
        const qint64 tagsA = a ? a->tagCount : 0;
        const qint64 tagsB = b ? b->tagCount : 0;
        qint64 createdA = a ? a->createdAt : 0;
        qint64 createdB = b ? b->createdAt : 0;
        bool leftIsLo = true;
        if (tagsA != tagsB) {
            leftIsLo = tagsA > tagsB;
        } else if (createdA != createdB && createdA > 0 && createdB > 0) {
            leftIsLo = createdA < createdB;  // older wins
        }
        // (If both created_at are unknown/0 we keep the original md5 order.)

        DuplicatePair normalized = p;
        normalized.md5A = leftIsLo ? lo : hi;
        normalized.md5B = leftIsLo ? hi : lo;
        pairs.append(normalized);
    }
    if (progress)
        progress(totalFiles + totalCandidates, totalFiles + totalCandidates);

    std::sort(pairs.begin(), pairs.end(),
              [](const DuplicatePair& x, const DuplicatePair& y) { return x.score > y.score; });
    return pairs;
}

bool DuplicateDetector::markResolved(const QString& md5, const QString& md5B, QString* error) {
    QString a = md5.trimmed();
    QString b = md5B.trimmed();
    if (a.isEmpty() || b.isEmpty() || a == b) {
        if (error)
            *error = QStringLiteral("Invalid pair");
        return false;
    }
    if (a > b)
        std::swap(a, b);

    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Database is not open");
        return false;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open database connection");
        return false;
    }

    const QString sql = QStringLiteral(
                            "INSERT INTO resolved_duplicates (md5_a, md5_b) "
                            "VALUES (%1, %2) "
                            "ON CONFLICT (md5_a, md5_b) DO UPDATE SET resolved_at = NOW()")
                            .arg(quoted(a), quoted(b));
    QString dbError;
    if (!conn.execute(sql, &dbError)) {
        if (error)
            *error = dbError;
        return false;
    }
    return true;
}

QPair<QString, QString> DuplicateDetector::retrieveDuplicate(const QString& md5,
                                                             QString* error) const {
    QString a = md5.trimmed();

    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Database is not open");
        return {};
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open database connection");
        return {};
    }

    try {
        duckdb_result meta{};
        const QString sql = QStringLiteral(
                                "SELECT md5_a, md5_b FROM resolved_duplicates "
                                "WHERE md5_a = %1 OR md5_b = %1")
                                .arg(quoted(a));
        QString dbError;
        if (conn.query(sql, &meta, &dbError)) {
            const idx_t mRows = duckdb_row_count(&meta);
            if (mRows > 0) {
                QString qva;
                QString qvb;
                char* va = duckdb_value_varchar(&meta, 0, 0);
                if (va) {
                    qva = QString::fromUtf8(va);
                    duckdb_free(va);
                }
                char* vb = duckdb_value_varchar(&meta, 1, 0);
                if (vb) {
                    qvb = QString::fromUtf8(vb);
                    duckdb_free(vb);
                }
                if (!qva.isEmpty() && !qvb.isEmpty()) {
                    return {qva, qvb};
                }
            }
            duckdb_destroy_result(&meta);
        }
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
    }
    return {};
}

bool DuplicateDetector::removeFromResolved(const QString& md5) const {
    QString a = md5.trimmed();

    Database& db = Database::instance();
    if (!db.isOpen()) {
        return false;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        return false;
    }

    const QString sql = QStringLiteral(
                            "DELETE FROM resolved_duplicates "
                            "WHERE md5_a = %1 OR md5_a = %1")
                            .arg(quoted(a));
    QString dbError;
    if (!conn.execute(sql, &dbError)) {
        return false;
    }
    return true;
}

bool DuplicateDetector::isResolved(const QString& md5, const QString& md5B) const {
    const QString a = md5.trimmed();
    const QString b = md5B.trimmed();
    if (a.isEmpty() || b.isEmpty())
        return false;

    Database& db = Database::instance();
    if (!db.isOpen())
        return false;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return false;

    const QString sql = QStringLiteral(
                            "SELECT 1 FROM resolved_duplicates "
                            "WHERE (md5_a = %1 AND md5_b = %2) "
                            "   OR (md5_a = %2 AND md5_b = %1) "
                            "LIMIT 1")
                            .arg(quoted(a), quoted(b));
    duckdb_result result{};
    QString error;
    if (!conn.query(sql, &result, &error)) {
        duckdb_destroy_result(&result);
        return false;
    }
    const bool found = duckdb_row_count(&result) > 0;
    duckdb_destroy_result(&result);
    return found;
}

bool DuplicateDetector::isMd5Resolved(const QString& md5) const {
    const QString a = md5.trimmed();
    if (a.isEmpty())
        return false;

    Database& db = Database::instance();
    if (!db.isOpen())
        return false;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return false;

    const QString sql = QStringLiteral(
                            "SELECT 1 FROM resolved_duplicates "
                            "WHERE md5_a = %1 OR md5_b = %1 "
                            "LIMIT 1")
                            .arg(quoted(a));
    duckdb_result result{};
    QString error;
    if (!conn.query(sql, &result, &error)) {
        duckdb_destroy_result(&result);
        return false;
    }
    const bool inPair = duckdb_row_count(&result) > 0;
    duckdb_destroy_result(&result);
    if (!inPair)
        return false;

    // Only the NON-survivor counts as resolved: an md5 that is still
    // cataloged is a live image (the kept copy of the pair) and must be
    // treated like any other image by save/favorite/delete operations.
    const LibraryCatalog catalog;
    return !catalog.hasImage(a);
}

}  // namespace Mosaic
