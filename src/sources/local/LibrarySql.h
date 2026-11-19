#pragma once

#include <QString>
#include <QStringList>

namespace Mosaic {

// Shared SQL / query-string helpers for the library catalog (DuckDB).
namespace LibrarySql {

QString escape(const QString& s);

QString quoted(const QString& s);

QString inList(const QStringList& values);

// Case-insensitive tag-name predicate; '*' becomes LIKE wildcards.
QString tagPredicate(const QString& term);

// Predicate for one term, optionally category-qualified ("usergroup:favorite").
QString termPredicate(const QString& term);

// Splits a query into included / excluded ("-tag") terms; order:/sort:
// modifiers land in orderTerm (last one wins).
void splitTags(const QString& tags, QStringList& include, QStringList& exclude, QString& orderTerm);

// One OR-group: an image matches when it has ANY alternative.
struct TagGroup {
    QStringList alternatives;
};

// Splits a query into OR-groups, exclusions and an order modifier.
// Syntax mirrors e621: "cat dog", "~cat ~dog", "( ~a ~b ) ( ~c ~d )",
// "-chicken", '*' wildcards, "category:name" qualifiers.
void splitTagGroups(const QString& tags, QList<TagGroup>& groups, QStringList& exclude,
                    QString& orderTerm);

QString orderByClause(const QString& order);

// "feed:NN" order term -> NN clamped to [0, 100] (100 = pure taste,
// 0 = pure exploration; malformed values mean 100). -1 when the term is
// not a feed ranking.
int feedOrderLevel(const QString& order);

}  // namespace LibrarySql

}  // namespace Mosaic
