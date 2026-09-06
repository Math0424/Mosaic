#pragma once

#include "HttpSource.h"

namespace Mosaic {

// Source for e621.net (JSON API). Site settings live in code; the JSON
// response is parsed directly with QJsonDocument.
class E621Source : public HttpSource {
   public:
    static constexpr int kMaxLimit = 320;

    QString id() const override;
    QString displayName() const override;

    SourceResult search(const SourceQuery& query) override;
    SourceImage parentOf(const SourceImage& image, QString* error = nullptr) override;
    QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr) override;
};

}  // namespace Mosaic
