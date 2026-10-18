#pragma once

#include "HttpSource.h"

namespace Mosaic {

// Source for rule34.xxx (Gelbooru 0.1 HTML pages, parsed with regular
// expressions). Pagination is a post offset (`pid`), 42 posts per page.
// The original file's extension differs from the thumbnail's, so full file
// URLs are resolved from the post page.
class Rule34XxxSource : public HttpSource {
   public:
    static constexpr int kPageSize = 42;

    QString id() const override;
    QString displayName() const override;

    SourceResult search(const SourceQuery& query) override;

    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;
    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;

   protected:
    QByteArray userAgent() const override;
    void addHeaders(QNetworkRequest& request) const override;

   private:
    QString resolveFullUrl(const SourceImage& image) const;
};

}  // namespace Mosaic
