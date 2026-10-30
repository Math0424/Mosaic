#include "LibrarySql.h"

#include "../../core/Schema.h"

namespace Mosaic {
namespace LibrarySql {

namespace {

bool isUniversalWildcard(const QString& term) {
    const QString trimmed = term.trimmed();
    if (trimmed.isEmpty())
        return false;
    for (QChar c : trimmed) {
        if (c != QLatin1Char('*'))
            return false;
    }
    return true;
}

/** Strips a leading '~' and validates a term as an OR-alternative. */
QString normalizeAlternative(const QString& raw) {
    QString term = raw.trimmed().toLower();
    if (term.startsWith(QLatin1Char('~')))
        term.remove(0, 1);
    term = term.trimmed();
    if (term.isEmpty() || isUniversalWildcard(term))
        return {};
    return term;
}

}  // namespace

QString escape(const QString& s) {
    return QString(s).replace(QLatin1Char('\''), QStringLiteral("''"));
}

QString quoted(const QString& s) { return QLatin1Char('\'') + escape(s) + QLatin1Char('\''); }

QString inList(const QStringList& values) {
    QStringList quotedValues;
    quotedValues.reserve(values.size());
    for (const QString& v : values) quotedValues << quoted(v);
    return quotedValues.join(QStringLiteral(", "));
}

QString tagPredicate(const QString& term) {
    const QString normalized = term.toLower();
    if (!normalized.contains(QLatin1Char('*')))
        return QStringLiteral("lower(t.name) = %1").arg(quoted(normalized));

    if (isUniversalWildcard(normalized))
        return QStringLiteral("TRUE");

    QString like = normalized;
    like.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    like.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    like.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    like.replace(QLatin1Char('*'), QStringLiteral("%"));
    // DuckDB string literals are standard-conforming: '\' is one character.
    return QStringLiteral("lower(t.name) LIKE %1 ESCAPE '\\'").arg(quoted(like));
}

QString termPredicate(const QString& term) {
    const int colon = term.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString category = term.left(colon).toLower();
        if (Schema::isCategoryName(category)) {
            return QStringLiteral("t.category = %1 AND (%2)")
                .arg(quoted(category), tagPredicate(term.mid(colon + 1)));
        }
    }
    return tagPredicate(term);
}

void splitTags(const QString& tags, QStringList& include, QStringList& exclude,
               QString& orderTerm) {
    QList<TagGroup> groups;
    splitTagGroups(tags, groups, exclude, orderTerm);
    // Flatten: every alternative of every group becomes a required include.
    for (const TagGroup& group : groups) include << group.alternatives;
}

void splitTagGroups(const QString& tags, QList<TagGroup>& groups, QStringList& exclude,
                    QString& orderTerm) {
    groups.clear();
    exclude.clear();
    orderTerm.clear();

    TagGroup pendingOrs;  // consecutive "~term" tokens outside parens
    bool pendingOrsOpen = false;

    auto flushPending = [&]() {
        if (pendingOrsOpen && !pendingOrs.alternatives.isEmpty())
            groups.append(pendingOrs);
        pendingOrs = TagGroup{};
        pendingOrsOpen = false;
    };

    int i = 0;
    const int n = tags.size();
    while (i < n) {
        QChar c = tags.at(i);
        if (c.isSpace()) {
            ++i;
            continue;
        }

        if (c == QLatin1Char('(') && tags.indexOf(QLatin1Char(')'), i + 1) < 0) {
            // Unclosed '(': ignore it rather than swallowing the rest of
            // the query (exclusions, order:) into one OR-group.
            ++i;
            continue;
        }

        if (c == QLatin1Char('(')) {
            // Parenthesized OR-group: collect until the matching ')'.
            ++i;
            TagGroup group;
            QString token;
            // Parens inside a tag ("fate_(series)") nest; only a ')' at
            // depth 0 closes the group.
            int depth = 0;
            while (i < n && !(tags.at(i) == QLatin1Char(')') && depth == 0)) {
                const QChar ch = tags.at(i);
                if (ch == QLatin1Char('('))
                    ++depth;
                else if (ch == QLatin1Char(')'))
                    --depth;
                if (ch.isSpace()) {
                    const QString alt = normalizeAlternative(token);
                    if (!alt.isEmpty())
                        group.alternatives << alt;
                    token.clear();
                } else {
                    token += ch;
                }
                ++i;
            }
            const QString alt = normalizeAlternative(token);
            if (!alt.isEmpty())
                group.alternatives << alt;
            if (i < n)
                ++i;  // consume ')'
            flushPending();
            if (!group.alternatives.isEmpty())
                groups.append(group);
            continue;
        }

        // Plain token up to the next whitespace.
        QString token;
        while (i < n && !tags.at(i).isSpace()) {
            token += tags.at(i);
            ++i;
        }

        if (token.startsWith(QLatin1Char('-')) && token.size() > 1) {
            flushPending();
            const QString exclusion = normalizeAlternative(token.mid(1));
            if (!exclusion.isEmpty())
                exclude << exclusion;
            continue;
        }

        if (token.startsWith(QLatin1String("order:")) || token.startsWith(QLatin1String("sort:"))) {
            flushPending();
            orderTerm = token.toLower().mid(token.startsWith(QLatin1String("order:")) ? 6 : 5);
            continue;
        }

        const QString term = normalizeAlternative(token);
        if (term.isEmpty())
            continue;
        if (token.startsWith(QLatin1Char('~'))) {
            // Consecutive ~terms form one implicit OR-group.
            pendingOrs.alternatives << term;
            pendingOrsOpen = true;
        } else {
            flushPending();
            groups.append(TagGroup{{term}});
        }
    }
    flushPending();
}

QString orderByClause(const QString& order) {
    if (order == QLatin1String("random"))
        return QStringLiteral("ORDER BY random()");
    if (order == QLatin1String("id"))
        return QStringLiteral("ORDER BY i.remote_id ASC NULLS LAST");
    if (order == QLatin1String("id_desc"))
        return QStringLiteral("ORDER BY i.remote_id DESC NULLS LAST");
    if (order == QLatin1String("created") || order == QLatin1String("created_at") ||
        order == QLatin1String("date"))
        return QStringLiteral("ORDER BY i.created_at DESC NULLS LAST");
    if (order == QLatin1String("imported") || order == QLatin1String("imported_at"))
        return QStringLiteral("ORDER BY i.imported_at DESC NULLS LAST");
    if (order == QLatin1String("filesize"))
        return QStringLiteral("ORDER BY i.file_size DESC NULLS LAST");
    if (order == QLatin1String("filesize_asc"))
        return QStringLiteral("ORDER BY i.file_size ASC NULLS LAST");
    if (order == QLatin1String("mpixels"))
        return QStringLiteral("ORDER BY (i.width * i.height) DESC NULLS LAST");
    if (order == QLatin1String("mpixels_asc"))
        return QStringLiteral("ORDER BY (i.width * i.height) ASC NULLS LAST");
    if (order == QLatin1String("landscape"))
        return QStringLiteral(
            "ORDER BY CASE WHEN i.width > i.height THEN 0 ELSE 1 END, "
            "i.imported_at DESC NULLS LAST");
    if (order == QLatin1String("portrait"))
        return QStringLiteral(
            "ORDER BY CASE WHEN i.height > i.width THEN 0 ELSE 1 END, "
            "i.imported_at DESC NULLS LAST");
    // Default (and unsupported modifiers like order:score): newest first.
    return QStringLiteral("ORDER BY i.imported_at DESC NULLS LAST");
}

}  // namespace LibrarySql
}  // namespace Mosaic
