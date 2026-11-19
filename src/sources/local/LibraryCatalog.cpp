#include "LibraryCatalog.h"
#include "LibrarySql.h"
#include "LibraryUtils.h"
#include "../../core/Database.h"
#include "../../core/Paths.h"
#include "../../core/Schema.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

#include <QDebug>
#include <QCryptographicHash>

namespace Mosaic {

namespace {

/**
 * The standard image column layout read by readImageRow(); the images table
 * must be aliased `i`. parent_id holds the parent's internal id, but
 * SourceImage::parentId is exposed in the same id space as remoteId (the
 * parent's remote id, else its internal id) so the viewer can match them.
 */
const QString& imageColumns() {
    static const QString columns = QStringLiteral(
        "i.md5, i.width, i.height, i.file_size, i.format, i.rating, "
        "i.source_id, i.remote_id, i.id, epoch(i.created_at) AS created, "
        "epoch(i.imported_at) AS imported, "
        "(SELECT COALESCE(NULLIF(p.remote_id, 0), p.id) FROM images p "
        " WHERE p.id = i.parent_id) AS parent_display_id, "
        "i.source_url, "
        "EXISTS (SELECT 1 FROM images c WHERE c.parent_id = i.id) AS has_children ");
    return columns;
}

/** Row -> SourceImage for the imageColumns() layout. */
SourceImage readImageRow(duckdb_result* res, idx_t row) {
    SourceImage img;
    char* value = duckdb_value_varchar(res, 0, row);
    img.md5 = value ? QString::fromUtf8(value) : QString();
    if (value)
        duckdb_free(value);

    img.width = duckdb_value_int32(res, 1, row);
    img.height = duckdb_value_int32(res, 2, row);
    img.fileSize = duckdb_value_int64(res, 3, row);

    value = duckdb_value_varchar(res, 4, row);
    img.format = value ? QString::fromUtf8(value) : QString();
    if (value)
        duckdb_free(value);

    value = duckdb_value_varchar(res, 5, row);
    img.rating = value ? QString::fromUtf8(value) : QString();
    if (value)
        duckdb_free(value);

    value = duckdb_value_varchar(res, 6, row);
    img.sourceId = value ? QString::fromUtf8(value) : QString();
    if (value)
        duckdb_free(value);

    img.remoteId = duckdb_value_int64(res, 7, row);
    if (img.remoteId == 0)
        img.remoteId = duckdb_value_int64(res, 8, row);

    img.createdAt = duckdb_value_int64(res, 9, row);
    img.parentId = duckdb_value_int64(res, 11, row);

    value = duckdb_value_varchar(res, 12, row);
    img.sourceUrl = value ? QString::fromUtf8(value) : QString();
    if (value)
        duckdb_free(value);

    img.hasChildren = duckdb_value_boolean(res, 13, row);
    return img;
}

/**
 * Attaches tags (and the favorite flag) to every given image in one query.
 * `md5s` must be pre-quoted via LibrarySql::quoted().
 *
 * Runs on the CALLER'S connection: opening a second connection here raced
 * with the caller's just-committed writes and intermittently returned zero
 * rows (images ending up tag-less in the viewer).
 */
void attachTags(DbConnection& conn, QList<SourceImage>& images, const QStringList& quotedMd5s) {
    if (quotedMd5s.isEmpty())
        return;

    const QString tagSql = QStringLiteral(
                               "SELECT i.md5, t.name, t.category "
                               "FROM image_tags it "
                               "JOIN tags t ON t.id = it.tag_id "
                               "JOIN images i ON i.id = it.image_id "
                               "WHERE i.md5 IN (%1)")
                               .arg(quotedMd5s.join(QStringLiteral(", ")));
    duckdb_result res{};
    QString error;
    if (!conn.query(tagSql, &res, &error)) {
        // A silent failure here leaves every image with zero tags ("no
        // tags" in the viewer) - surface it so it can be diagnosed.
        qWarning().noquote() << "LibraryCatalog::attachTags query failed:" << error << "--"
                             << tagSql;
        return;
    }

    QHash<QString, SourceImage*> byMd5;
    for (auto& img : images) byMd5.insert(img.md5, &img);

    const idx_t rowCount = duckdb_row_count(&res);
    for (idx_t row = 0; row < rowCount; ++row) {
        char* value = duckdb_value_varchar(&res, 0, row);
        const QString md5 = value ? QString::fromUtf8(value) : QString();
        if (value)
            duckdb_free(value);
        auto* img = byMd5.value(md5, nullptr);
        if (!img)
            continue;

        value = duckdb_value_varchar(&res, 1, row);
        const QString name = value ? QString::fromUtf8(value) : QString();
        if (value)
            duckdb_free(value);
        value = duckdb_value_varchar(&res, 2, row);
        const QString category = value ? QString::fromUtf8(value) : QString();
        if (value)
            duckdb_free(value);

        img->tags.append(SourceTag{name, category});
        if (category == QLatin1String("usergroup") && name == QLatin1String("favorite"))
            img->favorite = true;
    }
    duckdb_destroy_result(&res);
}

// feed ranking ("feed:NN")

QMutex& feedRankerMutex() {
    static QMutex mutex;
    return mutex;
}

LibraryCatalog::FeedRanker& feedRankerSlot() {
    static LibraryCatalog::FeedRanker ranker;
    return ranker;
}

LibraryCatalog::FeedRanker currentFeedRanker() {
    QMutexLocker lock(&feedRankerMutex());
    return feedRankerSlot();
}

// Ranked md5s per query string, so pages 2+ slice the order page 1 drew
// (exploration is random: re-ranking per page would repeat / skip posts).
// Page 1 always re-ranks. Small: one entry per recent "feed:" query.
struct FeedRankCache {
    QMutex mutex;
    QHash<QString, QStringList> byQuery;
};

FeedRankCache& feedRankCache() {
    static FeedRankCache cache;
    return cache;
}

constexpr int kFeedRankCacheEntries = 8;

/**
 * Scores every image matching `where` (a "WHERE ..." clause over images i,
 * possibly empty) with the installed ranker. False when no ranker is
 * installed or the catalog query fails.
 */
bool rankLibraryMatches(DbConnection& conn, const QString& queryKey, const QString& where,
                        int level, bool refresh, const SearchProgress& progress,
                        QStringList* out, QString* error) {
    if (!refresh) {
        FeedRankCache& cache = feedRankCache();
        QMutexLocker lock(&cache.mutex);
        auto it = cache.byQuery.constFind(queryKey);
        if (it != cache.byQuery.constEnd()) {
            *out = it.value();
            return true;
        }
    }

    const LibraryCatalog::FeedRanker ranker = currentFeedRanker();
    if (!ranker)
        return false;

    // Lightweight rows: the recommender needs tags (and age / format), not
    // the full column set. Tags come from one join over the same filter
    // rather than a giant md5 IN list.
    auto report = [&progress](const QString& stage, qint64 done, qint64 total) {
        if (progress)
            progress(stage, done, total);
    };

    QElapsedTimer timer;
    timer.start();
    qint64 readMs = 0, tagsMs = 0;

    QList<SourceImage> posts;
    QHash<QString, int> indexByMd5;
    report(QStringLiteral("Reading library"), 0, -1);
    {
        duckdb_result res{};
        if (!conn.query(QStringLiteral("SELECT i.md5, i.format, epoch(i.created_at) "
                                       "FROM images i %1")
                            .arg(where),
                        &res, error))
            return false;
        const idx_t rows = duckdb_row_count(&res);
        posts.reserve(static_cast<int>(rows));
        for (idx_t row = 0; row < rows; ++row) {
            SourceImage img;
            char* value = duckdb_value_varchar(&res, 0, row);
            img.md5 = value ? QString::fromUtf8(value) : QString();
            if (value)
                duckdb_free(value);
            value = duckdb_value_varchar(&res, 1, row);
            img.format = value ? QString::fromUtf8(value) : QString();
            if (value)
                duckdb_free(value);
            img.createdAt = duckdb_value_int64(&res, 2, row);
            if (img.md5.isEmpty() || indexByMd5.contains(img.md5))
                continue;
            indexByMd5.insert(img.md5, posts.size());
            posts.append(img);
        }
        duckdb_destroy_result(&res);
    }
    readMs = timer.restart();
    if (!posts.isEmpty()) {
        report(QStringLiteral("Loading tags"), 0, -1);
        duckdb_result res{};
        if (conn.query(QStringLiteral("SELECT i.md5, t.name, t.category FROM images i "
                                      "JOIN image_tags it ON it.image_id = i.id "
                                      "JOIN tags t ON t.id = it.tag_id %1")
                           .arg(where),
                       &res, error)) {
            const idx_t rows = duckdb_row_count(&res);
            for (idx_t row = 0; row < rows; ++row) {
                char* value = duckdb_value_varchar(&res, 0, row);
                const QString md5 = value ? QString::fromUtf8(value) : QString();
                if (value)
                    duckdb_free(value);
                const int index = indexByMd5.value(md5, -1);
                if (index < 0)
                    continue;
                value = duckdb_value_varchar(&res, 1, row);
                const QString name = value ? QString::fromUtf8(value) : QString();
                if (value)
                    duckdb_free(value);
                value = duckdb_value_varchar(&res, 2, row);
                const QString category = value ? QString::fromUtf8(value) : QString();
                if (value)
                    duckdb_free(value);
                posts[index].tags.append(SourceTag{name, category});
            }
            duckdb_destroy_result(&res);
        }
    }

    tagsMs = timer.restart();
    QStringList ranked = ranker(posts, 1.0 - level / 100.0, progress);
    qInfo().noquote() << QStringLiteral("feed:%1 library search | %2 posts | read %3ms, tags %4ms, "
                                        "ranker %5ms")
                             .arg(level)
                             .arg(posts.size())
                             .arg(readMs)
                             .arg(tagsMs)
                             .arg(timer.elapsed());
    // The ranker may drop posts it cannot score; keep them, at the end.
    if (ranked.size() < posts.size()) {
        const QSet<QString> seen(ranked.cbegin(), ranked.cend());
        for (const SourceImage& img : posts)
            if (!seen.contains(img.md5))
                ranked << img.md5;
    }

    {
        FeedRankCache& cache = feedRankCache();
        QMutexLocker lock(&cache.mutex);
        if (!cache.byQuery.contains(queryKey) && cache.byQuery.size() >= kFeedRankCacheEntries)
            cache.byQuery.clear();
        cache.byQuery.insert(queryKey, ranked);
    }
    *out = ranked;
    return true;
}

}  // namespace

void LibraryCatalog::setFeedRanker(FeedRanker ranker) {
    {
        QMutexLocker lock(&feedRankerMutex());
        feedRankerSlot() = std::move(ranker);
    }
    FeedRankCache& cache = feedRankCache();
    QMutexLocker lock(&cache.mutex);
    cache.byQuery.clear();
}

// queries

SourceResult LibraryCatalog::search(const SourceQuery& query) {
    using namespace LibrarySql;

    SourceResult result;
    result.page = query.page;

    DbConnection conn = Database::instance().connect(&result.error);
    if (!conn.isOpen())
        return result;

    QList<LibrarySql::TagGroup> groups;
    QStringList exclude;
    QString orderTerm;
    splitTagGroups(query.tags, groups, exclude, orderTerm);

    QString sql = QStringLiteral("SELECT ") + imageColumns() + QStringLiteral("FROM images i ");

    QStringList conditions;
    for (const LibrarySql::TagGroup& group : groups) {
        // Metatags: exact md5 / remote id lookups. "favorite" is a plain
        // usergroup tag now (single source of truth).
        if (group.alternatives.size() == 1) {
            const QString& term = group.alternatives.first();
            if (term.startsWith(QLatin1String("md5:"))) {
                conditions << QStringLiteral("i.md5 = %1").arg(quoted(term.mid(4)));
                continue;
            }
            if (term.startsWith(QLatin1String("id:"))) {
                // Never splice user text into SQL: a non-numeric id matches nothing.
                bool ok = false;
                const qint64 id = term.mid(3).toLongLong(&ok);
                conditions << (ok ? QStringLiteral("i.remote_id = %1").arg(id)
                                  : QStringLiteral("FALSE"));
                continue;
            }
        }
        QStringList alternatives;
        alternatives.reserve(group.alternatives.size());
        for (const QString& term : group.alternatives) alternatives << termPredicate(term);
        conditions << QStringLiteral(
                          "EXISTS ("
                          "  SELECT 1 FROM image_tags it "
                          "  JOIN tags t ON t.id = it.tag_id "
                          "  WHERE it.image_id = i.id AND (%1))")
                          .arg(alternatives.join(QStringLiteral(" OR ")));
    }
    if (!exclude.isEmpty()) {
        QStringList excludePredicates;
        excludePredicates.reserve(exclude.size());
        for (const QString& term : exclude) excludePredicates << termPredicate(term);
        if (!excludePredicates.isEmpty()) {
            conditions << QStringLiteral(
                              "NOT EXISTS ("
                              "  SELECT 1 FROM image_tags it "
                              "  JOIN tags t ON t.id = it.tag_id "
                              "  WHERE it.image_id = i.id AND (%1))")
                              .arg(excludePredicates.join(QStringLiteral(" OR ")));
        }
    }
    const QString where =
        conditions.isEmpty()
            ? QString()
            : QStringLiteral("WHERE %1 ").arg(conditions.join(QStringLiteral(" AND ")));

    const int limit = qBound(1, query.limit, 500);
    const int offset = (qMax(1, query.page) - 1) * limit;

    // "feed:NN": the recommender orders every match; this page is a slice
    // of that order. Without a ranker it falls through to the default order.
    QStringList feedOrder;
    bool feedRanked = false;
    if (const int level = feedOrderLevel(orderTerm); level >= 0) {
        QString rankError;
        feedRanked = rankLibraryMatches(conn, query.tags.simplified().toLower(), where, level,
                                        query.page <= 1, query.progress, &feedOrder,
                                        &rankError);
        if (feedRanked && query.progress)
            query.progress(QStringLiteral("Loading page"), 0, -1);
        if (!feedRanked && !rankError.isEmpty()) {
            result.error = rankError;
            return result;
        }
    }

    if (feedRanked) {
        const QStringList slice = feedOrder.mid(offset, limit);
        if (slice.isEmpty()) {
            result.ok = true;
            return result;
        }
        sql += QStringLiteral("WHERE i.md5 IN (%1) ").arg(inList(slice));
    } else {
        sql += where + orderByClause(orderTerm) +
               QStringLiteral(" LIMIT %1 OFFSET %2").arg(limit).arg(offset);
    }

    duckdb_result res{};
    QString queryError;
    if (!conn.query(sql, &res, &queryError)) {
        result.error = queryError;
        return result;
    }

    QStringList md5s;
    const idx_t rowCount = duckdb_row_count(&res);
    md5s.reserve(static_cast<int>(rowCount));
    for (idx_t row = 0; row < rowCount; ++row) {
        SourceImage img = readImageRow(&res, row);
        img.localPath = filePathFor(img.md5, img.format);
        img.status = QStringLiteral("active");
        result.images.append(img);
        md5s << quoted(img.md5);
    }
    duckdb_destroy_result(&res);

    if (feedRanked) {
        // IN (...) returns rows in table order; restore the ranking.
        QHash<QString, int> rank;
        for (int i = 0; i < feedOrder.size() && i < offset + limit; ++i)
            rank.insert(feedOrder.at(i), i);
        std::stable_sort(result.images.begin(), result.images.end(),
                         [&rank](const SourceImage& a, const SourceImage& b) {
                             return rank.value(a.md5) < rank.value(b.md5);
                         });
    }

    // Repair rows imported without dimensions (legacy imports, sources
    // that did not report a size): read the real size from the stored
    // file and fix the catalog so the UI stops showing 0x0.
    for (auto& img : result.images) {
        if (img.width > 0 && img.height > 0)
            continue;
        if (img.localPath.isEmpty() || !QFile::exists(img.localPath))
            continue;
        QImageReader reader(img.localPath);
        const QSize size = reader.size();
        if (size.width() <= 0 || size.height() <= 0)
            continue;  // videos / unknown formats keep their 0x0
        img.width = size.width();
        img.height = size.height();
        QString fixError;
        conn.execute(QStringLiteral("UPDATE images SET width = %1, height = %2 WHERE md5 = %3")
                         .arg(img.width)
                         .arg(img.height)
                         .arg(quoted(img.md5)),
                     &fixError);
    }

    attachTags(conn, result.images, md5s);

    result.ok = true;
    return result;
}

bool LibraryCatalog::lookupByMd5(const QString& md5, SourceImage* out) const {
    using namespace LibrarySql;

    if (md5.isEmpty())
        return false;
    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return false;

    const QString sql = QStringLiteral("SELECT ") + imageColumns() +
                        QStringLiteral("FROM images i WHERE i.md5 = %1 LIMIT 1").arg(quoted(md5));

    duckdb_result res{};
    QString error;
    if (!conn.query(sql, &res, &error))
        return false;
    if (duckdb_row_count(&res) == 0) {
        duckdb_destroy_result(&res);
        return false;
    }
    if (out) {
        *out = readImageRow(&res, 0);
        out->localPath = filePathFor(out->md5, out->format);
        out->status = QStringLiteral("active");
        QList<SourceImage> one{*out};
        attachTags(conn, one, {quoted(md5)});
        *out = one.first();
    }
    duckdb_destroy_result(&res);
    return true;
}

bool LibraryCatalog::hasImage(const QString& md5) const {
    using namespace LibrarySql;

    if (md5.isEmpty())
        return false;
    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return false;
    duckdb_result res{};
    QString error;
    const QString sql =
        QStringLiteral("SELECT 1 FROM images WHERE md5 = %1 LIMIT 1").arg(quoted(md5));
    if (!conn.query(sql, &res, &error))
        return false;
    const bool exists = duckdb_row_count(&res) > 0;
    duckdb_destroy_result(&res);
    return exists;
}

QString LibraryCatalog::md5ForPost(const QString& sourceId, qint64 remoteId) const {
    using namespace LibrarySql;

    // Local imports carry no real post identity.
    if (sourceId.isEmpty() || sourceId == QLatin1String("local") || remoteId <= 0)
        return QString();
    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return QString();
    duckdb_result res{};
    QString error;
    const QString sql = QStringLiteral(
                            "SELECT md5 FROM images "
                            "WHERE source_id = %1 AND remote_id = %2 LIMIT 1")
                            .arg(quoted(sourceId))
                            .arg(remoteId);
    if (!conn.query(sql, &res, &error))
        return QString();
    QString md5;
    if (duckdb_row_count(&res) > 0) {
        char* value = duckdb_value_varchar(&res, 0, 0);
        if (value) {
            md5 = QString::fromUtf8(value);
            duckdb_free(value);
        }
    }
    duckdb_destroy_result(&res);
    return md5;
}

// files

QList<SourceImage> LibraryCatalog::imagesWithTag(const QString& tagName,
                                                 const QString& category) const {
    using namespace LibrarySql;

    QList<SourceImage> images;
    const QString name = tagName.trimmed().toLower();
    if (name.isEmpty())
        return images;

    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return images;

    QString sql = QStringLiteral("SELECT ") + imageColumns() +
                  QStringLiteral(
                      "FROM images i "
                      "WHERE EXISTS ("
                      "  SELECT 1 FROM image_tags it "
                      "  JOIN tags t ON t.id = it.tag_id "
                      "  WHERE it.image_id = i.id AND lower(t.name) = %1")
                      .arg(quoted(name));
    if (!category.isEmpty())
        sql += QStringLiteral(" AND t.category = %2").arg(quoted(category));
    sql += QStringLiteral(")");

    duckdb_result res{};
    QString error;
    if (!conn.query(sql, &res, &error))
        return images;
    const idx_t rowCount = duckdb_row_count(&res);
    images.reserve(static_cast<int>(rowCount));
    QStringList md5s;
    md5s.reserve(static_cast<int>(rowCount));
    for (idx_t row = 0; row < rowCount; ++row) {
        SourceImage img = readImageRow(&res, row);
        img.localPath = filePathFor(img.md5, img.format);
        img.status = QStringLiteral("active");
        images.append(img);
        md5s << quoted(img.md5);
    }
    duckdb_destroy_result(&res);
    if (!images.isEmpty())
        attachTags(conn, images, md5s);
    return images;
}

QString LibraryCatalog::filePathFor(const QString& md5, const QString& format) {
    const QString dir = Paths::libraryDir();
    if (!format.isEmpty()) {
        const QString candidate = dir + QLatin1Char('/') + md5 + QLatin1Char('.') + format;
        if (QFile::exists(candidate))
            return candidate;
    }
    // Format unknown or stale: glob for <md5>.* in the library folder.
    const QStringList matches = QDir(dir).entryList({md5 + QStringLiteral(".*")}, QDir::Files);
    if (!matches.isEmpty())
        return dir + QLatin1Char('/') + matches.first();
    return {};
}

QByteArray LibraryCatalog::fetch(const SourceImage& image, FileKind kind, QString* error) const {
    Q_UNUSED(kind);  // the library only stores the original file
    const QString path =
        !image.localPath.isEmpty() ? image.localPath : filePathFor(image.md5, image.format);
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("Image %1 is not in the library").arg(image.md5);
        qWarning().noquote() << "LibraryCatalog: fetch - image not in library:" << image.md5;
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        qWarning().noquote() << "LibraryCatalog: fetch failed for" << path << "--"
                             << file.errorString();
        return {};
    }
    return file.readAll();
}

bool LibraryCatalog::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                    const std::function<void(qint64, qint64)>& progress,
                                    QString* error) const {
    Q_UNUSED(kind);
    const QString src =
        !image.localPath.isEmpty() ? image.localPath : filePathFor(image.md5, image.format);
    if (src.isEmpty()) {
        if (error)
            *error = QStringLiteral("Image %1 is not in the library").arg(image.md5);
        return false;
    }
    QFile::remove(path);
    if (!QFile::copy(src, path)) {
        if (error)
            *error = QStringLiteral("Cannot copy %1").arg(src);
        qWarning().noquote() << "LibraryCatalog: copy failed" << src << "->" << path;
        return false;
    }
    if (progress)
        progress(QFileInfo(path).size(), QFileInfo(path).size());
    return true;
}

// relationships

SourceImage LibraryCatalog::parentOf(const SourceImage& image, QString* error) {
    using namespace LibrarySql;

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return {};

    // Look the child up by md5: image.remoteId may be a remote post id,
    // not the internal id that parent_id refers to.
    const QString sql = QStringLiteral("SELECT ") + imageColumns() +
                        QStringLiteral(
                            "FROM images i "
                            "WHERE i.id = (SELECT parent_id FROM images WHERE md5 = %1)")
                            .arg(quoted(image.md5));

    duckdb_result res{};
    QString queryError;
    if (!conn.query(sql, &res, &queryError)) {
        if (error)
            *error = queryError;
        return {};
    }
    if (duckdb_row_count(&res) == 0) {
        duckdb_destroy_result(&res);
        if (error)
            *error = QStringLiteral("Image has no parent in the library");
        return {};
    }
    SourceImage parent = readImageRow(&res, 0);
    parent.localPath = filePathFor(parent.md5, parent.format);
    parent.status = QStringLiteral("active");
    duckdb_destroy_result(&res);
    QList<SourceImage> one{parent};
    attachTags(conn, one, {quoted(parent.md5)});
    return one.first();
}

QList<SourceImage> LibraryCatalog::childrenOf(const SourceImage& image, QString* error) {
    using namespace LibrarySql;

    QList<SourceImage> children;
    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return children;

    const QString sql = QStringLiteral("SELECT ") + imageColumns() +
                        QStringLiteral(
                            "FROM images i "
                            "WHERE i.parent_id = (SELECT id FROM images WHERE md5 = %1) "
                            "ORDER BY i.imported_at DESC NULLS LAST")
                            .arg(quoted(image.md5));

    duckdb_result res{};
    QString queryError;
    if (!conn.query(sql, &res, &queryError)) {
        if (error)
            *error = queryError;
        return children;
    }
    const idx_t rowCount = duckdb_row_count(&res);
    children.reserve(static_cast<int>(rowCount));
    QStringList md5s;
    for (idx_t row = 0; row < rowCount; ++row) {
        SourceImage child = readImageRow(&res, row);
        child.localPath = filePathFor(child.md5, child.format);
        child.status = QStringLiteral("active");
        children.append(child);
        md5s << quoted(child.md5);
    }
    duckdb_destroy_result(&res);
    attachTags(conn, children, md5s);
    return children;
}

bool LibraryCatalog::inParentChildChain(const QString& md5A, const QString& md5B) {
    using namespace LibrarySql;

    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return false;

    // Resolve md5 -> (internal id, parent_id).
    auto lookup = [&conn](const QString& md5, qint64& id, qint64& parentId) {
        const QString sql =
            QStringLiteral("SELECT id, parent_id FROM images WHERE md5 = %1").arg(quoted(md5));
        duckdb_result res{};
        QString error;
        if (!conn.query(sql, &res, &error) || duckdb_row_count(&res) == 0) {
            duckdb_destroy_result(&res);
            return false;
        }
        id = duckdb_value_int64(&res, 0, 0);
        parentId = duckdb_value_int64(&res, 1, 0);
        duckdb_destroy_result(&res);
        return true;
    };

    qint64 idA = 0, parentA = 0, idB = 0, parentB = 0;
    if (!lookup(md5A, idA, parentA) || !lookup(md5B, idB, parentB))
        return false;

    // Walk up the parent links from each side; reaching the other image's
    // id means one is an ancestor of the other (same chain). The visited
    // set guards against cycles in bad data.
    auto reaches = [&conn](qint64 startParent, qint64 targetId) {
        QSet<qint64> visited;
        qint64 current = startParent;
        while (current != 0 && !visited.contains(current)) {
            if (current == targetId)
                return true;
            visited.insert(current);
            const QString sql =
                QStringLiteral("SELECT parent_id FROM images WHERE id = %1").arg(current);
            duckdb_result res{};
            QString error;
            if (!conn.query(sql, &res, &error) || duckdb_row_count(&res) == 0) {
                duckdb_destroy_result(&res);
                return false;
            }
            current = duckdb_value_int64(&res, 0, 0);
            duckdb_destroy_result(&res);
        }
        return false;
    };

    return reaches(parentA, idB) || reaches(parentB, idA);
}

// mutation

ImportResult LibraryCatalog::importImage(const SourceImage& meta, const QByteArray& data) {
    using namespace LibrarySql;

    ImportResult result;
    QString md5;
    QString ext;
    if (data.isEmpty()) {
        // Tag-refresh pass: the image is already stored; only update its
        // catalog row and tags. The md5 must match an existing entry.
        if (meta.md5.isEmpty() || !hasImage(meta.md5)) {
            result.error = QStringLiteral("No image data to save");
            return result;
        }
        result.ok = true;
        result.alreadyExisted = true;
        md5 = meta.md5;
        SourceImage existing;
        lookupByMd5(meta.md5, &existing);
        result.filePath = existing.localPath;
        ext = existing.format;
    } else {
        Paths::ensureCreated();

        // Content-addressed name: the MD5 of the bytes is the primary key.
        md5 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());

        ext = meta.format.toLower();
        if (ext.isEmpty() && !meta.fileUrl.isEmpty()) {
            const int dot = meta.fileUrl.lastIndexOf(QLatin1Char('.'));
            if (dot >= 0)
                ext = meta.fileUrl.mid(dot + 1).toLower();
        }
        if (ext.isEmpty())
            ext = QStringLiteral("bin");

        const QString filePath = Paths::libraryDir() + QLatin1Char('/') + md5 + QLatin1Char('.') + ext;

        if (QFile::exists(filePath)) {
            result.ok = true;
            result.alreadyExisted = true;
            result.filePath = filePath;
        } else {
            QSaveFile file(filePath);
            if (!file.open(QIODevice::WriteOnly)) {
                result.error = file.errorString();
                return result;
            }
            file.write(data);
            if (!file.commit()) {
                result.error = file.errorString();
                return result;
            }
            result.filePath = filePath;
        }

        // Ensure the on-disk file timestamps reflect the original creation time
        // when provided by the source (or now when absent).
        const qint64 ts =
            meta.createdAt > 0 ? meta.createdAt : QDateTime::currentDateTime().toSecsSinceEpoch();
        LibraryUtils::setFileTimestamps(result.filePath, ts);
    }

    // Catalog entry. Keep the original creation timestamp from the source;
    // imported_at is stamped now by the schema default.
    DbConnection conn = Database::instance().connect(&result.error);
    if (!conn.isOpen())
        return result;

    // make_timestamp, not to_timestamp: to_timestamp yields TIMESTAMPTZ, and
    // casting that into the TIMESTAMP column shifts it by the session time
    // zone whenever DuckDB is built with ICU. epoch() reads it back as UTC.
    const QString createdAtSql = meta.createdAt > 0
                                     ? QStringLiteral("make_timestamp(%1::BIGINT * 1000000)").arg(meta.createdAt)
                                     : QStringLiteral("NULL");

    // Some sources do not report dimensions - read the real size from the
    // file that was just written so the catalog never stores 0x0.
    int width = meta.width;
    int height = meta.height;
    if (width <= 0 || height <= 0) {
        QImageReader reader(result.filePath);
        const QSize size = reader.size();
        if (size.width() > 0 && size.height() > 0) {
            width = size.width();
            height = size.height();
        }
    }

    const QString sourceId = meta.sourceId.isEmpty() ? QStringLiteral("local") : meta.sourceId;
    // Only real posts carry an identity that parent links can refer to.
    const bool hasPostIdentity = sourceId != QLatin1String("local") && meta.remoteId > 0;

    // Image row, tags and parent links are written as one unit.
    QString dbError;
    DbTransaction tx(conn, &dbError);
    if (!tx.isActive()) {
        result.error = dbError;
        return result;
    }

    // parent_id holds the parent's internal id; resolve the source's remote
    // parent id when that post is already in the library.
    qint64 parentInternalId = 0;
    const bool wantsParent = sourceId != QLatin1String("local") && meta.parentId > 0;
    if (wantsParent) {
        duckdb_result res{};
        const QString sql = QStringLiteral(
                                "SELECT id FROM images "
                                "WHERE source_id = %1 AND remote_id = %2 LIMIT 1")
                                .arg(quoted(sourceId))
                                .arg(meta.parentId);
        if (!conn.query(sql, &res, &dbError)) {
            result.error = dbError;
            return result;
        }
        if (duckdb_row_count(&res) > 0)
            parentInternalId = duckdb_value_int64(&res, 0, 0);
        duckdb_destroy_result(&res);
    }

    // On conflict (re-import, tag refresh) fill in metadata the existing row
    // is missing - e.g. a "local" scan import later resolved remotely - but
    // never overwrite values that are already set.
    const QString upsert =
        QStringLiteral(
            "INSERT INTO images("
            "md5, width, height, file_size, format, rating, "
            "source_id, remote_id, source_url, created_at, parent_id"
            ") VALUES ("
            "%1, %2, %3, %4, %5, %6, %7, %8, %9, %10, %11"
            ") "
            "ON CONFLICT (md5) DO UPDATE SET "
            "updated_at = NOW(), "
            "width = CASE WHEN COALESCE(width, 0) <= 0 THEN excluded.width ELSE width END, "
            "height = CASE WHEN COALESCE(height, 0) <= 0 THEN excluded.height ELSE height END, "
            "file_size = CASE WHEN COALESCE(file_size, 0) <= 0 "
            "                 THEN excluded.file_size ELSE file_size END, "
            "rating = COALESCE(NULLIF(rating, ''), excluded.rating), "
            "source_id = CASE WHEN COALESCE(source_id, '') IN ('', 'local') "
            "                 THEN excluded.source_id ELSE source_id END, "
            "remote_id = CASE WHEN COALESCE(remote_id, 0) = 0 "
            "                 THEN excluded.remote_id ELSE remote_id END, "
            "source_url = COALESCE(NULLIF(source_url, ''), excluded.source_url), "
            "created_at = COALESCE(created_at, excluded.created_at), "
            "parent_id = COALESCE(parent_id, excluded.parent_id)")
            .arg(quoted(md5))
            .arg(width)
            .arg(height)
            .arg(meta.fileSize > 0 ? meta.fileSize : static_cast<qint64>(data.size()))
            .arg(quoted(ext))
            .arg(quoted(meta.rating))
            .arg(quoted(sourceId))
            .arg(meta.remoteId)
            .arg(quoted(meta.sourceUrl))
            .arg(createdAtSql)
            .arg(parentInternalId > 0 ? QString::number(parentInternalId)
                                      : QStringLiteral("NULL"));
    if (!conn.execute(upsert, &dbError)) {
        result.error = dbError;
        return result;
    }

    QStringList parentStatements;
    if (wantsParent && parentInternalId <= 0) {
        // Parent not imported yet: remember the link for when it is.
        parentStatements << QStringLiteral(
                                "INSERT INTO pending_parents(image_id, source_id, remote_parent_id) "
                                "SELECT id, %2, %3 FROM images "
                                "WHERE md5 = %1 AND parent_id IS NULL "
                                "ON CONFLICT (image_id) DO UPDATE SET "
                                "source_id = excluded.source_id, "
                                "remote_parent_id = excluded.remote_parent_id")
                                .arg(quoted(md5), quoted(sourceId))
                                .arg(meta.parentId);
    }
    if (hasPostIdentity) {
        // Adopt children that were imported before this (their parent) post.
        const QString pending = QStringLiteral(
                                    "SELECT image_id FROM pending_parents "
                                    "WHERE source_id = %1 AND remote_parent_id = %2")
                                    .arg(quoted(sourceId))
                                    .arg(meta.remoteId);
        parentStatements << QStringLiteral(
                                "UPDATE images "
                                "SET parent_id = (SELECT id FROM images WHERE md5 = %1) "
                                "WHERE parent_id IS NULL AND md5 <> %1 AND id IN (%2)")
                                .arg(quoted(md5), pending)
                         << QStringLiteral(
                                "DELETE FROM pending_parents "
                                "WHERE source_id = %1 AND remote_parent_id = %2")
                                .arg(quoted(sourceId))
                                .arg(meta.remoteId);
    }
    for (const QString& sql : parentStatements) {
        if (!conn.execute(sql, &dbError)) {
            result.error = dbError;
            return result;
        }
    }

    // All tags in two statements instead of two per tag.
    QStringList tagRows;
    QSet<QString> seenTags;
    for (const SourceTag& tag : meta.tags) {
        const QString name = Schema::normalizeTagName(tag.name);
        if (name.isEmpty())
            continue;
        const QString category =
            tag.category.isEmpty() ? QStringLiteral("general") : tag.category.toLower();
        const QString key = category + QLatin1Char(':') + name;
        if (seenTags.contains(key))
            continue;
        seenTags.insert(key);
        tagRows << QStringLiteral("(%1, %2)").arg(quoted(category), quoted(name));
    }
    if (!tagRows.isEmpty()) {
        const QString values = tagRows.join(QStringLiteral(", "));
        const QString tagInsert = QStringLiteral(
                                      "INSERT INTO tags(category, name) "
                                      "SELECT category, name FROM (VALUES %1) v(category, name) "
                                      "ON CONFLICT (category, name) DO NOTHING")
                                      .arg(values);
        if (!conn.execute(tagInsert, &dbError)) {
            result.error = dbError;
            return result;
        }

        const QString linkInsert = QStringLiteral(
                                       "INSERT INTO image_tags(image_id, tag_id) "
                                       "SELECT i.id, t.id "
                                       "FROM (VALUES %2) v(category, name) "
                                       "JOIN tags t "
                                       "  ON t.category = v.category "
                                       " AND t.name = v.name "
                                       "JOIN images i ON i.md5 = %1 "
                                       "ON CONFLICT (image_id, tag_id) DO NOTHING")
                                       .arg(quoted(md5), values);
        if (!conn.execute(linkInsert, &dbError)) {
            result.error = dbError;
            return result;
        }
    }

    if (!tx.commit(&dbError)) {
        result.error = dbError;
        return result;
    }
    result.ok = true;
    return result;
}

bool LibraryCatalog::removeImage(const QString& md5, QString* error) {
    using namespace LibrarySql;

    // Also guards the file glob below: '*' or '?' in md5 would match others.
    if (!LibraryUtils::isValidMd5(md5)) {
        if (error)
            *error = QStringLiteral("Invalid md5: %1").arg(md5);
        return false;
    }

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return false;

    // Catalog rows first, in one transaction; files only once that commits,
    // so a failure never leaves a row pointing at a deleted file.
    QString dbError;
    DbTransaction tx(conn, &dbError);
    if (!tx.isActive()) {
        if (error)
            *error = dbError;
        return false;
    }

    const QString imageId = QStringLiteral("(SELECT id FROM images WHERE md5 = %1)").arg(quoted(md5));
    const QStringList statements{
        // Children fall back to a pending link so re-importing this post
        // relinks them.
        QStringLiteral("INSERT INTO pending_parents(image_id, source_id, remote_parent_id) "
                       "SELECT c.id, p.source_id, p.remote_id "
                       "FROM images c JOIN images p ON p.id = c.parent_id "
                       "WHERE p.md5 = %1 AND COALESCE(p.remote_id, 0) > 0 "
                       "  AND COALESCE(p.source_id, '') NOT IN ('', 'local') "
                       "ON CONFLICT (image_id) DO NOTHING")
            .arg(quoted(md5)),
        QStringLiteral("UPDATE images SET parent_id = NULL WHERE parent_id = %1").arg(imageId),
        QStringLiteral("DELETE FROM pending_parents WHERE image_id = %1").arg(imageId),
        QStringLiteral("DELETE FROM image_tags WHERE image_id = %1").arg(imageId),
        QStringLiteral("DELETE FROM images WHERE md5 = %1").arg(quoted(md5)),
    };
    for (const QString& sql : statements) {
        if (!conn.execute(sql, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }
    }
    if (!tx.commit(&dbError)) {
        if (error)
            *error = dbError;
        return false;
    }

    const QString dir = Paths::libraryDir();
    const QStringList files = QDir(dir).entryList({md5 + QStringLiteral(".*")}, QDir::Files);
    for (const QString& file : files) {
        if (!QFile::remove(dir + QLatin1Char('/') + file))
            qWarning().noquote() << "LibraryCatalog: could not delete" << file;
    }
    return true;
}

// favorites

bool LibraryCatalog::setFavorite(const QString& md5, bool favorite, QString* error) {
    using namespace LibrarySql;

    DbConnection conn = Database::instance().connect(error);
    if (!conn.isOpen())
        return false;
    QString dbError;
    if (favorite) {
        const QString insertTag = QStringLiteral(
            "INSERT INTO tags(category, name) "
            "VALUES ('usergroup', 'favorite') "
            "ON CONFLICT (category, name) DO NOTHING");
        if (!conn.execute(insertTag, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }
        const QString linkTag = QStringLiteral(
                                    "INSERT INTO image_tags(image_id, tag_id) "
                                    "SELECT i.id, t.id "
                                    "FROM images i "
                                    "JOIN tags t "
                                    "  ON t.category = 'usergroup' "
                                    " AND t.name = 'favorite' "
                                    "WHERE i.md5 = %1 "
                                    "ON CONFLICT (image_id, tag_id) DO NOTHING")
                                    .arg(quoted(md5));
        if (!conn.execute(linkTag, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }
    } else {
        const QString removeTag = QStringLiteral(
                                      "DELETE FROM image_tags "
                                      "WHERE image_id = ("
                                      "    SELECT id FROM images WHERE md5 = %1"
                                      ") "
                                      "AND tag_id IN ("
                                      "    SELECT id FROM tags "
                                      "    WHERE category = 'usergroup' "
                                      "      AND name = 'favorite'"
                                      ")")
                                      .arg(quoted(md5));
        if (!conn.execute(removeTag, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }
    }
    return true;
}

bool LibraryCatalog::isFavorite(const QString& md5) const {
    using namespace LibrarySql;

    if (md5.isEmpty())
        return false;
    DbConnection conn = Database::instance().connect();
    if (!conn.isOpen())
        return false;
    duckdb_result res{};
    QString queryError;
    const QString sql = QStringLiteral(
                            "SELECT 1 "
                            "FROM image_tags it "
                            "JOIN tags t ON t.id = it.tag_id "
                            "JOIN images i ON i.id = it.image_id "
                            "WHERE i.md5 = %1 "
                            "  AND t.category = 'usergroup' "
                            "  AND t.name = 'favorite'")
                            .arg(quoted(md5));
    if (!conn.query(sql, &res, &queryError))
        return false;
    const bool fav = duckdb_row_count(&res) > 0;
    duckdb_destroy_result(&res);
    return fav;
}

}  // namespace Mosaic
