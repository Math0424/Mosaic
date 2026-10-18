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
    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;

   protected:
    // Override for sites that reject the Mosaic agent (Cloudflare).
    virtual QByteArray userAgent() const;

    // Extra request headers (e.g. client hints for Cloudflare-fronted sites).
    virtual void addHeaders(QNetworkRequest& request) const;

    QByteArray httpGet(const QString& url, QString* error = nullptr) const;

    bool httpDownload(const QString& url, const QString& path,
                      const std::function<void(qint64, qint64)>& progress = nullptr,
                      QString* error = nullptr) const;

    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;

    virtual void addAuth(QUrlQuery& params) const;

   private:
    QNetworkRequest makeRequest(const QString& url) const;
};

}  // namespace Mosaic
