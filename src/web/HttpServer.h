#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUrlQuery>

#include <functional>

class QJsonArray;
class QJsonObject;
class QTcpServer;

namespace Mosaic {

class HttpConnection;
class HttpServer;

struct HttpRequest {
    QByteArray method;  // "GET", "POST", ...
    QString path;       // percent-decoded, no query
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;  // names lowercased
    QByteArray body;
    QHostAddress peer;

    QByteArray header(const QByteArray& lowerName) const { return headers.value(lowerName); }
    QString cookie(const QString& name) const;
    bool isLoopback() const;
};

struct HttpResponse {
    int status = 200;
    QByteArray contentType;
    QByteArray body;
    // When set, the file is streamed instead of `body` (Range requests are
    // honored, so <video> can seek).
    QString filePath;
    QList<QPair<QByteArray, QByteArray>> headers;

    HttpResponse& header(const QByteArray& name, const QByteArray& value) {
        headers.append({name, value});
        return *this;
    }

    static HttpResponse json(const QJsonObject& object, int status = 200);
    static HttpResponse json(const QJsonArray& array, int status = 200);
    static HttpResponse text(int status, const QString& message);
    static HttpResponse file(const QString& path, const QByteArray& contentType = {});
    static HttpResponse redirect(const QByteArray& location);

    // Content type from a file extension (falls back to octet-stream).
    static QByteArray mimeForSuffix(const QString& suffix);
};

// Answers one request. Copyable and callable from any thread; the answer is
// written on the server's thread. Responding twice, or after the client
// went away, is a no-op.
class HttpResponder {
   public:
    HttpResponder() = default;
    void send(const HttpResponse& response) const;

   private:
    friend class HttpServer;
    friend class HttpConnection;
    HttpResponder(HttpServer* server, quint64 connectionId, quint64 requestSeq)
        : m_server(server), m_connectionId(connectionId), m_requestSeq(requestSeq) {}

    QPointer<HttpServer> m_server;
    quint64 m_connectionId = 0;
    quint64 m_requestSeq = 0;
};

// Minimal HTTP/1.1 server on QTcpServer: keep-alive, one request in flight
// per connection, bodies up to 4 MB, single-range file streaming. Lives on
// the GUI thread; the handler is called there and must hand slow work to a
// worker pool (it gets a responder for that).
class HttpServer : public QObject {
   public:
    using Handler = std::function<void(const HttpRequest&, const HttpResponder&)>;

    explicit HttpServer(QObject* parent = nullptr);
    ~HttpServer() override;

    void setHandler(Handler handler) { m_handler = std::move(handler); }

    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    // Stops listening and drops every open connection.
    void close();
    bool isListening() const;
    quint16 port() const;

   private:
    friend class HttpConnection;
    friend class HttpResponder;

    void onNewConnection();
    void dispatch(HttpConnection* connection, const HttpRequest& request, quint64 seq);
    void deliver(quint64 connectionId, quint64 seq, const HttpResponse& response);
    void forget(quint64 connectionId);

    QTcpServer* m_server = nullptr;
    Handler m_handler;
    QHash<quint64, HttpConnection*> m_connections;
    quint64 m_nextConnectionId = 1;
};

}  // namespace Mosaic
