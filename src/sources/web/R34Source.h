#pragma once

#include "HttpSource.h"

namespace Mosaic {

// Source for rule34.us ("Modified Gelbooru 0.1" HTML API, parsed with
// regular expressions; search is 0-based, omit page=0). Full file URLs are
// derived from previews (/thumbnails/ -> /images/, strip thumbnail_);
// videos move to the //video. host.
class R34Source : public HttpSource {
   public:
    static constexpr int kMaxLimit = 20;

    QString id() const override;
    QString displayName() const override;

    SourceResult search(const SourceQuery& query) override;

    // Resolves the real full-size URL off the post page when the derived
    // one would 404.
    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;
    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;

   protected:
    QByteArray userAgent() const override;

   private:
    QString resolveFullUrl(const SourceImage& image) const;
};

}  // namespace Mosaic
