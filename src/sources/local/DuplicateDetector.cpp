#include "DuplicateDetector.h"

#include "LibraryCatalog.h"
#include "LibrarySql.h"
#include "../../core/Database.h"
#include "../../core/Paths.h"
#include "../../core/Shutdown.h"
#include "../../media/ImageFingerprint.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <mutex>
#include <numeric>
#include <vector>

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

/** Lazily computed high-detail SIFT blob, shared by every pair it is in. */
struct DetailedSift {
    std::once_flag once;
    QByteArray blob;
};

/**
 * Memory-maps a file for the fingerprint functions: these run on every core
 * at once, and mapping keeps the encoded bytes out of the heap (the OS pages
 * them in and drops them under pressure).
 */
class MappedFile {
   public:
    explicit MappedFile(const QString& path) : m_file(path) {
        if (!m_file.open(QIODevice::ReadOnly) || m_file.size() <= 0)
            return;
        m_data = m_file.map(0, m_file.size());
        if (m_data)
            m_bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(m_data),
                                              static_cast<qsizetype>(m_file.size()));
        else
            m_bytes = m_file.readAll();  // mapping unsupported: fall back
    }
    ~MappedFile() {
        m_bytes.clear();  // drop the raw-data view before unmapping
        if (m_data)
            m_file.unmap(m_data);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const QByteArray& bytes() const { return m_bytes; }

   private:
    QFile m_file;
    uchar* m_data = nullptr;
    QByteArray m_bytes;
};

}  // namespace

QList<DuplicatePair> DuplicateDetector::findDuplicates(
    const std::function<void(int, int)>& progress, QString* error) {
    QList<DuplicatePair> pairs;

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return pairs;

    // Collect the fingerprintable files first (cheap catalog query).
    QList<QPair<QString, QString>> files;  // md5, on-disk path
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

    // Phase 1: pHash every image in parallel. This is the only per-image
    // work for the whole library; SIFT (the expensive part) is deferred
    // until the pHash pre-filter has picked the few candidate images.

    const int totalFiles = files.size();
    std::atomic<int> hashed{0};
    const QList<qint64> phashes = QtConcurrent::blockingMapped(
        files, [&hashed, totalFiles, &progress](const QPair<QString, QString>& file) {
            if (Shutdown::requested())
                return qint64(0);
            const qint64 phash = computePhash(MappedFile(file.second).bytes());
            if (progress)
                progress(hashed.fetch_add(1) + 1, totalFiles);
            return phash;
        });
    if (Shutdown::requested())
        return pairs;

    QList<Entry> entries;
    entries.reserve(totalFiles);
    for (int i = 0; i < totalFiles; ++i) {
        if (phashes.at(i) == 0)
            continue;  // unreadable / undecodable
        Entry entry;
        entry.md5 = files.at(i).first;
        entry.path = files.at(i).second;
        entry.phash = phashes.at(i);
        entries.append(std::move(entry));
    }

    // Enrich entries with metadata used to pick the favored copy per pair:
    // more tags wins, and an older post wins when tag counts tie.
    QHash<QString, Entry*> entryIndex;
    for (Entry& e : entries) entryIndex.insert(e.md5, &e);
    // Best-effort: if this query fails the favoring heuristic just falls
    // back to the md5 order.
    duckdb_result meta{};
    const QString metaSql = QStringLiteral(
        "SELECT i.md5, epoch(i.created_at), "
        "       (SELECT COUNT(*) FROM image_tags it WHERE it.image_id = i.id) "
        "FROM images i "
        "ORDER BY i.md5");
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

    // Phase 2: cheap candidate selection (hamming distance on pHash).

    // Still all-pairs, but one row per task across every core; rows are
    // flattened in order so the candidate list stays deterministic.
    constexpr int kHammingThreshold = 12;
    const int n = entries.size();
    std::vector<quint64> hashes(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) hashes[static_cast<size_t>(i)] = static_cast<quint64>(entries.at(i).phash);
    QList<int> rows(n);
    std::iota(rows.begin(), rows.end(), 0);
    const QList<QList<QPair<int, int>>> rowCandidates =
        QtConcurrent::blockingMapped(rows, [&hashes, n](int i) {
            QList<QPair<int, int>> found;
            if (Shutdown::requested())
                return found;
            const quint64 a = hashes[static_cast<size_t>(i)];
            for (int j = i + 1; j < n; ++j) {
                const int hamming =
                    static_cast<int>(std::bitset<64>(a ^ hashes[static_cast<size_t>(j)]).count());
                if (hamming <= kHammingThreshold)
                    found.append({i, j});
            }
            return found;
        });
    if (Shutdown::requested())
        return pairs;
    QList<QPair<int, int>> candidates;
    for (const auto& row : rowCandidates) candidates += row;

    // SIFT only the images that appear in at least one candidate pair -
    // typically a small fraction of the library.
    QSet<int> needSift;
    for (const QPair<int, int>& c : candidates) {
        needSift.insert(c.first);
        needSift.insert(c.second);
    }
    const QList<int> siftIndices(needSift.begin(), needSift.end());
    const int totalSift = siftIndices.size();
    const int totalCandidates = qMax(1, candidates.size());
    const int totalWork = totalFiles + totalSift + totalCandidates;
    Entry* entryData = entries.data();  // detach once; workers write distinct slots
    std::atomic<int> sifted{0};
    QtConcurrent::blockingMap(
        siftIndices, [entryData, &sifted, totalFiles, totalWork, &progress](int index) {
            if (Shutdown::requested())
                return;
            entryData[index].sift = computeSiftBlob(MappedFile(entryData[index].path).bytes());
            if (progress)
                progress(totalFiles + sifted.fetch_add(1) + 1, totalWork);
        });

    // Borderline pairs re-extract a high-detail blob; an image can sit in
    // several borderline pairs, so each blob is computed at most once.
    std::vector<DetailedSift> detailed(static_cast<size_t>(n));
    auto detailedBlob = [&entries, &detailed](int index) -> const QByteArray& {
        DetailedSift& slot = detailed[static_cast<size_t>(index)];
        std::call_once(slot.once, [&]() {
            slot.blob = computeSiftBlob(MappedFile(entries.at(index).path).bytes(), 16000, 0.01, 0);
        });
        return slot.blob;
    };

    // Phase 3: verify candidates in parallel with SIFT + RANSAC.
    // Two stages: a fast pass with the standard keypoint budget; when the
    // verdict is borderline (where alternate versions of an image tend to
    // land), a detailed pass re-extracts many more keypoints on the full
    // resolution files for a sharper yes/no.

    constexpr double kConfirmRatio = 0.55;  ///< clearly a duplicate
    constexpr double kRefineRatio = 0.25;   ///< below this: clearly not
    std::atomic<int> compared{0};
    QList<DuplicatePair> verified = QtConcurrent::blockingMapped(
        candidates, [&entries, &detailedBlob, &compared, totalFiles, totalSift, totalWork,
                     &progress](const QPair<int, int>& c) {
            DuplicatePair pair;
            if (Shutdown::requested())
                return pair;
            const Entry& a = entries.at(c.first);
            const Entry& b = entries.at(c.second);

            if (!a.sift.isEmpty() && !b.sift.isEmpty()) {
                int inliers = 0;
                double ratio = matchSiftBlobs(a.sift, b.sift, &inliers);
                QString method = QStringLiteral("sift");

                if (ratio >= kRefineRatio && ratio < kConfirmRatio) {
                    // Borderline: rerun with many more keypoints (lower
                    // contrast threshold, no downscale) for a finer verdict.
                    const QByteArray& detA = detailedBlob(c.first);
                    const QByteArray& detB = detailedBlob(c.second);
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
                progress(totalFiles + totalSift + compared.fetch_add(1) + 1, totalWork);
            return pair;
        });

    if (Shutdown::requested())
        return pairs;

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
        progress(totalWork, totalWork);

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

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return false;

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

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return {};

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
    return {};
}

bool DuplicateDetector::removeFromResolved(const QString& md5) const {
    QString a = md5.trimmed();

    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return false;

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

    DbConnection conn = Database::instance().connect();
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

    DbConnection conn = Database::instance().connect();
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
