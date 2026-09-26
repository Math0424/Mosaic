#include "LibraryTagService.h"

#include "LibrarySql.h"
#include "../../core/Database.h"
#include "../../core/Schema.h"

namespace Mosaic {

using namespace LibrarySql;

QStringList LibraryTagService::allTagNames() const {
    QStringList names;
    Database& db = Database::instance();
    if (!db.isOpen())
        return names;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return names;
    duckdb_result res{};
    QString error;
    if (!conn.query(QStringLiteral("SELECT DISTINCT lower(name) FROM tags ORDER BY 1"), &res,
                    &error)) {
        return names;
    }
    const idx_t rowCount = duckdb_row_count(&res);
    names.reserve(static_cast<int>(rowCount));
    for (idx_t row = 0; row < rowCount; ++row) {
        char* value = duckdb_value_varchar(&res, 0, row);
        if (value) {
            names.append(QString::fromUtf8(value));
            duckdb_free(value);
        }
    }
    duckdb_destroy_result(&res);
    return names;
}

QHash<QString, QString> LibraryTagService::allTagCategories() const {
    QHash<QString, QString> categories;
    Database& db = Database::instance();
    if (!db.isOpen())
        return categories;
    DbConnection conn = db.connection();
    if (!conn.isOpen())
        return categories;
    duckdb_result res{};
    QString error;
    if (!conn.query(QStringLiteral("SELECT DISTINCT lower(name), category FROM tags"), &res,
                    &error)) {
        return categories;
    }
    const idx_t rowCount = duckdb_row_count(&res);
    categories.reserve(static_cast<int>(rowCount));
    for (idx_t row = 0; row < rowCount; ++row) {
        char* nameValue = duckdb_value_varchar(&res, 0, row);
        char* categoryValue = duckdb_value_varchar(&res, 1, row);
        if (nameValue && categoryValue)
            categories.insert(QString::fromUtf8(nameValue), QString::fromUtf8(categoryValue));
        if (nameValue)
            duckdb_free(nameValue);
        if (categoryValue)
            duckdb_free(categoryValue);
    }
    duckdb_destroy_result(&res);
    return categories;
}

QString LibraryTagService::addTag(const QString& md5, const QString& tagName,
                                  QString* error) const {
    QString name = Schema::normalizeTagName(tagName);

    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty tag name");
        return {};
    }

    // Optional category prefix: "species:fox" creates the new tag as a
    // species tag instead of the usergroup default. Only applies when the
    // tag is genuinely new - an existing tag always keeps its stored
    // category so re-adding can never silently re-classify it.
    QString requestedCategory;
    const int colon = name.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString prefix = name.left(colon);
        if (Schema::isCategoryName(prefix)) {
            requestedCategory = prefix;
            name = name.mid(colon + 1);
        }
        // Unknown prefixes stay part of the name (e.g. "scp-1471-a" has no
        // colon, but something like "foo:bar" with foo not a category is
        // treated as a literal tag name).
    }

    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty tag name");
        return {};
    }
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
        QString category =
            requestedCategory.isEmpty() ? QStringLiteral("usergroup") : requestedCategory;
        QString dbError;

        // Reuse the existing tag row's category when the tag is known.
        const QString categorySql =
            QStringLiteral("SELECT category FROM tags WHERE lower(name) = %1 LIMIT 1")
                .arg(quoted(name));
        duckdb_result categoryResult{};
        if (conn.query(categorySql, &categoryResult, &dbError)) {
            if (duckdb_row_count(&categoryResult) > 0) {
                char* value = duckdb_value_varchar(&categoryResult, 0, 0);
                if (value) {
                    category = QString::fromUtf8(value);
                    duckdb_free(value);
                }
            }
            duckdb_destroy_result(&categoryResult);
        }

        const QString insertTag = QStringLiteral(
                                      "INSERT INTO tags(category, name) "
                                      "VALUES (%1, %2) "
                                      "ON CONFLICT (category, name) DO NOTHING")
                                      .arg(quoted(category), quoted(name));
        if (!conn.execute(insertTag, &dbError)) {
            if (error)
                *error = dbError;
            return {};
        }

        const QString linkTag = QStringLiteral(
                                    "INSERT INTO image_tags(image_id, tag_id) "
                                    "SELECT i.id, t.id "
                                    "FROM images i "
                                    "JOIN tags t "
                                    "  ON t.category = %2 "
                                    " AND t.name = %3 "
                                    "WHERE i.md5 = %1 "
                                    "ON CONFLICT (image_id, tag_id) DO NOTHING")
                                    .arg(quoted(md5), quoted(category), quoted(name));
        if (!conn.execute(linkTag, &dbError)) {
            if (error)
                *error = dbError;
            return {};
        }

        return category;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return {};
    }
}

bool LibraryTagService::removeTag(const QString& md5, const QString& tagName,
                                  QString* error) const {
    const QString name = tagName.trimmed().toLower();
    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty tag name");
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
        const QString sql = QStringLiteral(
                                "DELETE FROM image_tags "
                                "WHERE image_id = ("
                                "    SELECT id FROM images WHERE md5 = %1"
                                ") "
                                "AND tag_id IN ("
                                "    SELECT id FROM tags WHERE lower(name) = %2"
                                ")")
                                .arg(quoted(md5), quoted(name));
        if (!conn.execute(sql, &dbError)) {
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

int LibraryTagService::mergeTags(const QString& primary, const QStringList& secondaries,
                                 QString* error) const {
    const QString primaryName = primary.trimmed().toLower();
    if (primaryName.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty primary tag");
        return -1;
    }

    QStringList secondaryNames;
    for (const QString& s : secondaries) {
        const QString name = s.trimmed().toLower();
        if (!name.isEmpty() && name != primaryName)
            secondaryNames << name;
    }
    secondaryNames.removeDuplicates();
    if (secondaryNames.isEmpty()) {
        if (error)
            *error = QStringLiteral("No secondary tags to merge");
        return -1;
    }

    Database& db = Database::instance();
    if (!db.isOpen()) {
        if (error)
            *error = QStringLiteral("Database is not open");
        return -1;
    }
    DbConnection conn = db.connection();
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Could not open database connection");
        return -1;
    }

    try {
        QString dbError;

        // Reuse the primary tag's existing category when it is already known,
        // otherwise file it under "general".
        QString category = QStringLiteral("general");
        {
            const QString sql =
                QStringLiteral("SELECT category FROM tags WHERE lower(name) = %1 LIMIT 1")
                    .arg(quoted(primaryName));
            duckdb_result res{};
            if (conn.query(sql, &res, &dbError)) {
                if (duckdb_row_count(&res) > 0) {
                    char* value = duckdb_value_varchar(&res, 0, 0);
                    if (value) {
                        category = QString::fromUtf8(value);
                        duckdb_free(value);
                    }
                }
                duckdb_destroy_result(&res);
            }
        }

        const QString insertTag = QStringLiteral(
                                      "INSERT INTO tags(category, name) "
                                      "VALUES (%1, %2) "
                                      "ON CONFLICT (category, name) DO NOTHING")
                                      .arg(quoted(category), quoted(primaryName));
        if (!conn.execute(insertTag, &dbError)) {
            if (error)
                *error = dbError;
            return -1;
        }

        int movedImages = 0;
        for (const QString& name : secondaryNames) {
            // How many images carry this secondary tag (for the status line).
            const QString countSql = QStringLiteral(
                                         "SELECT COUNT(DISTINCT it.image_id) "
                                         "FROM image_tags it "
                                         "JOIN tags t ON t.id = it.tag_id "
                                         "WHERE lower(t.name) = %1")
                                         .arg(quoted(name));
            duckdb_result res{};
            if (conn.query(countSql, &res, &dbError)) {
                if (duckdb_row_count(&res) > 0)
                    movedImages += static_cast<int>(duckdb_value_int64(&res, 0, 0));
                duckdb_destroy_result(&res);
            }

            // Re-link every image tagged with the secondary to the primary.
            const QString linkSql = QStringLiteral(
                                        "INSERT INTO image_tags(image_id, tag_id) "
                                        "SELECT it.image_id, p.id "
                                        "FROM image_tags it "
                                        "JOIN tags s "
                                        "  ON s.id = it.tag_id "
                                        " AND lower(s.name) = %2 "
                                        "JOIN tags p "
                                        "  ON lower(p.name) = %1 "
                                        "ON CONFLICT (image_id, tag_id) DO NOTHING")
                                        .arg(quoted(primaryName), quoted(name));
            if (!conn.execute(linkSql, &dbError)) {
                if (error)
                    *error = dbError;
                return -1;
            }

            // Unlink and delete the secondary tag itself.
            const QString unlinkSql = QStringLiteral(
                                          "DELETE FROM image_tags "
                                          "WHERE tag_id IN ("
                                          "    SELECT id FROM tags WHERE lower(name) = %1"
                                          ")")
                                          .arg(quoted(name));
            if (!conn.execute(unlinkSql, &dbError)) {
                if (error)
                    *error = dbError;
                return -1;
            }

            const QString deleteTagSql =
                QStringLiteral("DELETE FROM tags WHERE lower(name) = %1").arg(quoted(name));
            if (!conn.execute(deleteTagSql, &dbError)) {
                if (error)
                    *error = dbError;
                return -1;
            }
        }

        return movedImages;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return -1;
    }
}

}  // namespace Mosaic
