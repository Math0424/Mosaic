#pragma once

#include <QJsonObject>
#include <QMutex>

#include "HttpSource.h"

namespace Mosaic {

// Source for Gelbooru 0.2 API sites (gelbooru.com, safebooru.org), all
// sharing the JSON dapi endpoint. `pid` is a 0-based post offset. Older
// installs answer with a bare JSON array of posts.
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

    // Optional dapi credentials (gelbooru.com and safebooru.org require them).
    // Set from the GUI thread while worker threads read them in addAuth().
    void setCredentials(const QString& userId, const QString& apiKey);

    bool hasCredentials() const;

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
    mutable QMutex m_credentialsMutex;  // guards m_userId / m_apiKey
    QString m_userId;
    QString m_apiKey;
};

}  // namespace Mosaic
