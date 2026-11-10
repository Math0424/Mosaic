#include "WebServer.h"

#include "../core/Paths.h"
#include "../core/ThreadPools.h"
#include "../feed/FeedEngine.h"
#include "../media/VideoDecoder.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibraryCatalog.h"
#include "../sources/local/LibrarySource.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThreadPool>
#include <QUrl>

#include <utility>

namespace Mosaic {

namespace {

constexpr int kFeedBatchSize = 12;
// A batch computed ahead is served only while fresh; older ones are re-ranked.
constexpr qint64 kReadyBatchMaxAgeMs = 10 * 60 * 1000;
constexpr int kMaxFeedPosts = 3000;   // remembered posts for media/reactions
constexpr int kMaxServedKeys = 6000;  // cross-batch duplicate filter
constexpr int kThumbBox = 360;        // px, longest thumbnail side
constexpr int kMaxSuggestions = 20;
constexpr qint64 kTagListTtlMs = 5 * 60 * 1000;
constexpr qint64 kMediaCacheBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 kPruneIntervalMs = 60 * 1000;
constexpr int kMaxSearchLimit = 200;

const QByteArray kCookieName = "mosaic_token";

QString webMediaDir() { return QDir(Paths::tempDir()).filePath(QStringLiteral("webmedia")); }
QString webThumbDir() { return QDir(Paths::tempDir()).filePath(QStringLiteral("webthumbs")); }

bool isMd5(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{32}$"));
    return re.match(s).hasMatch();
}

// "<md5>.<ext>" from a URL segment; false for anything else (no paths).
bool splitFileName(const QString& name, QString* md5, QString* ext) {
    static const QRegularExpression re(QStringLiteral("^([0-9a-fA-F]{32})(?:\\.([0-9A-Za-z]{1,8}))?$"));
    const QRegularExpressionMatch m = re.match(name);
    if (!m.hasMatch())
        return false;
    *md5 = m.captured(1).toLower();
    *ext = m.captured(2).toLower();
    return true;
}

QString suffixOfUrl(const QString& url) {
    const QString path = QUrl(url).path();
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (dot < 0 || dot < slash)
        return {};
    const QString suffix = path.mid(dot + 1).toLower();
    static const QRegularExpression re(QStringLiteral("^[0-9a-z]{1,8}$"));
    return re.match(suffix).hasMatch() ? suffix : QString();
}

QString formatOf(const SourceImage& img) {
    QString f = img.format.toLower();
    if (f.isEmpty() && !img.localPath.isEmpty())
        f = QFileInfo(img.localPath).suffix().toLower();
    if (f.isEmpty())
        f = suffixOfUrl(img.fileUrl);
    return f == QLatin1String("jpeg") ? QStringLiteral("jpg") : f;
}

bool isVideoFormat(const QString& f) {
    static const QSet<QString> video = {QStringLiteral("webm"), QStringLiteral("mp4"),
                                        QStringLiteral("m4v"), QStringLiteral("mov"),
                                        QStringLiteral("mkv")};
    return video.contains(f);
}

// Formats a browser can't show at all.
bool isUnsupportedFormat(const QString& f) {
    static const QSet<QString> unsupported = {QStringLiteral("swf"), QStringLiteral("zip"),
                                              QStringLiteral("rar"), QStringLiteral("7z"),
                                              QStringLiteral("psd")};
    return unsupported.contains(f);
}

QString kindName(FileKind kind) {
    switch (kind) {
        case FileKind::Preview: return QStringLiteral("preview");
        case FileKind::Sample: return QStringLiteral("sample");
        case FileKind::Full: return QStringLiteral("full");
    }
    return QStringLiteral("full");
}

FileKind kindFromName(const QString& name) {
    if (name == QLatin1String("preview"))
        return FileKind::Preview;
    if (name == QLatin1String("sample"))
        return FileKind::Sample;
    return FileKind::Full;
}

QJsonArray tagsJson(const SourceImage& img) {
    QJsonArray tags;
    for (const SourceTag& t : img.tags) {
        QJsonObject o;
        o[QStringLiteral("n")] = t.name;
        if (!t.category.isEmpty())
            o[QStringLiteral("c")] = t.category;
        tags.append(o);
    }
    return tags;
}

QJsonObject basePostJson(const SourceImage& img, const QString& format) {
    QJsonObject o;
    o[QStringLiteral("md5")] = img.md5.toLower();
    o[QStringLiteral("source")] = img.sourceId;
    o[QStringLiteral("id")] = img.remoteId;
    o[QStringLiteral("w")] = img.width;
    o[QStringLiteral("h")] = img.height;
    o[QStringLiteral("format")] = format;
    o[QStringLiteral("video")] = isVideoFormat(format);
    o[QStringLiteral("score")] = img.score;
    o[QStringLiteral("rating")] = img.rating;
    o[QStringLiteral("favorite")] = img.favorite;
    o[QStringLiteral("tags")] = tagsJson(img);
    return o;
}

QJsonObject libraryPostJson(const SourceImage& img) {
    const QString format = formatOf(img);
    QJsonObject o = basePostJson(img, format);
    const QString name = img.md5.toLower() + (format.isEmpty() ? QString() : QLatin1Char('.') + format);
    o[QStringLiteral("thumb")] = QStringLiteral("/lib/thumb/") + name;
    o[QStringLiteral("media")] = QStringLiteral("/lib/file/") + name;
    o[QStringLiteral("saved")] = true;
    return o;
}

QJsonObject feedPostJson(const SourceImage& img, const QString& sourceName, bool saved) {
    const QString format = formatOf(img);
    QJsonObject o = basePostJson(img, format);
    const QString base = QStringLiteral("/feed/media/") + img.md5.toLower();
    o[QStringLiteral("sourceName")] = sourceName;
    o[QStringLiteral("thumb")] = base + QStringLiteral("?kind=preview");
    // Phones get the sample for stills (much lighter); animated formats
    // need the full file, since samples are often a static frame.
    const bool needsFull = isVideoFormat(format) || format == QLatin1String("gif") ||
                           format == QLatin1String("webp") || format == QLatin1String("apng");
    o[QStringLiteral("media")] = base + (needsFull ? QStringLiteral("?kind=full")
                                                   : QStringLiteral("?kind=sample"));
    o[QStringLiteral("full")] = base + QStringLiteral("?kind=full");
    o[QStringLiteral("saved")] = saved;
    return o;
}

// Decodes a thumbnail-sized frame of a library file (image or video).
QImage makeThumbnail(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > kThumbBox || size.height() > kThumbBox))
        reader.setScaledSize(size.scaled(kThumbBox, kThumbBox, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) {
        VideoDecoder decoder;
        if (decoder.open(path))
            image = decoder.nextFrame().image;
    }
    if (!image.isNull() && (image.width() > kThumbBox || image.height() > kThumbBox))
        image = image.scaled(kThumbBox, kThumbBox, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// Keeps the proxied-media cache under its byte budget, oldest first.
void pruneMediaCache() {
    QDir dir(webMediaDir());
    QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    const QDateTime staleParts = QDateTime::currentDateTime().addSecs(-3600);
    qint64 total = 0;
    for (const QFileInfo& f : files) total += f.size();
    for (const QFileInfo& f : files) {
        const bool stalePart =
            f.suffix() == QLatin1String("part") && f.lastModified() < staleParts;
        if (total <= kMediaCacheBytes && !stalePart)
            continue;
        if (f.suffix() == QLatin1String("part") && !stalePart)
            continue;  // a download in progress
        if (QFile::remove(f.absoluteFilePath()))
            total -= f.size();
    }
}

QByteArray manifestJson() {
    QJsonObject icon;
    icon[QStringLiteral("src")] = QStringLiteral("/icon.svg");
    icon[QStringLiteral("sizes")] = QStringLiteral("any");
    icon[QStringLiteral("type")] = QStringLiteral("image/svg+xml");
    QJsonObject m;
    m[QStringLiteral("name")] = QStringLiteral("Mosaic");
    m[QStringLiteral("short_name")] = QStringLiteral("Mosaic");
    m[QStringLiteral("start_url")] = QStringLiteral("/");
    // Home-screen installs open without browser bars.
    m[QStringLiteral("display")] = QStringLiteral("fullscreen");
    m[QStringLiteral("display_override")] =
        QJsonArray{QStringLiteral("fullscreen"), QStringLiteral("standalone")};
    m[QStringLiteral("background_color")] = QStringLiteral("#000000");
    m[QStringLiteral("theme_color")] = QStringLiteral("#000000");
    m[QStringLiteral("icons")] = QJsonArray{icon};
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}

}  // namespace

WebServer::WebServer(SourceManager& sources, EngineProvider engineProvider, QObject* parent)
    : QObject(parent),
      m_sources(sources),
      m_engineProvider(std::move(engineProvider)),
      m_http(this) {
    m_http.setHandler(
        [this](const HttpRequest& request, const HttpResponder& responder) {
            handle(request, responder);
        });
}

WebServer::~WebServer() { stop(); }

bool WebServer::start(quint16 port, const QString& token, QString* error) {
    m_token = token;
    if (!m_http.listen(QHostAddress::Any, port, error))
        return false;
    QDir().mkpath(webMediaDir());
    QDir().mkpath(webThumbDir());
    qInfo().noquote() << "WebServer: listening on port" << m_http.port();
    return true;
}

void WebServer::stop() {
    if (m_http.isListening())
        qInfo().noquote() << "WebServer: stopped";
    m_http.close();
    m_batchWaiters.clear();
    m_downloads.clear();
}

QString WebServer::generateToken() {
    QByteArray bytes(18, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()),
                                          bytes.size() / sizeof(quint32));
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QStringList WebServer::urls() const {
    QStringList list;
    if (!isRunning())
        return list;
    const QString port = QString::number(m_http.port());
    list << QStringLiteral("http://localhost:%1/").arg(port);
    // Real Wi-Fi/Ethernet first: a phone can't reach Hyper-V/WSL/VM switches,
    // and "Copy link" hands out the first network address.
    static const QRegularExpression virtualName(
        QStringLiteral("vEthernet|WSL|Hyper-V|VirtualBox|VMware|Docker|vbox|virbr|tailscale|ZeroTier"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList physical, virtualOnes;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        const bool isVirtual = iface.type() == QNetworkInterface::Virtual ||
                               virtualName.match(iface.humanReadableName()).hasMatch() ||
                               virtualName.match(iface.name()).hasMatch();
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal())
                continue;
            (isVirtual ? virtualOnes : physical)
                << QStringLiteral("http://%1:%2/?t=%3").arg(ip.toString(), port, m_token);
        }
    }
    list << physical << virtualOnes;
    return list;
}

bool WebServer::authorized(const HttpRequest& request) const {
    if (request.isLoopback())
        return true;
    if (m_token.isEmpty())
        return false;
    return request.cookie(QString::fromLatin1(kCookieName)) == m_token ||
           request.query.queryItemValue(QStringLiteral("t")) == m_token;
}

// routing

void WebServer::handle(const HttpRequest& request, const HttpResponder& responder) {
    const QString& path = request.path;
    const bool isGet = request.method == "GET" || request.method == "HEAD";

    // Harmless public files (fetched by the browser without cookies).
    if (isGet && path == QLatin1String("/manifest.webmanifest")) {
        HttpResponse r;
        r.contentType = "application/manifest+json";
        r.body = manifestJson();
        responder.send(r);
        return;
    }
    if (isGet && (path == QLatin1String("/icon.svg") || path == QLatin1String("/favicon.ico"))) {
        QFile icon(QStringLiteral(":/icon.svg"));
        HttpResponse r;
        r.contentType = "image/svg+xml";
        r.body = icon.open(QIODevice::ReadOnly) ? icon.readAll() : QByteArray();
        r.header("Cache-Control", "public, max-age=86400");
        responder.send(r);
        return;
    }

    if (!authorized(request)) {
        responder.send(HttpResponse::text(
            401, QStringLiteral("Mosaic: open the address shown in Mosaic under "
                                "Tools > Web server (it includes the access key).")));
        return;
    }

    if (isGet && path == QLatin1String("/")) {
        serveApp(request, responder);
    } else if (isGet && path == QLatin1String("/api/library/search")) {
        librarySearch(request, responder);
    } else if (isGet && path == QLatin1String("/api/tags")) {
        tagSuggestions(request, responder);
    } else if (isGet && path.startsWith(QLatin1String("/lib/thumb/"))) {
        libraryThumb(path.mid(11), responder);
    } else if (isGet && path.startsWith(QLatin1String("/lib/file/"))) {
        libraryFile(path.mid(10), responder);
    } else if (isGet && path == QLatin1String("/api/feed/next")) {
        feedNext(request, responder);
    } else if (request.method == "POST" && path == QLatin1String("/api/feed/event")) {
        feedEvent(request, responder);
    } else if ((isGet || request.method == "POST") && path == QLatin1String("/api/feed/settings")) {
        feedSettings(request, responder);
    } else if (isGet && path.startsWith(QLatin1String("/feed/media/"))) {
        feedMedia(path.mid(12), request, responder);
    } else {
        responder.send(HttpResponse::text(404, QStringLiteral("Not found")));
    }
}

void WebServer::serveApp(const HttpRequest& request, const HttpResponder& responder) {
    // First visit with ?t=: keep the key in a cookie and drop it from the
    // address bar (and from anything the page later shares).
    const QString t = request.query.queryItemValue(QStringLiteral("t"));
    if (!t.isEmpty() && t == m_token) {
        HttpResponse r = HttpResponse::redirect("/");
        r.header("Set-Cookie", kCookieName + '=' + m_token.toLatin1() +
                                   "; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax");
        responder.send(r);
        return;
    }
    static const QByteArray page = []() {
        QFile file(QStringLiteral(":/web/index.html"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    HttpResponse r;
    r.contentType = "text/html; charset=utf-8";
    r.body = page;
    r.header("Cache-Control", "no-cache");
    responder.send(r);
}

// library

void WebServer::librarySearch(const HttpRequest& request, const HttpResponder& responder) {
    SourceQuery query;
    query.tags = request.query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded).trimmed();
    query.page = qMax(1, request.query.queryItemValue(QStringLiteral("page")).toInt());
    const int limit = request.query.queryItemValue(QStringLiteral("limit")).toInt();
    query.limit = limit > 0 ? qMin(limit, kMaxSearchLimit) : 60;
    LibrarySource* library = m_sources.library();
    QThreadPool::globalInstance()->start([library, query, responder]() {
        const SourceResult result = library->search(query);
        QJsonObject out;
        QJsonArray posts;
        for (const SourceImage& img : result.images) {
            if (!img.md5.isEmpty())
                posts.append(libraryPostJson(img));
        }
        out[QStringLiteral("posts")] = posts;
        out[QStringLiteral("page")] = query.page;
        out[QStringLiteral("hasMore")] = result.ok && result.images.size() >= query.limit;
        if (!result.ok)
            out[QStringLiteral("error")] = result.error;
        responder.send(HttpResponse::json(out));
    });
}

void WebServer::tagSuggestions(const HttpRequest& request, const HttpResponder& responder) {
    const QString prefix =
        request.query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded).trimmed().toLower();
    if (prefix.isEmpty()) {
        responder.send(HttpResponse::json(QJsonArray{}));
        return;
    }
    LibrarySource* library = m_sources.library();
    QThreadPool::globalInstance()->start([this, library, prefix, responder]() {
        QStringList names;
        {
            // Loaded under the lock so concurrent keystrokes share one load.
            QMutexLocker lock(&m_tagMutex);
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_tagNames.isEmpty() || now - m_tagsLoadedMs > kTagListTtlMs) {
                m_tagNames = library->allTagNames();
                m_tagsLoadedMs = now;
            }
            names = m_tagNames;  // implicitly shared
        }
        QJsonArray out;
        QStringList contains;
        for (const QString& name : names) {
            if (name.startsWith(prefix)) {
                out.append(name);
                if (out.size() >= kMaxSuggestions)
                    break;
            } else if (contains.size() < kMaxSuggestions && name.contains(prefix)) {
                contains << name;
            }
        }
        for (const QString& name : contains) {
            if (out.size() >= kMaxSuggestions)
                break;
            out.append(name);
        }
        responder.send(HttpResponse::json(out));
    });
}

void WebServer::libraryThumb(const QString& name, const HttpResponder& responder) {
    QString md5, ext;
    if (!splitFileName(name, &md5, &ext)) {
        responder.send(HttpResponse::text(404, QStringLiteral("Not found")));
        return;
    }
    QThreadPool::globalInstance()->start([md5, ext, responder]() {
        const QString cachePath = webThumbDir() + QLatin1Char('/') + md5 + QStringLiteral(".jpg");
        if (!QFileInfo::exists(cachePath)) {
            const QString source = LibraryCatalog::filePathFor(md5, ext);
            const QImage thumb = source.isEmpty() ? QImage() : makeThumbnail(source);
            if (thumb.isNull()) {
                responder.send(HttpResponse::text(404, QStringLiteral("No preview")));
                return;
            }
            // Write-then-rename: a parallel request never reads half a file.
            const QString tmp = cachePath + QStringLiteral(".%1.tmp").arg(
                                                QRandomGenerator::global()->generate(), 0, 16);
            if (!thumb.save(tmp, "JPG", 78) ||
                (!QFile::rename(tmp, cachePath) && !QFileInfo::exists(cachePath))) {
                QFile::remove(tmp);
                responder.send(HttpResponse::text(500, QStringLiteral("Thumbnail failed")));
                return;
            }
            QFile::remove(tmp);
        }
        HttpResponse r = HttpResponse::file(cachePath, "image/jpeg");
        r.header("Cache-Control", "private, max-age=604800");
        responder.send(r);
    });
}

void WebServer::libraryFile(const QString& name, const HttpResponder& responder) {
    QString md5, ext;
    if (!splitFileName(name, &md5, &ext)) {
        responder.send(HttpResponse::text(404, QStringLiteral("Not found")));
        return;
    }
    QThreadPool::globalInstance()->start([md5, ext, responder]() {
        const QString path = LibraryCatalog::filePathFor(md5, ext);
        if (path.isEmpty()) {
            responder.send(HttpResponse::text(404, QStringLiteral("Not in library")));
            return;
        }
        HttpResponse r = HttpResponse::file(path);
        r.header("Cache-Control", "private, max-age=86400");
        responder.send(r);
    });
}

// feed

bool WebServer::ensureFeed() {
    if (!m_engine && m_engineProvider) {
        m_engine = m_engineProvider();
        if (m_engine)
            m_engine->startWarmStart();
    }
    return m_engine && m_engine->isOpen();
}

void WebServer::rememberFeedPost(const SourceImage& image) {
    const QString key = image.md5.toLower();
    if (!m_feedPosts.contains(key))
        m_feedPostOrder.append(key);
    m_feedPosts.insert(key, image);
    while (m_feedPostOrder.size() > kMaxFeedPosts) {
        const QString old = m_feedPostOrder.takeFirst();
        m_feedPosts.remove(old);
        m_interacted.remove(old);
    }
}

void WebServer::feedNext(const HttpRequest& request, const HttpResponder& responder) {
    Q_UNUSED(request);
    if (!ensureFeed()) {
        QJsonObject out;
        out[QStringLiteral("posts")] = QJsonArray{};
        out[QStringLiteral("error")] =
            QStringLiteral("Feed unavailable: the feed database could not be opened.");
        responder.send(HttpResponse::json(out, 503));
        return;
    }
    m_batchWaiters.append(responder);
    // A batch computed ahead of time answers at once (unless it went stale);
    // the next one starts right away so there is always one in the oven.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_hasReady && now - m_readyAtMs > kReadyBatchMaxAgeMs)
        m_hasReady = false;
    if (m_hasReady) {
        m_hasReady = false;
        deliverBatch(std::exchange(m_readyBatch, {}), std::exchange(m_readySaved, {}));
    }
    startBatch();
}

// One batch at a time (clients asking meanwhile share its result). No-op
// while one runs, or while a ready batch is already waiting.
void WebServer::startBatch() {
    if (m_batchRunning || m_hasReady || !m_engine)
        return;
    m_batchRunning = true;
    const quint64 generation = m_batchGeneration;
    std::shared_ptr<FeedEngine> engine = m_engine;
    QPointer<WebServer> self(this);
    QThreadPool::globalInstance()->start([self, engine, generation]() {
        const QList<SourceImage> batch = engine->nextBatch(kFeedBatchSize);
        // Saved state drives the save button; asked off the GUI thread.
        QHash<QString, bool> saved;
        LibrarySource library;
        for (const SourceImage& img : batch)
            saved.insert(img.md5.toLower(), !img.localPath.isEmpty() || library.hasImage(img));
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, batch, saved, generation]() {
                if (self)
                    self->finishBatch(batch, saved, generation);
            },
            Qt::QueuedConnection);
    });
}

void WebServer::finishBatch(const QList<SourceImage>& batch, const QHash<QString, bool>& saved,
                            quint64 generation) {
    m_batchRunning = false;
    const bool stale = generation != m_batchGeneration;  // ranked under old settings
    if (!m_batchWaiters.isEmpty()) {
        // Someone is waiting: even a stale batch beats an empty screen.
        deliverBatch(batch, saved);
    } else if (!stale) {
        m_readyBatch = batch;
        m_readySaved = saved;
        m_readyAtMs = QDateTime::currentMSecsSinceEpoch();
        m_hasReady = true;
    }
    // Keep one batch ahead while a client is using the feed.
    if (m_http.isListening())
        startBatch();
}

void WebServer::deliverBatch(const QList<SourceImage>& batch, const QHash<QString, bool>& saved) {
    QJsonArray posts;
    for (const SourceImage& img : batch) {
        const QString md5 = img.md5.toLower();
        if (md5.isEmpty())
            continue;
        const QString postKey = img.remoteId > 0
                                    ? img.sourceId + QLatin1Char(':') + QString::number(img.remoteId)
                                    : QString();
        if (m_servedKeys.contains(md5) || (!postKey.isEmpty() && m_servedKeys.contains(postKey)))
            continue;
        const bool isSaved = saved.value(md5);
        if (!isSaved && img.fileUrl.isEmpty() && img.sampleUrl.isEmpty() &&
            img.previewUrl.isEmpty())
            continue;
        if (isUnsupportedFormat(formatOf(img)))
            continue;
        for (const QString& key : {md5, postKey}) {
            if (key.isEmpty())
                continue;
            m_servedKeys.insert(key);
            m_servedOrder.append(key);
        }
        rememberFeedPost(img);
        Source* source = m_sources.source(img.sourceId);
        posts.append(feedPostJson(img, source ? source->displayName() : img.sourceId, isSaved));
    }
    while (m_servedOrder.size() > kMaxServedKeys) m_servedKeys.remove(m_servedOrder.takeFirst());

    QJsonObject out;
    out[QStringLiteral("posts")] = posts;
    if (posts.isEmpty())
        out[QStringLiteral("message")] = QStringLiteral(
            "No recommendations yet - favorite some posts or add source tabs first.");
    const HttpResponse response = HttpResponse::json(out);
    const QList<HttpResponder> waiters = std::exchange(m_batchWaiters, {});
    for (const HttpResponder& w : waiters) w.send(response);
}

void WebServer::feedSettings(const HttpRequest& request, const HttpResponder& responder) {
    if (!ensureFeed()) {
        responder.send(HttpResponse::json(QJsonObject{{QStringLiteral("ok"), false}}, 503));
        return;
    }
    std::shared_ptr<FeedEngine> engine = m_engine;
    double level = -1.0;
    if (request.method == "POST") {
        const QJsonValue v =
            QJsonDocument::fromJson(request.body).object().value(QStringLiteral("exploration"));
        if (!v.isDouble()) {
            responder.send(HttpResponse::text(400, QStringLiteral("exploration must be 0..1")));
            return;
        }
        level = qBound(0.0, v.toDouble(), 1.0);
        // The batch waiting (or cooking) was ranked under the old level.
        ++m_batchGeneration;
        m_hasReady = false;
        m_readyBatch.clear();
        m_readySaved.clear();
    }
    // The DB write can wait on the training lock: off the GUI thread.
    QThreadPool::globalInstance()->start([engine, level, responder]() {
        if (level >= 0.0)
            engine->db().setExplorationLevel(level);
        QJsonObject out;
        out[QStringLiteral("ok")] = true;
        out[QStringLiteral("exploration")] = engine->db().explorationLevel();
        responder.send(HttpResponse::json(out));
    });
}

void WebServer::feedEvent(const HttpRequest& request, const HttpResponder& responder) {
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    const QString md5 = body.value(QStringLiteral("md5")).toString().toLower();
    const QString type = body.value(QStringLiteral("type")).toString();
    const qint64 dwellMs = qMax<qint64>(0, body.value(QStringLiteral("dwellMs")).toInteger());

    if (!ensureFeed()) {
        responder.send(HttpResponse::json(QJsonObject{{QStringLiteral("ok"), false}}, 503));
        return;
    }
    if (!m_feedPosts.contains(md5)) {
        responder.send(HttpResponse::json(
            QJsonObject{{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("unknown post")}},
            404));
        return;
    }
    const SourceImage img = m_feedPosts.value(md5);
    FeedEngine* engine = m_engine.get();
    const QJsonObject ok{{QStringLiteral("ok"), true}};
    const QJsonObject ignored{{QStringLiteral("ok"), true}, {QStringLiteral("ignored"), true}};

    if (type == QLatin1String("impression")) {
        // Marks the post as seen; carries no taste signal.
        if (!m_interacted.contains(md5)) {
            QThreadPool::globalInstance()->start([engine, img]() {
                engine->db().recordEvent(FeedDatabase::Event::Impression, img.sourceId, img.md5,
                                         FeedEngine::tagNames(img), 0.0, -1, img.remoteId);
            });
        }
        responder.send(HttpResponse::json(ok));
        return;
    }

    FeedDatabase::Event event = FeedDatabase::Event::View;
    const bool isDwell = type == QLatin1String("dwell");
    if (type == QLatin1String("like"))
        event = FeedDatabase::Event::Like;
    else if (type == QLatin1String("dislike"))
        event = FeedDatabase::Event::Dislike;
    else if (type == QLatin1String("favorite"))
        event = FeedDatabase::Event::Favorite;
    else if (!isDwell) {
        responder.send(HttpResponse::text(400, QStringLiteral("Unknown event type")));
        return;
    }

    // One training signal per post, ever (going back never re-trains).
    const bool train = !m_interacted.contains(md5);
    m_interacted.insert(md5);

    if (event == FeedDatabase::Event::Favorite) {
        // Saving always imports, even when the post already trained.
        QThreadPool::globalInstance()->start([engine, img, train, dwellMs, responder]() {
            if (train)
                engine->recordReaction(FeedDatabase::Event::Favorite, img, dwellMs);
            const ImportResult result = engine->saveToLibrary(img);
            QJsonObject out;
            out[QStringLiteral("ok")] = result.ok;
            out[QStringLiteral("alreadyExisted")] = result.alreadyExisted;
            if (!result.ok)
                out[QStringLiteral("error")] = result.error;
            responder.send(HttpResponse::json(out));
        });
        return;
    }
    if (!train) {
        responder.send(HttpResponse::json(ignored));
        return;
    }
    QThreadPool::globalInstance()->start([engine, img, event, isDwell, dwellMs]() {
        if (isDwell)
            engine->recordDwell(img, dwellMs);
        else
            engine->recordReaction(event, img, dwellMs);
    });
    responder.send(HttpResponse::json(ok));
}

void WebServer::feedMedia(const QString& md5In, const HttpRequest& request,
                          const HttpResponder& responder) {
    const QString md5 = md5In.toLower();
    if (!isMd5(md5) || !m_feedPosts.contains(md5)) {
        responder.send(HttpResponse::text(404, QStringLiteral("Unknown post")));
        return;
    }
    const SourceImage img = m_feedPosts.value(md5);
    const FileKind kind = kindFromName(request.query.queryItemValue(QStringLiteral("kind")));
    const QString url = img.urlFor(kind);
    QString ext = suffixOfUrl(url);
    if (ext.isEmpty())
        ext = formatOf(img);
    if (ext.isEmpty())
        ext = QStringLiteral("bin");
    const QString cachePath =
        webMediaDir() + QLatin1Char('/') + md5 + QLatin1Char('-') + kindName(kind) + '.' + ext;

    if (QFileInfo::exists(cachePath)) {
        HttpResponse r = HttpResponse::file(cachePath);
        r.header("Cache-Control", "private, max-age=86400");
        responder.send(r);
        return;
    }
    // Already downloading for another client (or the same one re-asking).
    auto it = m_downloads.find(cachePath);
    if (it != m_downloads.end()) {
        it->append(responder);
        return;
    }
    m_downloads.insert(cachePath, {responder});

    Source* source = m_sources.source(img.sourceId);
    if (!source) {
        const QList<Source*> remotes = m_sources.remoteSources();
        source = remotes.isEmpty() ? nullptr : remotes.first();
    }
    QPointer<WebServer> self(this);
    ThreadPools::network()->start([self, source, img, kind, url, cachePath]() {
        QString servedPath;
        QString error;
        // Posts already in the library are served straight from disk.
        LibrarySource library;
        const QString savedMd5 =
            !img.localPath.isEmpty() ? img.md5 : library.savedMd5(img);
        if (!savedMd5.isEmpty()) {
            servedPath = !img.localPath.isEmpty() && QFileInfo::exists(img.localPath)
                             ? img.localPath
                             : LibraryCatalog::filePathFor(savedMd5, img.format);
        }
        if (servedPath.isEmpty()) {
            if (!source || url.isEmpty()) {
                error = QStringLiteral("No source for this post");
            } else {
                const QString part = cachePath + QStringLiteral(".part");
                QFile::remove(part);
                if (source->downloadToFile(img, kind, part, nullptr, &error) &&
                    QFile::rename(part, cachePath)) {
                    servedPath = cachePath;
                } else {
                    QFile::remove(part);
                    if (error.isEmpty())
                        error = QStringLiteral("Download failed");
                }
            }
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, cachePath, servedPath, error]() {
                if (!self)
                    return;
                const QList<HttpResponder> waiters = self->m_downloads.take(cachePath);
                HttpResponse r;
                if (servedPath.isEmpty()) {
                    qDebug().noquote() << "WebServer: media failed:" << error;
                    r = HttpResponse::text(502, error);
                } else {
                    r = HttpResponse::file(servedPath);
                    r.header("Cache-Control", "private, max-age=86400");
                }
                for (const HttpResponder& w : waiters) w.send(r);

                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - self->m_lastPruneMs > kPruneIntervalMs) {
                    self->m_lastPruneMs = now;
                    ThreadPools::network()->start([]() { pruneMediaCache(); });
                }
            },
            Qt::QueuedConnection);
    });
}

}  // namespace Mosaic
