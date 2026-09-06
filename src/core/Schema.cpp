#include "Schema.h"

#include <QString>
#include <QStringList>

namespace Mosaic::Schema {

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

    for (const QString& statement : ddlStatements()) {
        QString queryError;

        if (!conn.execute(statement, &queryError)) {
            if (error) {
                *error = QStringLiteral("Schema DDL failed: %1").arg(queryError);
            }

            return false;
        }
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