#pragma once

#include "HttpServer.h"

#include "../sources/Source.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>

namespace Mosaic {

class FeedEngine;
class SourceManager;

// Serves the mobile web app: the "For You" feed (TikTok-style, training the
// same recommender as the desktop feed) and the local library (tag search,
// autocomplete, thumbnails, viewer). Runs inside Mosaic because the catalog
// and feed databases allow only one open per process.
//
// Clients on this machine are trusted; any other client needs the access
// token once (?t=...), which is then kept in a cookie.
//
// Routes:
//   GET  /                          web app (embedded resource)
//   GET  /api/library/search        ?q=tags&page=N&limit=M
//   GET  /api/tags                  ?q=prefix  (autocomplete)
//   GET  /lib/thumb/<md5>.<ext>     cached JPEG thumbnail
//   GET  /lib/file/<md5>.<ext>      original file (Range)
//   GET  /api/feed/next             ?count=N  (next recommended posts)
//   POST /api/feed/event            {md5, type, dwellMs}
//   GET  /api/feed/settings         {exploration: 0..1}
//   POST /api/feed/settings         {exploration: 0..1} (same level as the desktop slider)
//   GET  /feed/media/<md5>          ?kind=preview|sample|full (proxied, Range)
class WebServer : public QObject {
   public:
    using EngineProvider = std::function<std::shared_ptr<FeedEngine>()>;

    WebServer(SourceManager& sources, EngineProvider engineProvider, QObject* parent = nullptr);
    ~WebServer() override;

    bool start(quint16 port, const QString& token, QString* error = nullptr);
    void stop();
    bool isRunning() const { return m_http.isListening(); }
    quint16 port() const { return m_http.port(); }

    // Addresses to open the app at: this machine first, then one per LAN
    // IPv4 address (with the access token).
    QStringList urls() const;

    static QString generateToken();

   private:
    void handle(const HttpRequest& request, const HttpResponder& responder);
    bool authorized(const HttpRequest& request) const;

    void serveApp(const HttpRequest& request, const HttpResponder& responder);
    void librarySearch(const HttpRequest& request, const HttpResponder& responder);
    void tagSuggestions(const HttpRequest& request, const HttpResponder& responder);
    void libraryThumb(const QString& name, const HttpResponder& responder);
    void libraryFile(const QString& name, const HttpResponder& responder);
    void feedNext(const HttpRequest& request, const HttpResponder& responder);
    void feedEvent(const HttpRequest& request, const HttpResponder& responder);
    void feedSettings(const HttpRequest& request, const HttpResponder& responder);
    void feedMedia(const QString& md5, const HttpRequest& request,
                   const HttpResponder& responder);

    // Feed state (GUI thread only).
    bool ensureFeed();
    void rememberFeedPost(const SourceImage& image);
    void startBatch();
    void finishBatch(const QList<SourceImage>& batch, const QHash<QString, bool>& saved,
                     quint64 generation);
    void deliverBatch(const QList<SourceImage>& batch, const QHash<QString, bool>& saved);

    SourceManager& m_sources;
    EngineProvider m_engineProvider;
    HttpServer m_http;
    QString m_token;

    // Shared with the desktop feed: same DB, same recommender.
    std::shared_ptr<FeedEngine> m_engine;
    bool m_batchRunning = false;
    QList<HttpResponder> m_batchWaiters;
    // One batch computed ahead, so the next request answers at once.
    QList<SourceImage> m_readyBatch;
    QHash<QString, bool> m_readySaved;
    bool m_hasReady = false;
    qint64 m_readyAtMs = 0;
    quint64 m_batchGeneration = 0;  // bumped by settings changes
    // Posts handed to web clients, so media and reactions can find them.
    QHash<QString, SourceImage> m_feedPosts;
    QStringList m_feedPostOrder;
    QSet<QString> m_servedKeys;      // md5 / "source:id" already sent to a client
    QStringList m_servedOrder;
    QSet<QString> m_interacted;      // one training signal per post
    // In-flight remote media downloads: cache path -> waiting clients.
    QHash<QString, QList<HttpResponder>> m_downloads;
    qint64 m_lastPruneMs = 0;

    // Tag autocomplete list, refreshed lazily off-thread.
    mutable QMutex m_tagMutex;
    QStringList m_tagNames;
    qint64 m_tagsLoadedMs = 0;
};

}  // namespace Mosaic
