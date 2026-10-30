#include "HttpSource.h"

#include <QDateTime>
#include <QEventLoop>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
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
// Retries after "429 Too Many Requests" before giving up.
constexpr int kMaxRateLimitRetries = 3;
constexpr int kMaxRetryAfterMs = 60000;

QString httpError(QNetworkReply* reply) {
    return QStringLiteral("HTTP %1: %2")
        .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
        .arg(reply->errorString());
}

bool isRateLimited(QNetworkReply* reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429;
}

/** Back-off after a 429: the server's Retry-After seconds, else 2s, 4s, 8s. */
int retryDelayMs(QNetworkReply* reply, int attempt) {
    bool ok = false;
    const int seconds = reply->rawHeader("Retry-After").trimmed().toInt(&ok);
    if (ok && seconds > 0)
        return qMin(seconds * 1000, kMaxRetryAfterMs);
    return 2000 << attempt;
}

// Host-wide cooldown after consecutive 429s: 2s, 4s, 8s ... capped.
constexpr int kBaseCooldownMs = 2000;

// Per-thread request policy (see HttpSource::ImpatientScope).
thread_local int t_maxWaitMs = -1;  // -1 = patient
thread_local bool t_lastRateLimited = false;

/**
 * Next free request slot per host, shared by all sources and threads, so
 * parallel thumbnail/search/post-page requests queue up instead of bursting.
 */
struct HostPacing {
    QMutex mutex;
    QHash<QString, qint64> nextSlotMs;
    QHash<QString, int> strikes;  // consecutive 429s
};

HostPacing& hostPacing() {
    static HostPacing pacing;
    return pacing;
}

/**
 * Reserves the next request slot for host and sleeps until it arrives.
 * With a non-negative maxWaitMs, returns false (reserving nothing) when the
 * slot is further away than that.
 */
bool waitForSlot(const QString& host, int intervalMs, int maxWaitMs) {
    qint64 waitMs = 0;
    {
        HostPacing& pacing = hostPacing();
        QMutexLocker lock(&pacing.mutex);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64& next = pacing.nextSlotMs[host];
        const qint64 slot = qMax(now, next);
        if (maxWaitMs >= 0 && slot - now > maxWaitMs)
            return false;
        next = slot + qMax(0, intervalMs);
        waitMs = slot - now;
    }
    if (waitMs > 0)
        QThread::msleep(static_cast<unsigned long>(waitMs));
    return true;
}

/**
 * Holds every request to host back after a 429: at least delayMs, and
 * longer for each consecutive 429, so a site that keeps refusing is left
 * alone instead of hammered. Returns the cooldown applied.
 */
int backOff(const QString& host, int delayMs) {
    HostPacing& pacing = hostPacing();
    QMutexLocker lock(&pacing.mutex);
    const int strikes = ++pacing.strikes[host];
    const int escalated = qMin(kMaxRetryAfterMs, kBaseCooldownMs << qMin(strikes - 1, 5));
    const int cooldown = qMax(delayMs, escalated);
    qint64& next = pacing.nextSlotMs[host];
    next = qMax(next, QDateTime::currentMSecsSinceEpoch() + cooldown);
    return cooldown;
}

void clearStrikes(const QString& host) {
    HostPacing& pacing = hostPacing();
    QMutexLocker lock(&pacing.mutex);
    pacing.strikes.remove(host);
}

}  // namespace

HttpSource::ImpatientScope::ImpatientScope(int maxWaitMs) : m_previous(t_maxWaitMs) {
    t_maxWaitMs = qMax(0, maxWaitMs);
}

HttpSource::ImpatientScope::~ImpatientScope() { t_maxWaitMs = m_previous; }

bool HttpSource::lastRequestRateLimited() { return t_lastRateLimited; }

void HttpSource::addAuth(QUrlQuery& params) const { Q_UNUSED(params); }

QByteArray HttpSource::userAgent() const { return QByteArray(kDefaultUserAgent); }

void HttpSource::addHeaders(QNetworkRequest& request) const { Q_UNUSED(request); }

int HttpSource::requestIntervalMs(const QString& host) const {
    Q_UNUSED(host);
    return 0;
}

QNetworkRequest HttpSource::makeRequest(const QString& url) const {
    QNetworkRequest request(QUrl{url});
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", "*/*");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    addHeaders(request);
    return request;
}

QByteArray HttpSource::httpGet(const QString& url, QString* error) const {
    const QString host = QUrl(url).host();
    const bool impatient = t_maxWaitMs >= 0;
    t_lastRateLimited = false;
    for (int attempt = 0;; ++attempt) {
        if (!waitForSlot(host, requestIntervalMs(host), t_maxWaitMs)) {
            t_lastRateLimited = true;
            if (error)
                *error = QStringLiteral("%1 is rate limiting - skipped").arg(host);
            qDebug().noquote() << "HTTP GET skipped, host cooling down --" << url;
            return {};
        }

        QNetworkAccessManager manager;
        QNetworkReply* reply = manager.get(makeRequest(url));

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start(kTimeoutMs);
        loop.exec();

        if (timeout.isActive() && isRateLimited(reply)) {
            const int cooldownMs = backOff(host, retryDelayMs(reply, attempt));
            if (!impatient && attempt < kMaxRateLimitRetries) {
                qWarning().noquote() << "HTTP 429, retrying in" << cooldownMs << "ms --" << url;
                reply->deleteLater();
                continue;
            }
            // Impatient callers give up at once; the cooldown still
            // protects the host from everyone else.
            t_lastRateLimited = true;
            if (impatient)
                qWarning().noquote() << "HTTP 429, not retrying (cooling" << host << "for"
                                     << cooldownMs << "ms) --" << url;
        } else if (timeout.isActive() && reply->error() == QNetworkReply::NoError) {
            clearStrikes(host);
        }

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
}

bool HttpSource::httpDownload(const QString& url, const QString& path,
                              const std::function<void(qint64, qint64)>& progress,
                              QString* error) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(path);
        qWarning().noquote() << "HTTP download: cannot open file:" << path << "--"
                             << file.errorString();
        return false;
    }

    const QString host = QUrl(url).host();
    const bool impatient = t_maxWaitMs >= 0;
    t_lastRateLimited = false;
    for (int attempt = 0;; ++attempt) {
        if (!waitForSlot(host, requestIntervalMs(host), t_maxWaitMs)) {
            t_lastRateLimited = true;
            file.close();
            QFile::remove(path);
            if (error)
                *error = QStringLiteral("%1 is rate limiting - skipped").arg(host);
            qDebug().noquote() << "HTTP download skipped, host cooling down --" << url;
            return false;
        }
        // A retried attempt starts over; drop the rate-limit page's body.
        file.resize(0);
        file.seek(0);

        QNetworkAccessManager manager;
        QNetworkReply* reply = manager.get(makeRequest(url));

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

        if (timeout.isActive() && isRateLimited(reply)) {
            const int cooldownMs = backOff(host, retryDelayMs(reply, attempt));
            if (!impatient && attempt < kMaxRateLimitRetries) {
                qWarning().noquote()
                    << "HTTP 429, retrying download in" << cooldownMs << "ms --" << url;
                reply->deleteLater();
                continue;
            }
            t_lastRateLimited = true;
        } else if (timeout.isActive() && reply->error() == QNetworkReply::NoError) {
            clearStrikes(host);
        }

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
