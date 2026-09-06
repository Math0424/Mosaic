#pragma once

#include "Source.h"

namespace Mosaic {

// Pseudo-source running the same query on member sources in parallel,
// merged on md5. Per-image operations route back via sourceId.
class MultiSource : public Source {
   public:
    explicit MultiSource(const QList<Source*>& members);

    QString id() const override;
    QString displayName() const override;

    QList<Source*> members() const override { return m_members; }

    SourceResult search(const SourceQuery& query) override;
    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;
    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;
    SourceImage parentOf(const SourceImage& image, QString* error = nullptr) override;
    QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr) override;

   private:
    Source* sourceFor(const QString& sourceId) const;

    QList<Source*> m_members;  // non-owning
};

}  // namespace Mosaic
