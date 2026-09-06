#include "GelbooruSource.h"

#include <QDebug>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

namespace Mosaic {

namespace {

/**
 * Repairs the file URL like the reference Gelbooru completeImage():
 * derives the full file from the preview and moves videos to their host.
 */
QString fixFileUrl(QString fileUrl, const QString& previewUrl, const QStringList& tagList) {
    if (fileUrl.size() < 5 && !previewUrl.isEmpty()) {
        fileUrl = previewUrl;
        fileUrl.replace(QStringLiteral("/thumbnails/"), QStringLiteral("/images/"));
        fileUrl.replace(QStringLiteral("thumbnail_"), QString());
    }

    if (tagList.contains(QStringLiteral("video")) && fileUrl.contains(QStringLiteral("//img"))) {
        static const QRegularExpression imgHost(QStringLiteral("//img\\d+\\."));
        fileUrl.replace(imgHost, QStringLiteral("//video."));
    }
    return fileUrl;
}

}  // namespace

GelbooruSource::GelbooruSource(const QString& id, const QString& displayName,
                               const QString& apiBase, const QString& siteBase)
    : m_id(id), m_displayName(displayName), m_apiBase(apiBase), m_siteBase(siteBase) {}

QString GelbooruSource::id() const { return m_id; }

QString GelbooruSource::displayName() const { return m_displayName; }

void GelbooruSource::addAuth(QUrlQuery& params) const {
    if (!m_userId.isEmpty() && !m_apiKey.isEmpty()) {
        params.addQueryItem(QStringLiteral("user_id"), m_userId);
        params.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    }
}

bool GelbooruSource::parsePost(const QJsonObject& post, SourceImage& out) const {
    out.md5 = post.value(QStringLiteral("md5")).toString();
    out.fileUrl = post.value(QStringLiteral("file_url")).toString();
    out.previewUrl = post.value(QStringLiteral("preview_url")).toString();
    out.sampleUrl = post.value(QStringLiteral("sample_url")).toString();

    // Fall back to deriving the md5 from the file URL ("<md5>.<ext>").
    if (out.md5.isEmpty() && !out.fileUrl.isEmpty()) {
        const int slash = out.fileUrl.lastIndexOf(QLatin1Char('/'));
        const int dot = out.fileUrl.lastIndexOf(QLatin1Char('.'));
        if (slash >= 0 && dot > slash)
            out.md5 = out.fileUrl.mid(slash + 1, dot - slash - 1);
    }
    if (out.md5.isEmpty())
        return false;

    out.sourceId = m_id;
    out.remoteId = post.value(QStringLiteral("id")).toVariant().toLongLong();
    out.width = post.value(QStringLiteral("width")).toInt();
    out.height = post.value(QStringLiteral("height")).toInt();
    out.fileSize = post.value(QStringLiteral("file_size")).toVariant().toLongLong();
    out.rating = post.value(QStringLiteral("rating")).toString();
    out.score = post.value(QStringLiteral("score")).toVariant().toLongLong();
    out.parentId = post.value(QStringLiteral("parent_id")).toVariant().toLongLong();
    out.hasChildren = post.value(QStringLiteral("has_children")).toBool();
    out.status = QStringLiteral("active");
    out.sourceUrl =
        QStringLiteral("%1/index.php?page=post&s=view&id=%2").arg(m_siteBase).arg(out.remoteId);

    // created_at arrives as "yyyy-MM-dd HH:mm:ss" (UTC) on Gelbooru 0.2
    // sites; keep it as unix seconds for the Favorites date filter.
    const QString createdAt = post.value(QStringLiteral("created_at")).toString();
    QDateTime dt =
        QDateTime::fromString(createdAt, QStringLiteral("yyyy-MM-dd HH:mm:ss"), QTimeZone::UTC);
    if (!dt.isValid())
        dt = QDateTime::fromString(createdAt, Qt::ISODate);
    if (dt.isValid())
        out.createdAt = dt.toSecsSinceEpoch();

    // The search endpoint returns tags as one space-separated string with
    // no categories (tag categories would need one extra request per post).
    const QStringList tagList = post.value(QStringLiteral("tags"))
                                    .toString()
                                    .simplified()
                                    .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& tag : tagList)
        out.tags.append(SourceTag{tag.toLower(), QStringLiteral("general")});

    out.fileUrl = fixFileUrl(out.fileUrl, out.previewUrl, tagList);

    // Guess the format from the best URL's extension.
    const QString& urlForFormat = !out.fileUrl.isEmpty() ? out.fileUrl : out.previewUrl;
    const int dot = urlForFormat.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0)
        out.format = urlForFormat.mid(dot + 1).toLower();

    return true;
}

SourceResult GelbooruSource::search(const SourceQuery& query) {
    SourceResult result;
    result.page = query.page;

    const int limit = qBound(1, query.limit, kMaxLimit);
    // dapi pagination is a 0-based post offset, not a page number.
    const int pid = (qMax(1, query.page) - 1) * limit;

    QUrlQuery params;
    params.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
    params.addQueryItem(QStringLiteral("s"), QStringLiteral("post"));
    params.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
    params.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
    params.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    params.addQueryItem(QStringLiteral("pid"), QString::number(pid));
    const QString tags = query.tags.trimmed();
    if (!tags.isEmpty())
        params.addQueryItem(QStringLiteral("tags"), tags);
    addAuth(params);

    QUrl url(m_apiBase + QStringLiteral("/index.php"));
    url.setQuery(params);

    QString error;
    const QByteArray body = httpGet(url.toString(), &error);
    if (body.isEmpty() && !error.isEmpty()) {
        result.error = error;
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        // Some installs answer errors as a plain string instead of JSON.
        const QString text = QString::fromUtf8(body).trimmed();
        if (!text.isEmpty() && !text.startsWith(QLatin1Char('{')) &&
            !text.startsWith(QLatin1Char('['))) {
            result.error = text.left(300);
        } else {
            result.error = QStringLiteral("Invalid JSON from %1: %2")
                               .arg(m_displayName, parseError.errorString());
        }
        return result;
    }

    QJsonArray posts;
    if (doc.isArray()) {
        posts = doc.array();  // old installs answer with a bare array
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonObject response = root.value(QStringLiteral("response")).toObject();
        const QJsonObject attrs = response.value(QStringLiteral("@attributes")).toObject();
        if (attrs.value(QStringLiteral("success")).toString() == QLatin1String("false")) {
            result.error = attrs.value(QStringLiteral("reason"))
                               .toString(QStringLiteral("%1 returned an error").arg(m_displayName));
            return result;
        }
        const QJsonValue postVal = root.value(QStringLiteral("post"));
        if (postVal.isArray())
            posts = postVal.toArray();
        else if (postVal.isObject())
            posts.append(postVal.toObject());  // single-post shorthand
    }

    for (const auto& v : posts) {
        SourceImage img;
        if (parsePost(v.toObject(), img))
            result.images.append(img);
    }

    qDebug().noquote() << m_displayName << "search:" << result.images.size() << "images, page"
                       << query.page << "tags:" << query.tags.trimmed();
    result.ok = true;
    return result;
}

SourceImage GelbooruSource::postById(qint64 id, QString* error) const {
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("page"), QStringLiteral("dapi"));
    params.addQueryItem(QStringLiteral("s"), QStringLiteral("post"));
    params.addQueryItem(QStringLiteral("q"), QStringLiteral("index"));
    params.addQueryItem(QStringLiteral("json"), QStringLiteral("1"));
    params.addQueryItem(QStringLiteral("id"), QString::number(id));
    addAuth(params);

    QUrl url(m_apiBase + QStringLiteral("/index.php"));
    url.setQuery(params);

    QString httpError;
    const QByteArray body = httpGet(url.toString(), &httpError);
    if (body.isEmpty()) {
        if (error)
            *error = httpError.isEmpty() ? QStringLiteral("Empty response for post %1").arg(id)
                                         : httpError;
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject post;
    if (doc.isObject()) {
        const QJsonValue postVal = doc.object().value(QStringLiteral("post"));
        if (postVal.isArray() && !postVal.toArray().isEmpty())
            post = postVal.toArray().first().toObject();
        else if (postVal.isObject())
            post = postVal.toObject();
    } else if (doc.isArray() && !doc.array().isEmpty()) {
        post = doc.array().first().toObject();
    }

    SourceImage out;
    if (post.isEmpty() || !parsePost(post, out)) {
        if (error)
            *error = QStringLiteral("Post %1 has no usable data").arg(id);
        return {};
    }
    return out;
}

SourceImage GelbooruSource::parentOf(const SourceImage& image, QString* error) {
    if (image.parentId <= 0) {
        if (error)
            *error = QStringLiteral("Post has no parent");
        return {};
    }
    return postById(image.parentId, error);
}

QList<SourceImage> GelbooruSource::childrenOf(const SourceImage& image, QString* error) {
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
