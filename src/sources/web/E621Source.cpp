#include "E621Source.h"

#include <QDebug>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace Mosaic {

namespace {

// Site settings (kept in code, one place per source).
constexpr const char* kBaseUrl = "https://e621.net";

/** Maps e621 tag categories to the catalog's category names. */
QString mapTagCategory(const QString& e621Type) {
    if (e621Type == QLatin1String("artist"))
        return QStringLiteral("artist");
    if (e621Type == QLatin1String("character"))
        return QStringLiteral("character");
    if (e621Type == QLatin1String("species"))
        return QStringLiteral("species");
    if (e621Type == QLatin1String("meta"))
        return QStringLiteral("meta");
    return QStringLiteral("general");  // general, lore, invalid, ...
}

QString id() { return QStringLiteral("e621"); }

/** Parses one post object; returns false when it has no usable md5. */
bool parsePost(const QJsonObject& post, SourceImage& out) {
    const QJsonObject file = post.value(QStringLiteral("file")).toObject();
    const QJsonObject preview = post.value(QStringLiteral("preview")).toObject();
    const QJsonObject sample = post.value(QStringLiteral("sample")).toObject();

    out.md5 = file.value(QStringLiteral("md5")).toString();
    out.fileUrl = file.value(QStringLiteral("url")).toString();

    // Fall back to deriving the md5 from the file URL ("<md5>.<ext>").
    if (out.md5.isEmpty() && !out.fileUrl.isEmpty()) {
        const int slash = out.fileUrl.lastIndexOf(QLatin1Char('/'));
        const int dot = out.fileUrl.lastIndexOf(QLatin1Char('.'));
        if (slash >= 0 && dot > slash) {
            out.md5 = out.fileUrl.mid(slash + 1, dot - slash - 1);
        }
    }
    if (out.md5.isEmpty())
        return false;

    out.sourceId = id();
    out.remoteId = post.value(QStringLiteral("id")).toInt();
    out.previewUrl = preview.value(QStringLiteral("url")).toString();
    out.sampleUrl = sample.value(QStringLiteral("url")).toString();
    out.width = file.value(QStringLiteral("width")).toInt();
    out.height = file.value(QStringLiteral("height")).toInt();
    out.fileSize = file.value(QStringLiteral("size")).toInt();
    out.format = file.value(QStringLiteral("ext")).toString();
    out.rating = post.value(QStringLiteral("rating")).toString();
    out.score =
        post.value(QStringLiteral("score")).toObject().value(QStringLiteral("total")).toInt();
    out.sourceUrl = QStringLiteral("%1/posts/%2").arg(QLatin1String(kBaseUrl)).arg(out.remoteId);

    // created_at is an ISO timestamp; keep it as unix seconds.
    const QString createdAt = post.value(QStringLiteral("created_at")).toString();
    out.createdAt = QDateTime::fromString(createdAt, Qt::ISODateWithMs).toSecsSinceEpoch();
    if (out.createdAt <= 0) {
        out.createdAt = QDateTime::fromString(createdAt, Qt::ISODate).toSecsSinceEpoch();
    }

    // Status from the flags object.
    const QJsonObject flags = post.value(QStringLiteral("flags")).toObject();
    out.status = QStringLiteral("active");
    if (flags.value(QStringLiteral("pending")).toBool())
        out.status = QStringLiteral("pending");
    else if (flags.value(QStringLiteral("flagged")).toBool())
        out.status = QStringLiteral("flagged");
    else if (flags.value(QStringLiteral("deleted")).toBool())
        out.status = QStringLiteral("deleted");

    // Parent chain: relationships.parent_id (null when there is no parent).
    const QJsonObject relationships = post.value(QStringLiteral("relationships")).toObject();
    out.parentId = relationships.value(QStringLiteral("parent_id")).toInt(0);
    out.hasChildren = relationships.value(QStringLiteral("has_children")).toBool();

    // Tags: { general: [...], artist: [...], character: [...], ... }
    const QJsonObject tags = post.value(QStringLiteral("tags")).toObject();
    for (auto it = tags.begin(); it != tags.end(); ++it) {
        const QString category = mapTagCategory(it.key());
        for (const auto& v : it.value().toArray()) {
            out.tags.append(SourceTag{v.toString(), category});
        }
    }
    return true;
}

}  // namespace

QString E621Source::id() const { return QStringLiteral("e621"); }

QString E621Source::displayName() const { return QStringLiteral("e621.net"); }

SourceResult E621Source::search(const SourceQuery& query) {
    SourceResult result;
    result.page = query.page;

    const int limit = qBound(1, query.limit, kMaxLimit);
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    params.addQueryItem(QStringLiteral("page"), QString::number(qMax(1, query.page)));
    params.addQueryItem(QStringLiteral("tags"), query.tags.trimmed());

    QUrl url(QStringLiteral("%1/posts.json").arg(QLatin1String(kBaseUrl)));
    url.setQuery(params);

    QString error;
    const QByteArray body = httpGet(url.toString(), &error);
    if (body.isEmpty() && !error.isEmpty()) {
        result.error = error;
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("Invalid JSON from e621: %1").arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("success")).isBool() &&
        !root.value(QStringLiteral("success")).toBool()) {
        result.error = root.value(QStringLiteral("message"))
                           .toString(QStringLiteral("e621 returned an error"));
        return result;
    }

    int withoutUrl = 0;
    for (const auto& v : root.value(QStringLiteral("posts")).toArray()) {
        SourceImage img;
        if (!parsePost(v.toObject(), img))
            continue;
        if (img.fileUrl.isEmpty())
            ++withoutUrl;  // login required to view
        result.images.append(img);
    }

    if (withoutUrl > 0) {
        result.warning = QStringLiteral("%1 post(s) have no file URL (login required to view them)")
                             .arg(withoutUrl);
        qInfo().noquote() << "e621:" << result.warning;
    }

    qDebug().noquote() << "e621 search:" << result.images.size() << "images, page" << query.page
                       << "tags:" << query.tags.trimmed();
    result.ok = true;
    return result;
}

SourceImage E621Source::parentOf(const SourceImage& image, QString* error) {
    if (image.parentId <= 0) {
        if (error)
            *error = QStringLiteral("Post has no parent");
        return {};
    }

    // Details endpoint: /posts/<id>.json -> { post: { ... } }
    const QString url =
        QStringLiteral("%1/posts/%2.json").arg(QLatin1String(kBaseUrl)).arg(image.parentId);
    QString httpError;
    const QByteArray body = httpGet(url, &httpError);
    if (body.isEmpty()) {
        if (error)
            *error =
                httpError.isEmpty() ? QStringLiteral("Empty response for parent post") : httpError;
        return {};
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid JSON for parent post");
        return {};
    }

    SourceImage parent;
    if (!parsePost(doc.object().value(QStringLiteral("post")).toObject(), parent)) {
        if (error)
            *error = QStringLiteral("Parent post has no usable data");
        return {};
    }
    return parent;
}

QList<SourceImage> E621Source::childrenOf(const SourceImage& image, QString* error) {
    if (image.remoteId <= 0) {
        if (error)
            *error = QStringLiteral("Post has no remote id");
        return {};
    }

    // Children are posts whose parent is this post: search "parent:<id>".
    SourceQuery query;
    query.tags = QStringLiteral("parent:%1").arg(image.remoteId);
    query.page = 1;
    query.limit = kMaxLimit;

    const SourceResult result = search(query);
    if (!result.ok) {
        if (error)
            *error = result.error;
        return {};
    }
    return result.images;
}

}  // namespace Mosaic
