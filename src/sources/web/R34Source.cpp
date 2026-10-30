#include "R34Source.h"

#include <QDebug>
#include <QRegularExpression>
#include <QUrl>

namespace Mosaic {

namespace {

// Site settings (kept in code, one place per source).
constexpr const char* kBaseUrl = "https://rule34.us";

/**
 * Derives the full file URL from the preview URL; videos move to //video.
 */
QString completeImageUrl(QString fileUrl, const QString& previewUrl, const QStringList& tags) {
    if (fileUrl.size() < 5 && !previewUrl.isEmpty()) {
        fileUrl = previewUrl;
        fileUrl.replace(QStringLiteral("/thumbnails/"), QStringLiteral("/images/"));
        fileUrl.replace(QStringLiteral("thumbnail_"), QString());
    }

    const bool isVideo = tags.contains(QStringLiteral("video"));
    if (isVideo && fileUrl.contains(QStringLiteral("//img"))) {
        static const QRegularExpression imgHost(QStringLiteral("//img\\d+\\."));
        fileUrl.replace(imgHost, QStringLiteral("//video."));
    }
    return fileUrl;
}

}  // namespace

QString R34Source::id() const { return QStringLiteral("r34"); }

QString R34Source::displayName() const { return QStringLiteral("rule34.us"); }

QByteArray R34Source::userAgent() const {
    return QByteArray(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:108.0) "
        "Gecko/20100101 Firefox/108.0");
}

SourceResult R34Source::search(const SourceQuery& query) {
    SourceResult result;
    result.page = query.page;

    // The HTML API is 0-based and omits page=0.
    const int page = qMax(0, query.page - 1);
    const QString search = query.tags.trimmed();

    QString url = QStringLiteral("%1/index.php?r=posts/index&q=%2")
                      .arg(QLatin1String(kBaseUrl))
                      .arg(QString::fromLatin1(QUrl::toPercentEncoding(
                          search.isEmpty() ? QStringLiteral("all") : search)));
    if (page > 0)
        url += QStringLiteral("&page=%1").arg(page);

    QString error;
    const QByteArray body = httpGet(url, &error);
    if (body.isEmpty() && !error.isEmpty()) {
        result.error = error;
        return result;
    }
    const QString html = QString::fromUtf8(body);

    // One <a id="..."><img src="...thumbnail_<md5>.<ext>" title="<tags>"...>
    // block per post.
    static const QRegularExpression postRe(QStringLiteral(
        "<a id=\"(?<id>\\d+)\" href=\"[^\"]+\"><img +src=\"(?<preview_url>[^\"]*"
        "thumbnail_(?<md5>[^.]+)\\.[^\"]+)\" title=\" *(?<tags>[^\"]*)\" .+?</a></div>"));

    auto it = postRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();

        SourceImage img;
        img.sourceId = id();
        img.remoteId = m.captured(QStringLiteral("id")).toLongLong();
        img.md5 = m.captured(QStringLiteral("md5"));
        img.previewUrl = m.captured(QStringLiteral("preview_url"));
        if (img.md5.isEmpty())
            continue;

        // rule34.us titles are comma-separated with underscores for spaces.
        QStringList tags;
        for (QString tag :
             m.captured(QStringLiteral("tags")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            tag = tag.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
            if (!tag.isEmpty())
                tags << tag;
        }
        for (const QString& tag : tags) img.tags.append(SourceTag{tag, QStringLiteral("general")});

        // The derived file URL is unreliable (the real file can have a
        // different extension than the thumbnail, e.g. .jpeg vs .jpg), so
        // leave fileUrl empty and resolve it lazily from the detail page.
        img.fileUrl = completeImageUrl(QString(), img.previewUrl, tags);
        img.sourceUrl = QStringLiteral("%1/index.php?r=posts/view&id=%2")
                            .arg(QLatin1String(kBaseUrl))
                            .arg(img.remoteId);
        img.status = QStringLiteral("active");

        // Guess the format from the preview URL extension.
        const int dot = img.previewUrl.lastIndexOf(QLatin1Char('.'));
        if (dot >= 0)
            img.format = img.previewUrl.mid(dot + 1).toLower();

        result.images.append(img);
    }

    // The site ignores `limit` and returns ~40 posts per page, so report the
    // actual page size; callers must not treat short pages as "no more results".
    result.pageLimit = result.images.size();
    if (result.images.isEmpty())
        qInfo().noquote() << "rule34.us: 0 posts parsed (page" << query.page << ", tags:" << search
                          << ") - layout may have changed";
    else
        qDebug().noquote() << "rule34.us search:" << result.images.size() << "images, page"
                           << query.page << "tags:" << search;
    result.ok = true;
    return result;
}

QString R34Source::resolveFullUrl(const SourceImage& image) const {
    if (image.remoteId <= 0)
        return {};
    const QString detailsUrl = QStringLiteral("%1/index.php?r=posts/view&id=%2")
                                   .arg(QLatin1String(kBaseUrl))
                                   .arg(image.remoteId);
    const QByteArray details = httpGet(detailsUrl);
    if (details.isEmpty())
        return {};
    static const QRegularExpression originalRe(
        QStringLiteral("<a href=\"(?<url>https?://[^\"]+)\"><li[^>]*>Original</li></a>"));
    const QRegularExpressionMatch m = originalRe.match(QString::fromUtf8(details));
    return m.hasMatch() ? m.captured(QStringLiteral("url")) : QString();
}

QByteArray R34Source::fetch(const SourceImage& image, FileKind kind, QString* error) {
    // Full-size files: the URL derived from the thumbnail can 404 when the
    // original file has a different extension, so resolve the real URL from
    // the post's detail page first (falling back to the derived one).
    if (kind == FileKind::Full) {
        const QString resolved = resolveFullUrl(image);
        if (!resolved.isEmpty()) {
            const QByteArray data = mediaGet(resolved, error);
            if (!data.isEmpty())
                return data;
        }
        // Fall through to the derived URL.
    }
    return HttpSource::fetch(image, kind, error);
}

bool R34Source::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                               const std::function<void(qint64, qint64)>& progress,
                               QString* error) {
    QString url;
    if (kind == FileKind::Full)
        url = resolveFullUrl(image);
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
