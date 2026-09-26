#pragma once

#include <QString>
#include "Database.h"

namespace Mosaic {

// Catalog schema for the Mosaic image library.

// Flat library: files are copied into one folder named by MD5; the catalog
// is the source of truth. Image primary key is the MD5 hash. Tags are
// normalized (tags dictionary + image_tags junction) so tag filtering is a
// cheap indexed join and renames/merges touch one row. Parent/child lives
// directly on images.parent_id.
namespace Schema {

// Bump when ddlStatements() changes. Migrations are additive (IF NOT EXISTS).
constexpr int kVersion = 2;

enum class TagCategory {
    General,
    Artist,
    Character,
    Species,
    Meta,
    Model,
    UserGroup,
};

QString categoryToString(TagCategory c);

bool isCategoryName(const QString& name);

// Canonical tag form: lowercase, commas treated as separators, whitespace
// collapsed to underscores. Empty string when nothing usable remains.
QString normalizeTagName(const QString& name);

// Repairs malformed stored tags (commas/spaces/mixed case): splits them
// into normalized pieces, re-links affected images, deletes the broken
// rows. Safe to run on every open; no-ops when nothing is malformed.
bool sanitizeTags(DbConnection& conn, QString* error);

// Creates all catalog tables if missing and records the schema version.
bool ensureSchema(DbConnection& conn, QString* error);

}  // namespace Schema

}  // namespace Mosaic
