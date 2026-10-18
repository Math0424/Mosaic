#include "LibraryCatalog.h"
#include "LibrarySql.h"
#include "LibraryUtils.h"
#include "../../core/Database.h"
#include "../../core/Paths.h"
#include "../../core/Schema.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QHash>
#include <QSaveFile>

#include <QDebug>
#include <QCryptographicHash>

namespace Mosaic {

namespace {

/** Row -> SourceImage for the standard image query column layout. */
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

    img.remoteId = duckdb_value_int32(res, 7, row);
    if (img.remoteId == 0)
        img.remoteId = duckdb_value_int32(res, 8, row);

    img.createdAt = duckdb_value_int64(res, 9, row);
    img.parentId = duckdb_value_int32(res, 11, row);

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

}  // namespace

// queries

SourceResult LibraryCatalog::search(const SourceQuery& query) {
    using namespace LibrarySql;

    SourceResult result;
    result.page = query.page;

    Database& db = Database::instance();
    if (!db.isOpen()) {
        result.error = QStringLiteral("Local database is not open");
        return result;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        result.error = QStringLiteral("Could not open a database connection");
        return result;
    }

    QList<LibrarySql::TagGroup> groups;
    QStringList exclude;
    QString orderTerm;
    splitTagGroups(query.tags, groups, exclude, orderTerm);

    QString sql = QStringLiteral(
        "SELECT i.md5, i.width, i.height, i.file_size, i.format, i.rating, "
        "       i.source_id, i.remote_id, i.id, epoch(i.created_at) AS created, "
        "       epoch(i.imported_at) AS imported, i.parent_id, i.source_url, "
        "       EXISTS (SELECT 1 FROM images c WHERE c.parent_id = i.id) AS has_children "
        "FROM images i ");

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
                conditions << QStringLiteral("i.remote_id = %1").arg(term.mid(3));
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
    if (!conditions.isEmpty())
        sql += QStringLiteral("WHERE %1 ").arg(conditions.join(QStringLiteral(" AND ")));

    const int limit = qBound(1, query.limit, 500);
    const int offset = (qMax(1, query.page) - 1) * limit;
    sql += orderByClause(orderTerm) + QStringLiteral(" LIMIT %1 OFFSET %2").arg(limit).arg(offset);

    try {
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
    } catch (const std::exception& e) {
        result.error = QString::fromUtf8(e.what());
        return result;
    }
}

bool LibraryCatalog::lookupByMd5(const QString& md5, SourceImage* out) const {
    using namespace LibrarySql;

    if (md5.isEmpty())
        return false;
    Database& db = Database::instance();
    if (!db.isOpen())
        return false;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return false;

    const QString sql = QStringLiteral(
                            "SELECT i.md5, i.width, i.height, i.file_size, i.format, i.rating, "
                            "       i.source_id, i.remote_id, i.id, epoch(i.created_at), "
                            "       epoch(i.imported_at), i.parent_id, i.source_url, "
                            "       EXISTS (SELECT 1 FROM images c WHERE c.parent_id = i.id) "
                            "FROM images i WHERE i.md5 = %1 LIMIT 1")
                            .arg(quoted(md5));

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
    Database& db = Database::instance();
    if (!db.isOpen())
        return false;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return false;
    try {
        duckdb_result res{};
        QString error;
        const QString sql =
            QStringLiteral("SELECT 1 FROM images WHERE md5 = %1 LIMIT 1").arg(quoted(md5));
        if (!conn.query(sql, &res, &error))
            return false;
        const bool exists = duckdb_row_count(&res) > 0;
        duckdb_destroy_result(&res);
        return exists;
    } catch (const std::exception&) {
        return false;
    }
}

// files

QList<SourceImage> LibraryCatalog::imagesWithTag(const QString& tagName,
                                                 const QString& category) const {
    using namespace LibrarySql;

    QList<SourceImage> images;
    const QString name = tagName.trimmed().toLower();
    if (name.isEmpty())
        return images;

    Database& db = Database::instance();
    if (!db.isOpen())
        return images;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return images;

    QString sql = QStringLiteral(
        "SELECT i.md5, i.width, i.height, i.file_size, i.format, i.rating, "
        "       i.source_id, i.remote_id, i.id, epoch(i.created_at), "
        "       epoch(i.imported_at), i.parent_id, i.source_url, "
        "       EXISTS (SELECT 1 FROM images c WHERE c.parent_id = i.id) "
        "FROM images i "
        "WHERE EXISTS ("
        "  SELECT 1 FROM image_tags it "
        "  JOIN tags t ON t.id = it.tag_id "
        "  WHERE it.image_id = i.id AND lower(t.name) = %1");
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

    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Local database is not open");
        return {};
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open a database connection");
        return {};
    }

    const QString sql = QStringLiteral(
                            "SELECT "
                            "p.md5, p.width, p.height, p.file_size, p.format, p.rating, "
                            "p.source_url, epoch(p.created_at), epoch(p.imported_at), "
                            "p.parent_id, "
                            "EXISTS (SELECT 1 FROM images c "
                            "        WHERE c.parent_id = p.id AND c.source_id = p.source_id) "
                            "FROM images i "
                            "JOIN images p ON p.id = i.parent_id "
                            "WHERE i.id = %1")
                            .arg(QString::number(image.remoteId));

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
    parent.hasChildren = duckdb_value_boolean(&res, 10, 0);
    parent.localPath = filePathFor(parent.md5, parent.format);
    parent.status = QStringLiteral("active");
    duckdb_destroy_result(&res);
    return parent;
}

QList<SourceImage> LibraryCatalog::childrenOf(const SourceImage& image, QString* error) {
    using namespace LibrarySql;

    QList<SourceImage> children;
    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Local database is not open");
        return children;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open a database connection");
        return children;
    }

    const QString sql = QStringLiteral(
                            "SELECT "
                            "c.md5, c.width, c.height, c.file_size, c.format, c.rating, "
                            "c.source_url, epoch(c.created_at), epoch(c.imported_at), "
                            "c.parent_id, "
                            "EXISTS (SELECT 1 FROM images gc WHERE gc.parent_id = c.id) "
                            "FROM images c "
                            "JOIN images p ON p.id = c.parent_id "
                            "WHERE p.md5 = %1 "
                            "ORDER BY c.imported_at DESC NULLS LAST")
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
    for (idx_t row = 0; row < rowCount; ++row) {
        SourceImage child = readImageRow(&res, row);
        child.localPath = filePathFor(child.md5, child.format);
        child.status = QStringLiteral("active");
        children.append(child);
    }
    duckdb_destroy_result(&res);
    return children;
}

bool LibraryCatalog::inParentChildChain(const QString& md5A, const QString& md5B) {
    using namespace LibrarySql;

    Database& db = Database::instance();
    if (!db.isOpen())
        return false;
    DbConnection conn = db.connection();
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
    Database& db = Database::instance();
    if (!db.isOpen()) {
        result.error = QStringLiteral("Database is not open");
        return result;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        result.error = QStringLiteral("Could not open database connection");
        return result;
    }

    const QString createdAtSql = meta.createdAt > 0
                                     ? QStringLiteral("to_timestamp(%1)").arg(meta.createdAt)
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

    const QString upsert =
        QStringLiteral(
            "INSERT INTO images("
            "md5, width, height, file_size, format, rating, "
            "source_id, remote_id, source_url, created_at"
            ") VALUES ("
            "%1, %2, %3, %4, %5, %6, %7, %8, %9, %10"
            ") "
            "ON CONFLICT (md5) DO UPDATE "
            "SET updated_at = NOW()")
            .arg(quoted(md5))
            .arg(width)
            .arg(height)
            .arg(meta.fileSize > 0 ? meta.fileSize : static_cast<qint64>(data.size()))
            .arg(quoted(ext))
            .arg(quoted(meta.rating))
            .arg(quoted(meta.sourceId.isEmpty() ? QStringLiteral("local") : meta.sourceId))
            .arg(meta.remoteId)
            .arg(quoted(meta.sourceUrl))
            .arg(createdAtSql);
    QString dbError;
    if (!conn.execute(upsert, &dbError)) {
        result.error = dbError;
        return result;
    }

    for (const SourceTag& tag : meta.tags) {
        const QString name = Schema::normalizeTagName(tag.name);
        if (name.isEmpty())
            continue;

        const QString category =
            tag.category.isEmpty() ? QStringLiteral("general") : tag.category.toLower();

        const QString tagInsert = QStringLiteral(
                                      "INSERT INTO tags(category, name) "
                                      "VALUES (%1, %2) "
                                      "ON CONFLICT (category, name) DO NOTHING")
                                      .arg(quoted(category), quoted(name));
        if (!conn.execute(tagInsert, &dbError)) {
            result.error = dbError;
            return result;
        }

        const QString linkInsert = QStringLiteral(
                                       "INSERT INTO image_tags(image_id, tag_id) "
                                       "SELECT i.id, t.id "
                                       "FROM images i "
                                       "JOIN tags t "
                                       "  ON t.category = %2 "
                                       " AND t.name = %3 "
                                       "WHERE i.md5 = %1 "
                                       "ON CONFLICT (image_id, tag_id) DO NOTHING")
                                       .arg(quoted(md5), quoted(category), quoted(name));
        if (!conn.execute(linkInsert, &dbError)) {
            result.error = dbError;
            return result;
        }
    }

    result.ok = true;
    return result;
}

bool LibraryCatalog::removeImage(const QString& md5, QString* error) {
    using namespace LibrarySql;

    if (md5.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty md5");
        return false;
    }

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

    try {
        QString dbError;
        const QString dir = Paths::libraryDir();
        const QStringList files = QDir(dir).entryList({md5 + QStringLiteral(".*")}, QDir::Files);
        for (const QString& file : files) QFile::remove(dir + QLatin1Char('/') + file);

        const QString deleteTags = QStringLiteral(
                                       "DELETE FROM image_tags "
                                       "WHERE image_id = ("
                                       "    SELECT id FROM images WHERE md5 = %1"
                                       ")")
                                       .arg(quoted(md5));
        if (!conn.execute(deleteTags, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }

        const QString deleteImage =
            QStringLiteral("DELETE FROM images WHERE md5 = %1").arg(quoted(md5));
        if (!conn.execute(deleteImage, &dbError)) {
            if (error)
                *error = dbError;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}

// favorites

bool LibraryCatalog::setFavorite(const QString& md5, bool favorite, QString* error) {
    using namespace LibrarySql;

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
    try {
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
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}

bool LibraryCatalog::isFavorite(const QString& md5) const {
    using namespace LibrarySql;

    Database& db = Database::instance();
    if (!db.isOpen() || md5.isEmpty())
        return false;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return false;
    try {
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
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace Mosaic
