#include "HttpServer.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace Mosaic {

namespace {

constexpr qsizetype kMaxHeaderBytes = 32 * 1024;
constexpr qint64 kMaxBodyBytes = 4LL * 1024 * 1024;
constexpr qint64 kFileChunkBytes = 256 * 1024;
// Keep this much queued in the socket while streaming a file.
constexpr qint64 kSocketHighWater = 1024 * 1024;
constexpr int kIdleTimeoutMs = 120000;

QByteArray reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

// Parses a single "bytes=a-b" range against a file of `size` bytes.
// Returns false for anything unsatisfiable or multi-range.
bool parseRange(const QByteArray& header, qint64 size, qint64* start, qint64* end) {
    if (!header.startsWith("bytes=") || header.contains(','))
        return false;
    const QByteArray spec = header.mid(6).trimmed();
    const int dash = spec.indexOf('-');
    if (dash < 0)
        return false;
    const QByteArray a = spec.left(dash).trimmed();
    const QByteArray b = spec.mid(dash + 1).trimmed();
    bool ok = true;
    if (a.isEmpty()) {
        // Suffix range: the last N bytes.
        const qint64 n = b.toLongLong(&ok);
        if (!ok || n <= 0)
            return false;
        *start = qMax<qint64>(0, size - n);
        *end = size - 1;
    } else {
        *start = a.toLongLong(&ok);
        if (!ok)
            return false;
        *end = b.isEmpty() ? size - 1 : b.toLongLong(&ok);
        if (!ok)
            return false;
        *end = qMin(*end, size - 1);
    }
    return *start >= 0 && *start <= *end && *start < size;
}

}  // namespace

// request / response helpers

QString HttpRequest::cookie(const QString& name) const {
    const QList<QByteArray> parts = header("cookie").split(';');
    for (const QByteArray& part : parts) {
        const int eq = part.indexOf('=');
        if (eq < 0)
            continue;
        if (QString::fromUtf8(part.left(eq).trimmed()) == name)
            return QString::fromUtf8(part.mid(eq + 1).trimmed());
    }
    return {};
}

bool HttpRequest::isLoopback() const {
    if (peer.isLoopback())
        return true;
    // IPv4-mapped IPv6 loopback (::ffff:127.0.0.1) on a dual-stack socket.
    bool ok = false;
    const quint32 v4 = peer.toIPv4Address(&ok);
    return ok && (v4 >> 24) == 127;
}

HttpResponse HttpResponse::json(const QJsonObject& object, int status) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    r.header("Cache-Control", "no-store");
    return r;
}

HttpResponse HttpResponse::json(const QJsonArray& array, int status) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = QJsonDocument(array).toJson(QJsonDocument::Compact);
    r.header("Cache-Control", "no-store");
    return r;
}

HttpResponse HttpResponse::text(int status, const QString& message) {
    HttpResponse r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = message.toUtf8();
    return r;
}

HttpResponse HttpResponse::file(const QString& path, const QByteArray& contentType) {
    HttpResponse r;
    r.filePath = path;
    r.contentType =
        contentType.isEmpty() ? mimeForSuffix(QFileInfo(path).suffix()) : contentType;
    return r;
}

HttpResponse HttpResponse::redirect(const QByteArray& location) {
    HttpResponse r;
    r.status = 302;
    r.header("Location", location);
    return r;
}

QByteArray HttpResponse::mimeForSuffix(const QString& suffix) {
    static const QHash<QString, QByteArray> types = {
        {QStringLiteral("html"), "text/html; charset=utf-8"},
        {QStringLiteral("js"), "text/javascript; charset=utf-8"},
        {QStringLiteral("css"), "text/css; charset=utf-8"},
        {QStringLiteral("json"), "application/json; charset=utf-8"},
        {QStringLiteral("webmanifest"), "application/manifest+json"},
        {QStringLiteral("svg"), "image/svg+xml"},
        {QStringLiteral("png"), "image/png"},
        {QStringLiteral("apng"), "image/apng"},
        {QStringLiteral("jpg"), "image/jpeg"},
        {QStringLiteral("jpeg"), "image/jpeg"},
        {QStringLiteral("gif"), "image/gif"},
        {QStringLiteral("webp"), "image/webp"},
        {QStringLiteral("avif"), "image/avif"},
        {QStringLiteral("bmp"), "image/bmp"},
        {QStringLiteral("ico"), "image/x-icon"},
        {QStringLiteral("webm"), "video/webm"},
        {QStringLiteral("mp4"), "video/mp4"},
        {QStringLiteral("m4v"), "video/mp4"},
        {QStringLiteral("mov"), "video/quicktime"},
        {QStringLiteral("mkv"), "video/x-matroska"},
        {QStringLiteral("txt"), "text/plain; charset=utf-8"},
    };
    return types.value(suffix.toLower(), "application/octet-stream");
}

// connection

class HttpConnection : public QObject {
   public:
    HttpConnection(HttpServer* server, QTcpSocket* socket, quint64 id)
        : QObject(server), m_server(server), m_socket(socket), m_id(id) {
        m_socket->setParent(this);
        m_idle.setSingleShot(true);
        m_idle.setInterval(kIdleTimeoutMs);
        connect(&m_idle, &QTimer::timeout, this, [this]() {
            // A slow handler (a feed batch can take a while) keeps the
            // connection; only a truly idle one is dropped.
            if (m_busy)
                m_idle.start();
            else
                m_socket->abort();
        });
        connect(m_socket, &QTcpSocket::readyRead, this, [this]() {
            m_idle.start();
            m_buffer += m_socket->readAll();
            tryParse();
        });
        connect(m_socket, &QTcpSocket::bytesWritten, this, [this]() {
            m_idle.start();
            pump();
        });
        connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
            m_server->forget(m_id);
            deleteLater();
        });
        m_idle.start();
    }

    quint64 id() const { return m_id; }

    void abort() { m_socket->abort(); }

    void respond(quint64 seq, const HttpResponse& response) {
        if (!m_busy || seq != m_seq || m_socket->state() != QAbstractSocket::ConnectedState)
            return;
        if (!response.filePath.isEmpty()) {
            respondFile(response);
            return;
        }
        writeHead(response.status, response.contentType, response.body.size(), response.headers);
        if (!m_headOnly && !response.body.isEmpty())
            m_socket->write(response.body);
        finishResponse();
    }

   private:
    void tryParse() {
        if (m_busy)
            return;  // pipelined request: handled after the current one
        const qsizetype headerEnd = m_buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (m_buffer.size() > kMaxHeaderBytes)
                failAndClose(431, QStringLiteral("Headers too large"));
            return;
        }

        const QList<QByteArray> lines = m_buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        if (requestLine.size() != 3) {
            failAndClose(400, QStringLiteral("Bad request line"));
            return;
        }

        HttpRequest request;
        request.method = requestLine.at(0).toUpper();
        request.peer = m_socket->peerAddress();
        const QByteArray version = requestLine.at(2).toUpper();
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            request.headers.insert(line.left(colon).trimmed().toLower(),
                                   line.mid(colon + 1).trimmed());
        }

        bool lengthOk = true;
        const QByteArray lengthHeader = request.header("content-length");
        const qint64 length = lengthHeader.isEmpty() ? 0 : lengthHeader.toLongLong(&lengthOk);
        if (!lengthOk || length < 0 || length > kMaxBodyBytes) {
            failAndClose(413, QStringLiteral("Body too large"));
            return;
        }
        if (m_buffer.size() < headerEnd + 4 + length)
            return;  // wait for the rest of the body

        request.body = m_buffer.mid(headerEnd + 4, length);
        m_buffer.remove(0, headerEnd + 4 + length);

        const QUrl url = QUrl::fromEncoded(requestLine.at(1));
        request.path = url.path(QUrl::FullyDecoded);
        request.query = QUrlQuery(url.query(QUrl::FullyEncoded));
        if (!request.path.startsWith(QLatin1Char('/'))) {
            failAndClose(400, QStringLiteral("Bad request target"));
            return;
        }

        const QByteArray connectionHeader = request.header("connection").toLower();
        m_keepAlive = version == "HTTP/1.1" ? !connectionHeader.contains("close")
                                            : connectionHeader.contains("keep-alive");
        m_headOnly = request.method == "HEAD";
        m_rangeHeader = request.header("range");
        m_ifNoneMatch = request.header("if-none-match");
        m_busy = true;
        ++m_seq;
        m_server->dispatch(this, request, m_seq);
    }

    void writeHead(int status, const QByteArray& contentType, qint64 length,
                   const QList<QPair<QByteArray, QByteArray>>& extra) {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) +
                          "\r\n";
        if (!contentType.isEmpty())
            head += "Content-Type: " + contentType + "\r\n";
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
        head += m_keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += "X-Content-Type-Options: nosniff\r\n";
        for (const auto& h : extra) head += h.first + ": " + h.second + "\r\n";
        head += "\r\n";
        m_socket->write(head);
    }

    void respondFile(const HttpResponse& response) {
        auto file = std::make_unique<QFile>(response.filePath);
        if (!file->open(QIODevice::ReadOnly)) {
            respond(m_seq, HttpResponse::text(404, QStringLiteral("Not found")));
            return;
        }
        const qint64 size = file->size();
        const QFileInfo info(response.filePath);
        const QByteArray etag = '"' + QByteArray::number(size, 16) + '-' +
                                QByteArray::number(info.lastModified().toMSecsSinceEpoch(), 16) +
                                '"';
        auto headers = response.headers;
        headers.append({QByteArray("Accept-Ranges"), QByteArray("bytes")});
        headers.append({QByteArray("ETag"), etag});

        if (!m_ifNoneMatch.isEmpty() && m_ifNoneMatch == etag) {
            writeHead(304, {}, 0, headers);
            finishResponse();
            return;
        }

        qint64 start = 0;
        qint64 end = size - 1;
        int status = response.status;
        if (!m_rangeHeader.isEmpty() && size > 0) {
            if (!parseRange(m_rangeHeader, size, &start, &end)) {
                headers.append({QByteArray("Content-Range"), "bytes */" + QByteArray::number(size)});
                writeHead(416, {}, 0, headers);
                finishResponse();
                return;
            }
            status = 206;
            headers.append({QByteArray("Content-Range"), "bytes " + QByteArray::number(start) + '-' +
                                                 QByteArray::number(end) + '/' +
                                                 QByteArray::number(size)});
        }
        const qint64 length = size > 0 ? end - start + 1 : 0;
        writeHead(status, response.contentType, length, headers);
        if (m_headOnly || length == 0) {
            finishResponse();
            return;
        }
        file->seek(start);
        m_file = std::move(file);
        m_remaining = length;
        pump();
    }

    // Feeds the next file chunks while the socket has room.
    void pump() {
        if (!m_file)
            return;
        while (m_remaining > 0 && m_socket->bytesToWrite() < kSocketHighWater) {
            const QByteArray chunk = m_file->read(qMin(kFileChunkBytes, m_remaining));
            if (chunk.isEmpty()) {
                // File shrank under us: the promised length can't be met.
                m_file.reset();
                m_socket->abort();
                return;
            }
            m_remaining -= chunk.size();
            m_socket->write(chunk);
        }
        if (m_remaining == 0) {
            m_file.reset();
            finishResponse();
        }
    }

    void finishResponse() {
        m_busy = false;
        m_rangeHeader.clear();
        m_ifNoneMatch.clear();
        if (!m_keepAlive) {
            m_socket->disconnectFromHost();
            return;
        }
        if (!m_buffer.isEmpty())
            QTimer::singleShot(0, this, [this]() { tryParse(); });
    }

    void failAndClose(int status, const QString& message) {
        m_keepAlive = false;
        m_busy = true;
        ++m_seq;
        respond(m_seq, HttpResponse::text(status, message));
    }

    HttpServer* m_server;
    QTcpSocket* m_socket;
    quint64 m_id;
    QTimer m_idle;
    QByteArray m_buffer;
    bool m_busy = false;
    quint64 m_seq = 0;
    bool m_keepAlive = true;
    bool m_headOnly = false;
    QByteArray m_rangeHeader;
    QByteArray m_ifNoneMatch;
    std::unique_ptr<QFile> m_file;
    qint64 m_remaining = 0;
};

// responder

void HttpResponder::send(const HttpResponse& response) const {
    HttpServer* server = m_server.data();
    if (!server)
        return;
    const quint64 id = m_connectionId;
    const quint64 seq = m_requestSeq;
    QMetaObject::invokeMethod(
        server, [server, id, seq, response]() { server->deliver(id, seq, response); },
        Qt::QueuedConnection);
}

// server

HttpServer::HttpServer(QObject* parent) : QObject(parent) {}

HttpServer::~HttpServer() { close(); }

bool HttpServer::listen(const QHostAddress& address, quint16 port, QString* error) {
    close();
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
    if (!m_server->listen(address, port)) {
        if (error)
            *error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    return true;
}

void HttpServer::close() {
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    const auto connections = m_connections;
    m_connections.clear();
    for (HttpConnection* c : connections) {
        // abort() emits disconnected synchronously; forget() is then a no-op.
        c->abort();
        c->deleteLater();
    }
}

bool HttpServer::isListening() const { return m_server && m_server->isListening(); }

quint16 HttpServer::port() const { return m_server ? m_server->serverPort() : 0; }

void HttpServer::onNewConnection() {
    while (QTcpSocket* socket = m_server->nextPendingConnection()) {
        const quint64 id = m_nextConnectionId++;
        m_connections.insert(id, new HttpConnection(this, socket, id));
    }
}

void HttpServer::dispatch(HttpConnection* connection, const HttpRequest& request, quint64 seq) {
    const HttpResponder responder(this, connection->id(), seq);
    if (!m_handler) {
        responder.send(HttpResponse::text(503, QStringLiteral("No handler")));
        return;
    }
    m_handler(request, responder);
}

void HttpServer::deliver(quint64 connectionId, quint64 seq, const HttpResponse& response) {
    if (HttpConnection* c = m_connections.value(connectionId))
        c->respond(seq, response);
}

void HttpServer::forget(quint64 connectionId) { m_connections.remove(connectionId); }

}  // namespace Mosaic
