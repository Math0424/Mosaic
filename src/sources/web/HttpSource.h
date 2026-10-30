#pragma once

#include <QNetworkRequest>

#include <functional>

#include "../Source.h"

class QUrlQuery;

namespace Mosaic {

// Base class for remote HTTP sources: buffered GET, streaming download
// and default URL-based file access. Concrete sites implement search(),
// parentOf() and childrenOf().
class HttpSource : public Source {
   public:
    // While alive, requests made on this thread fail fast instead of
    // waiting out a rate limit: no 429 retries, and a request whose host
    // slot is more than `maxWaitMs` away is skipped without being sent.
    // For latency-sensitive callers (the feed) that can use another site;
    // browsing stays patient. Scopes nest.
    class ImpatientScope {
       public:
        explicit ImpatientScope(int maxWaitMs = 1500);
        ~ImpatientScope();
        ImpatientScope(const ImpatientScope&) = delete;
        ImpatientScope& operator=(const ImpatientScope&) = delete;

       private:
        int m_previous;
    };

    // True when this thread's most recent request failed because of a
    // rate limit (429, or skipped by an ImpatientScope).
    static bool lastRequestRateLimited();

    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;

   protected:
    // Override for sites that reject the Mosaic agent (Cloudflare).
    virtual QByteArray userAgent() const;

    // Extra request headers (e.g. client hints for Cloudflare-fronted sites).
    virtual void addHeaders(QNetworkRequest& request) const;

    // Minimum spacing between requests to one host, shared by every thread
    // using the source. 0 (the default) only honours 429 back-off.
    virtual int requestIntervalMs(const QString& host) const;

    QByteArray httpGet(const QString& url, QString* error = nullptr) const;

    // httpGet for media files (content-addressed URLs): concurrent requests
    // for one URL share a single download, and recent results are reused.
    // Not for API/search pages, whose content changes.
    QByteArray mediaGet(const QString& url, QString* error = nullptr) const;

    bool httpDownload(const QString& url, const QString& path,
                      const std::function<void(qint64, qint64)>& progress = nullptr,
                      QString* error = nullptr) const;

    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;

    virtual void addAuth(QUrlQuery& params) const;

   private:
    QNetworkRequest makeRequest(const QString& url) const;
};

}  // namespace Mosaic
