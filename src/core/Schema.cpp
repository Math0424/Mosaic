#include "Schema.h"

#include "../sources/local/LibrarySql.h"

#include <QString>
#include <QStringList>

#include <QRegularExpression>

namespace Mosaic::Schema {

using LibrarySql::quoted;

QString categoryToString(TagCategory c) {
    switch (c) {
        case TagCategory::General:
            return QStringLiteral("general");

        case TagCategory::Artist:
            return QStringLiteral("artist");

        case TagCategory::Character:
            return QStringLiteral("character");

        case TagCategory::Species:
            return QStringLiteral("species");

        case TagCategory::Meta:
            return QStringLiteral("meta");

        case TagCategory::Model:
            return QStringLiteral("model");

        case TagCategory::UserGroup:
            return QStringLiteral("usergroup");
    }

    return QStringLiteral("general");
}

bool isCategoryName(const QString& name) {
    const QString lower = name.toLower();
    return lower == QLatin1String("general") || lower == QLatin1String("artist") ||
           lower == QLatin1String("character") || lower == QLatin1String("species") ||
           lower == QLatin1String("meta") || lower == QLatin1String("model") ||
           lower == QLatin1String("usergroup");
}

QString normalizeTagName(const QString& name) {
    QString cleaned = name;
    cleaned.replace(QLatin1Char(','), QLatin1Char(' '));
    cleaned = cleaned.simplified().toLower();
    cleaned.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
    // Strip leading/trailing separators left over from "tag," or ", tag".
    cleaned.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    return cleaned;
}

bool sanitizeTags(DbConnection& conn, QString* error) {
    if (!conn.isOpen()) {
        if (error)
            *error = QStringLiteral("Tag sanitize failed: connection not open");
        return false;
    }

    duckdb_result res{};
    QString dbError;
    const QString selectSql = QStringLiteral("SELECT id, category, name FROM tags");
    if (!conn.query(selectSql, &res, &dbError)) {
        if (error)
            *error = dbError;
        return false;
    }

    // A tag stored un-normalized ("League of Legends") is renamed to its
    // normalized form ("league_of_legends"), merging into that tag when it
    // already exists. Never split into words: "of" is not a tag.
    struct Repair {
        qint64 id;
        QString category;
        QString normalized;
    };
    QList<Repair> repairs;
    const idx_t rowCount = duckdb_row_count(&res);
    for (idx_t row = 0; row < rowCount; ++row) {
        char* idValue = duckdb_value_varchar(&res, 0, row);
        char* categoryValue = duckdb_value_varchar(&res, 1, row);
        char* nameValue = duckdb_value_varchar(&res, 2, row);
        if (idValue && categoryValue && nameValue) {
            const QString raw = QString::fromUtf8(nameValue);
            const QString normalized = normalizeTagName(raw);
            if (normalized != raw)
                repairs.append({QString::fromUtf8(idValue).toLongLong(),
                                QString::fromUtf8(categoryValue), normalized});
        }
        if (idValue)
            duckdb_free(idValue);
        if (categoryValue)
            duckdb_free(categoryValue);
        if (nameValue)
            duckdb_free(nameValue);
    }
    duckdb_destroy_result(&res);

    for (const Repair& repair : repairs) {
        QStringList statements;
        if (!repair.normalized.isEmpty()) {
            statements << QStringLiteral(
                              "INSERT INTO tags(category, name) VALUES (%1, %2) "
                              "ON CONFLICT (category, name) DO NOTHING")
                              .arg(quoted(repair.category), quoted(repair.normalized))
                       << QStringLiteral(
                              "INSERT INTO image_tags(image_id, tag_id) "
                              "SELECT it.image_id, t.id FROM image_tags it "
                              "JOIN tags t ON t.category = %2 AND t.name = %3 "
                              "WHERE it.tag_id = %1 "
                              "ON CONFLICT (image_id, tag_id) DO NOTHING")
                              .arg(QString::number(repair.id), quoted(repair.category),
                                   quoted(repair.normalized));
        }
        statements << QStringLiteral("DELETE FROM image_tags WHERE tag_id = %1").arg(repair.id)
                   << QStringLiteral("DELETE FROM tags WHERE id = %1").arg(repair.id);
        // One transaction per tag: a failed repair rolls back cleanly and
        // the old tag stays intact for the next startup to retry.
        DbTransaction tx(conn, &dbError);
        if (!tx.isActive()) {
            if (error)
                *error = dbError;
            return false;
        }
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
    }

    // Links to tags that no longer exist (left behind by older builds).
    if (!conn.execute(QStringLiteral("DELETE FROM image_tags "
                                     "WHERE tag_id NOT IN (SELECT id FROM tags)"),
                      &dbError)) {
        if (error)
            *error = dbError;
        return false;
    }
    return true;
}

namespace {

QStringList ddlStatements() {
    return {// meta
            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS meta (
                key   VARCHAR PRIMARY KEY,
                value VARCHAR NOT NULL
            )
        )"),

            // images
            QStringLiteral("CREATE SEQUENCE IF NOT EXISTS seq_image_id"),

            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS images (
                id           INTEGER PRIMARY KEY
                             DEFAULT nextval('seq_image_id'),
                md5          VARCHAR UNIQUE,
                parent_id    INTEGER,                       -- id of parent
                width        INTEGER,
                height       INTEGER,
                file_size    BIGINT,
                format       VARCHAR,                          
                rating       VARCHAR,                       
                source_id    VARCHAR,                       -- source name (e621...)
                remote_id    BIGINT,                        -- post id
                source_url   VARCHAR,                       -- post url
                created_at   TIMESTAMP,
                imported_at  TIMESTAMP DEFAULT NOW(),
                updated_at   TIMESTAMP DEFAULT NOW()
            )
        )"),

            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_images_parent "
                           "ON images(parent_id)"),

            // tags
            QStringLiteral("CREATE SEQUENCE IF NOT EXISTS seq_tag_id"),

            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS tags (
                id       INTEGER PRIMARY KEY
                         DEFAULT nextval('seq_tag_id'),
                category VARCHAR NOT NULL,
                name     VARCHAR NOT NULL,
                UNIQUE (category, name)
            )
        )"),

            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tags_category "
                           "ON tags(category)"),

            // image_tags
            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS image_tags (
                image_id INTEGER NOT NULL,
                tag_id   INTEGER NOT NULL,
                PRIMARY KEY (image_id, tag_id)
            )
        )"),

            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_image_tags_tag "
                           "ON image_tags(tag_id)"),

            // Parent links whose parent post is not in the library yet:
            // images.parent_id holds the parent's internal id, so it can
            // only be set once the parent is imported (LibraryCatalog
            // adopts these rows then).
            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS pending_parents (
                image_id         INTEGER PRIMARY KEY,
                source_id        VARCHAR NOT NULL,
                remote_parent_id BIGINT NOT NULL
            )
        )"),

            // Duplicate pairs the user has dismissed on the Conflicts page.
            // Pairs where one side was deleted are not recorded: the next
            // scan can't find both sides anyway. Stored once per pair with
            // md5_a < md5_b so lookups are order-independent.
            QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS resolved_duplicates (
                md5_a       VARCHAR NOT NULL,
                md5_b       VARCHAR NOT NULL,
                resolved_at TIMESTAMP DEFAULT NOW(),
                PRIMARY KEY (md5_a, md5_b)
            )
        )")};
}

/** Stored meta.schema_version; 0 for a new or pre-versioning database. */
int storedVersion(DbConnection& conn) {
    duckdb_result res{};
    QString error;
    // Fails on a new database (no meta table yet): version 0.
    if (!conn.query(QStringLiteral("SELECT value FROM meta WHERE key = 'schema_version'"), &res,
                    &error))
        return 0;
    int version = 0;
    if (duckdb_row_count(&res) > 0) {
        char* value = duckdb_value_varchar(&res, 0, 0);
        if (value) {
            version = QString::fromUtf8(value).toInt();
            duckdb_free(value);
        }
    }
    duckdb_destroy_result(&res);
    return version;
}

/**
 * Data migrations from version N-1 to N, run after the DDL (so new tables
 * exist) inside one transaction. Additive schema changes belong in
 * ddlStatements(); this is for rewriting existing rows. Empty for now:
 * v2 -> v3 only added pending_parents.
 */
QStringList migrationTo(int version) {
    // Add a case per version that needs row rewrites, e.g.
    //   if (version == 4) return {QStringLiteral("UPDATE ...")};
    Q_UNUSED(version);
    return {};
}

}  // namespace

bool ensureSchema(DbConnection& conn, QString* error) {
    if (!conn.isOpen()) {
        if (error) {
            *error = QStringLiteral(
                "Schema creation failed: "
                "database connection is not open");
        }

        return false;
    }

    // A database written by a newer build may use a layout this build does
    // not understand; writing to it could corrupt it.
    const int fromVersion = storedVersion(conn);
    if (fromVersion > kVersion) {
        if (error)
            *error = QStringLiteral(
                         "The library database was created by a newer version of Mosaic "
                         "(schema %1, this build supports %2). Please update Mosaic.")
                         .arg(fromVersion)
                         .arg(kVersion);
        return false;
    }

    for (const QString& statement : ddlStatements()) {
        QString queryError;

        if (!conn.execute(statement, &queryError)) {
            if (error) {
                *error = QStringLiteral("Schema DDL failed: %1").arg(queryError);
            }

            return false;
        }
    }

    if (fromVersion > 0 && fromVersion < kVersion) {
        DbTransaction tx(conn, error);
        if (!tx.isActive())
            return false;
        for (int version = fromVersion + 1; version <= kVersion; ++version) {
            for (const QString& sql : migrationTo(version)) {
                QString queryError;
                if (!conn.execute(sql, &queryError)) {
                    if (error)
                        *error = QStringLiteral("Migration to schema %1 failed: %2")
                                     .arg(version)
                                     .arg(queryError);
                    return false;
                }
            }
        }
        if (!tx.commit(error))
            return false;
    }

    const QString versionSql = QStringLiteral(
                                   "INSERT INTO meta(key, value) "
                                   "VALUES ('schema_version', '%1') "
                                   "ON CONFLICT (key) DO UPDATE "
                                   "SET value = excluded.value")
                                   .arg(QString::number(kVersion));

    QString versionError;

    if (!conn.execute(versionSql, &versionError)) {
        if (error) {
            *error = QStringLiteral("Schema version INSERT failed: %1").arg(versionError);
        }

        return false;
    }

    return true;
}

}  // namespace Mosaic::Schema