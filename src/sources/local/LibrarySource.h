#pragma once

#include "DuplicateDetector.h"
#include "LibraryCatalog.h"
#include "LibraryTagService.h"
#include "../Source.h"

#include <functional>

namespace Mosaic {

// The downloaded local library exposed as a Source. Thin facade adapting
// the focused library services to the Source interface; new code should
// prefer the services directly.
class LibrarySource : public Source {
   public:
    QString id() const override;
    QString displayName() const override;
    bool isLocal() const override;

    // Source interface
    SourceResult search(const SourceQuery& query) override;
    QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) override;
    bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                        const std::function<void(qint64, qint64)>& progress = nullptr,
                        QString* error = nullptr) override;
    SourceImage parentOf(const SourceImage& image, QString* error = nullptr) override;
    QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr) override;

    // catalog facade

    ImportResult importImage(const SourceImage& meta, const QByteArray& data);

    bool hasImage(const QString& md5) const;

    // True when this md5 is the deleted side of an already-resolved
    // duplicate pair; such images are treated as saved/removed everywhere.
    bool isDuplicateResolvedMd5(const QString& md5) const;

    bool removeImage(const QString& md5, QString* error = nullptr);

    /** Marks an image as favorite (or not). */
    bool setFavorite(const QString& md5, bool favorite, QString* error = nullptr);

    /** True when the image carries the "usergroup:favorite" tag. */
    bool isFavorite(const QString& md5) const;

    // tag facade

    QStringList allTagNames() const;
    QHash<QString, QString> allTagCategories() const;
    QString addTag(const QString& md5, const QString& tagName, QString* error = nullptr);
    bool removeTag(const QString& md5, const QString& tagName, QString* error = nullptr);
    int mergeTags(const QString& primary, const QStringList& secondaries, QString* error = nullptr);

    // duplicate facade

    QList<DuplicatePair> findDuplicates(const std::function<void(int, int)>& progress = nullptr,
                                        QString* error = nullptr);
    bool inParentChildChain(const QString& md5A, const QString& md5B);
    bool markDuplicateResolved(const QString& md5A, const QString& md5B, QString* error = nullptr);
    bool isDuplicateResolved(const QString& md5A, const QString& md5B) const;
    bool removeFromDuplicates(const QString& md5) const;
    QPair<QString, QString> getDuplicate(const QString& md5, QString* error) const;

    // import facade

    int importLibraryFolder(const QString& folder, int* skipped = nullptr,
                            const std::function<void(int, int)>& progress = nullptr,
                            QString* error = nullptr, bool markFavorite = false,
                            const QString& userTag = QString());
    int scanLibrary(const std::function<void(int, int)>& progress = nullptr,
                    QString* error = nullptr);
    int refreshUntagged(const std::function<void(int, int)>& progress = nullptr,
                        QString* error = nullptr);

   private:
    // Maps a ghost md5 (deleted side of a resolved pair) onto the
    // surviving copy; non-ghost md5s pass through.
    QString resolveDuplicateSurvivor(const QString& md5) const;

    LibraryCatalog m_catalog;
    LibraryTagService m_tags;
    DuplicateDetector m_duplicates;
};

}  // namespace Mosaic
