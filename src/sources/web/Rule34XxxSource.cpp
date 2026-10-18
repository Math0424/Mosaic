#include "Rule34XxxSource.h"

#include <QDebug>
#include <QRegularExpression>
#include <QUrl>

namespace Mosaic {

namespace {

// Site settings (kept in code, one place per source).
constexpr const char* kBaseUrl = "https://rule34.xxx";

/** Decodes the HTML entities the site uses inside tag attributes. */
QString decodeEntities(QString text) {
    text.replace(QStringLiteral("&#039;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return text;
}

/** Post page URL for a post id. */
QString postUrl(qint64 id) {
    return QStringLiteral("%1/index.php?page=post&s=view&id=%2").arg(QLatin1String(kBaseUrl)).arg(id);
}

/**
 * Originals served from the main domain redirect to a hotlink page; the
 * same path on the image host serves the file directly.
 */
QString directFileUrl(QString url) {
    static const QRegularExpression mainHost(
        QStringLiteral("^https?://(?:www\\.)?rule34\\.xxx/"));
    url.replace(mainHost, QStringLiteral("https://wimg.rule34.xxx/"));
    return url;
}

/**
 * Best-effort full file URL derived from the preview; the real extension
 * can differ (.jpeg vs .jpg), so this is only a fallback.
 */
QString deriveFileUrl(const QString& previewUrl) {
    if (previewUrl.isEmpty())
        return {};
    QString url = previewUrl;
    url.replace(QStringLiteral("/thumbnails/"), QStringLiteral("/images/"));
    url.replace(QStringLiteral("thumbnail_"), QString());
    return directFileUrl(url);
}

/** Sample URL derived from the preview; last-resort fallback. */
QString deriveSampleUrl(const QString& previewUrl) {
    if (previewUrl.isEmpty())
        return {};
    QString url = previewUrl;
    url.replace(QStringLiteral("/thumbnails/"), QStringLiteral("/samples/"));
    url.replace(QStringLiteral("thumbnail_"), QStringLiteral("sample_"));
    return directFileUrl(url);
}

/** True for the formats the viewer plays as video. */
bool isVideoFormat(const QString& format) {
    static const QStringList video = {QStringLiteral("mp4"), QStringLiteral("webm"),
                                      QStringLiteral("mov"), QStringLiteral("mkv"),
                                      QStringLiteral("m4v"), QStringLiteral("avi")};
    return video.contains(format.toLower());
}

}  // namespace

QString Rule34XxxSource::id() const { return QStringLiteral("r34xxx"); }

QString Rule34XxxSource::displayName() const { return QStringLiteral("rule34.xxx"); }

QByteArray Rule34XxxSource::userAgent() const {
    return QByteArray(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/141.0.0.0 Safari/537.36");
}

void Rule34XxxSource::addHeaders(QNetworkRequest& request) const {
    // Cloudflare answers plain clients with a challenge page unless the
    // browser client hints accompany the user agent.
    request.setRawHeader("sec-ch-ua", "\"Chromium\";v=\"141\", \"Not?A_Brand\";v=\"24\"");
    request.setRawHeader("sec-ch-ua-mobile", "?0");
    request.setRawHeader("sec-ch-ua-platform", "\"Windows\"");
}

SourceResult Rule34XxxSource::search(const SourceQuery& query) {
    SourceResult result;
    result.page = query.page;

    // Pagination is a 0-based post offset, not a page number.
    const int pid = (qMax(1, query.page) - 1) * kPageSize;
    const QString search = query.tags.trimmed();

    QString url = QStringLiteral("%1/index.php?page=post&s=list&tags=%2")
                      .arg(QLatin1String(kBaseUrl))
                      .arg(QString::fromLatin1(QUrl::toPercentEncoding(
                          search.isEmpty() ? QStringLiteral("all") : search)));
    if (pid > 0)
        url += QStringLiteral("&pid=%1").arg(pid);

    QString error;
    const QByteArray body = httpGet(url, &error);
    if (body.isEmpty() && !error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QString html = QString::fromUtf8(body);

    // Cloudflare challenges and rate limiting answer with a page that has no
    // post list; report them instead of "no results".
    if (html.contains(QLatin1String("Rule34.xxx CAPTCHA")) ||
        html.contains(QLatin1String("429 Rate limiting"))) {
        result.error = QStringLiteral("%1 is rate limiting or challenging requests; try again later")
                           .arg(displayName());
        return result;
    }

    // One <span id="s<id>" class="thumb"> block per post, holding the post
    // link and the preview <img>.
    static const QRegularExpression thumbRe(
        QStringLiteral("<span id=\"s(?<id>\\d+)\" class=\"thumb\"\\s*>(?<block>.*?)</span>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression previewRe(QStringLiteral("<img src=\"(?<url>[^\"]+)\""));
    static const QRegularExpression titleRe(QStringLiteral("title=\"(?<tags>[^\"]*)\""));
    static const QRegularExpression classRe(QStringLiteral("class=\"preview(?<cls>[^\"]*)\""));
    static const QRegularExpression md5Re(
        QStringLiteral("thumbnail_(?<md5>[0-9a-f]{32,40})\\."));
    static const QRegularExpression scoreRe(QStringLiteral("\\bscore:(-?\\d+)"));
    static const QRegularExpression ratingRe(QStringLiteral("\\brating:(\\w+)"));

    auto it = thumbRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString block = m.captured(QStringLiteral("block"));

        const QString previewUrl =
            previewRe.match(block).captured(QStringLiteral("url"));
        const QString md5 = md5Re.match(previewUrl).captured(QStringLiteral("md5"));
        if (previewUrl.isEmpty() || md5.isEmpty())
            continue;

        SourceImage img;
        img.sourceId = id();
        img.remoteId = m.captured(QStringLiteral("id")).toLongLong();
        img.md5 = md5;
        img.previewUrl = previewUrl;
        img.sourceUrl = postUrl(img.remoteId);
        img.status = QStringLiteral("active");

        // The title attribute is the space-separated tag list followed by
        // "score:<n> rating:<r>".
        const QString title = decodeEntities(titleRe.match(block).captured(QStringLiteral("tags")));
        img.score = scoreRe.match(title).captured(1).toLongLong();
        img.rating = ratingRe.match(title).captured(1);

        QStringList tagList;
        for (const QString& tag : title.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (tag.startsWith(QLatin1String("score:")) || tag.startsWith(QLatin1String("rating:")))
                continue;
            tagList << tag.toLower();
        }
        for (const QString& tag : tagList)
            img.tags.append(SourceTag{tag, QStringLiteral("general")});

        // Videos are flagged by the preview class; their real file is an
        // .mp4 on a separate host. Animated GIFs keep a plain preview, so
        // fall back to the tags for those. Everything else keeps the
        // preview's extension as a guess - the real one is resolved from the
        // post page.
        const bool isVideo = classRe.match(block).captured(QStringLiteral("cls")).contains(
            QLatin1String("webm-thumb"));
        if (isVideo) {
            img.format = QStringLiteral("mp4");
        } else if (tagList.contains(QStringLiteral("gif"))) {
            img.format = QStringLiteral("gif");
        } else {
            // Preview URLs carry a "?<post id>" cache-buster.
            const QString path = previewUrl.left(previewUrl.indexOf(QLatin1Char('?')));
            const int dot = path.lastIndexOf(QLatin1Char('.'));
            if (dot >= 0)
                img.format = path.mid(dot + 1).toLower();
        }

        // The real file's extension differs from the thumbnail's (.jpeg vs
        // .jpg), so leave fileUrl empty and resolve it from the post page.
        result.images.append(img);
    }

    result.pageLimit = kPageSize;
    if (result.images.isEmpty())
        qInfo().noquote() << "rule34.xxx: 0 posts parsed (page" << query.page << ", tags:" << search
                          << ") - layout may have changed";
    else
        qDebug().noquote() << "rule34.xxx search:" << result.images.size() << "images, page"
                           << query.page << "tags:" << search;
    result.ok = true;
    return result;
}

QString Rule34XxxSource::resolveFullUrl(const SourceImage& image) const {
    if (image.remoteId <= 0)
        return {};
    const QByteArray details = httpGet(postUrl(image.remoteId));
    if (details.isEmpty())
        return {};
    static const QRegularExpression originalRe(
        QStringLiteral("<a href=\"(?<url>https?://[^\"]+)\"[^>]*>\\s*Original image"));
    const QRegularExpressionMatch m = originalRe.match(QString::fromUtf8(details));
    if (!m.hasMatch())
        return {};
    return directFileUrl(m.captured(QStringLiteral("url")));
}

QByteArray Rule34XxxSource::fetch(const SourceImage& image, FileKind kind, QString* error) {
    if (kind != FileKind::Full)
        return HttpSource::fetch(image, kind, error);

    // The real file's extension differs from the thumbnail's, so resolve it
    // from the post page. Stills fall back to the derived full URL and then
    // to the sample, which always exists and shares the thumbnail's
    // extension; videos have no such fallback.
    QStringList candidates{resolveFullUrl(image)};
    if (!isVideoFormat(image.format)) {
        candidates << deriveFileUrl(image.previewUrl) << deriveSampleUrl(image.previewUrl);
    }

    QString lastError;
    for (const QString& url : candidates) {
        if (url.isEmpty())
            continue;
        QString attemptError;
        const QByteArray data = httpGet(url, &attemptError);
        if (!data.isEmpty())
            return data;
        if (lastError.isEmpty())
            lastError = attemptError;
    }
    if (error)
        *error = lastError.isEmpty() ? QStringLiteral("Image has no URL to download") : lastError;
    return {};
}

bool Rule34XxxSource::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                     const std::function<void(qint64, qint64)>& progress,
                                     QString* error) {
    QString url;
    if (kind == FileKind::Full) {
        url = resolveFullUrl(image);
        if (url.isEmpty() && !isVideoFormat(image.format)) {
            url = deriveFileUrl(image.previewUrl);
            if (url.isEmpty())
                url = deriveSampleUrl(image.previewUrl);
        }
    }
    if (url.isEmpty())
        url = image.urlFor(kind);
    if (url.isEmpty()) {
        if (error)
            *error = QStringLiteral("Image has no URL to download");
        return false;
    }
    return httpDownload(url, path, progress, error);
}

}  // namespace Mosaic
