#pragma once

#include <QJsonObject>

#include "HttpSource.h"

namespace Mosaic {

// Source for Gelbooru 0.2 API sites (gelbooru.com, rule34.xxx,
// safebooru.org), all sharing the JSON dapi endpoint. `pid` is a 0-based
// post offset. Older installs answer with a bare JSON array of posts.
class GelbooruSource : public HttpSource {
   public:
    static constexpr int kMaxLimit = 100;

    GelbooruSource(const QString& id, const QString& displayName, const QString& apiBase,
                   const QString& siteBase);

    QString id() const override;
    QString displayName() const override;

    SourceResult search(const SourceQuery& query) override;
    SourceImage parentOf(const SourceImage& image, QString* error = nullptr) override;
    QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr) override;

    // Optional dapi credentials (rule34.xxx requires them).
    void setCredentials(const QString& userId, const QString& apiKey) {
        m_userId = userId;
        m_apiKey = apiKey;
    }

    bool hasCredentials() const { return !m_userId.isEmpty() && !m_apiKey.isEmpty(); }

    // Only usable with configured credentials; hidden otherwise.
    bool isEnabled() const override { return hasCredentials(); }

   protected:
    void addAuth(QUrlQuery& params) const override;

   private:
    bool parsePost(const QJsonObject& post, SourceImage& out) const;

    SourceImage postById(qint64 id, QString* error) const;

    QString m_id;
    QString m_displayName;
    QString m_apiBase;
    QString m_siteBase;
    QString m_userId;
    QString m_apiKey;
};

}  // namespace Mosaic
