#include "HttpSource.h"

#include <QEventLoop>
#include <QDebug>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace Mosaic {

namespace {

constexpr const char* kDefaultUserAgent =
    "Mosaic/0.1 (image library; contact: local user) "
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:108.0) "
    "Gecko/20100101 Firefox/108.0";
constexpr int kTimeoutMs = 30000;

QString httpError(QNetworkReply* reply) {
    return QStringLiteral("HTTP %1: %2")
        .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
        .arg(reply->errorString());
}

}  // namespace

void HttpSource::addAuth(QUrlQuery& params) const { Q_UNUSED(params); }

QByteArray HttpSource::userAgent() const { return QByteArray(kDefaultUserAgent); }

QNetworkRequest HttpSource::makeRequest(const QString& url) const {
    QNetworkRequest request(QUrl{url});
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", "*/*");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

QByteArray HttpSource::httpGet(const QString& url, QString* error) const {
    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(makeRequest(url));

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(kTimeoutMs);
    loop.exec();

    QByteArray data;
    if (!timeout.isActive()) {
        if (error)
            *error = QStringLiteral("Request timed out: %1").arg(url);
        qWarning().noquote() << "HTTP GET timed out:" << url;
    } else if (reply->error() != QNetworkReply::NoError) {
        if (error)
            *error = httpError(reply);
        qWarning().noquote() << "HTTP GET failed:" << httpError(reply) << "--" << url;
    } else {
        data = reply->readAll();
        qDebug().noquote() << "HTTP GET ok:" << data.size() << "bytes --" << url;
    }
    reply->deleteLater();
    return data;
}

bool HttpSource::httpDownload(const QString& url, const QString& path,
                              const std::function<void(qint64, qint64)>& progress,
                              QString* error) const {
    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(makeRequest(url));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(path);
        qWarning().noquote() << "HTTP download: cannot open file:" << path << "--"
                             << file.errorString();
        reply->abort();
        reply->deleteLater();
        return false;
    }

    // Stream every chunk to disk as it arrives; never buffer the body.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        file.write(reply->readAll());
        if (progress)
            progress(file.size(), -1);
        timeout.start(kTimeoutMs);  // per-chunk stall timeout
    });
    timeout.start(kTimeoutMs);
    loop.exec();

    // Drain anything that arrived after the last readyRead.
    file.write(reply->readAll());
    file.close();

    bool ok = false;
    if (!timeout.isActive()) {
        if (error)
            *error = QStringLiteral("Request timed out: %1").arg(url);
        qWarning().noquote() << "HTTP download timed out:" << url;
    } else if (reply->error() != QNetworkReply::NoError) {
        if (error)
            *error = httpError(reply);
        qWarning().noquote() << "HTTP download failed:" << httpError(reply) << "--" << url;
    } else {
        ok = true;
        if (progress)
            progress(file.size(), file.size());
        qDebug().noquote() << "HTTP download ok:" << file.size() << "bytes ->" << path;
    }
    if (!ok)
        QFile::remove(path);
    reply->deleteLater();
    return ok;
}

QByteArray HttpSource::fetch(const SourceImage& image, FileKind kind, QString* error) {
    const QString url = image.urlFor(kind);
    if (url.isEmpty()) {
        if (error)
            *error = QStringLiteral("Image has no URL to download");
        return {};
    }
    return httpGet(url, error);
}

bool HttpSource::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                const std::function<void(qint64, qint64)>& progress,
                                QString* error) {
    const QString url = image.urlFor(kind);
    if (url.isEmpty()) {
        if (error)
            *error = QStringLiteral("Image has no URL to download");
        return false;
    }
    return httpDownload(url, path, progress, error);
}

}  // namespace Mosaic
