#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace Mosaic {

// Tag dictionary and tag-editing operations on the library catalog.
class LibraryTagService {
   public:
    QStringList allTagNames() const;

    QHash<QString, QString> allTagCategories() const;

    // Optional "category:name" prefix applies only when creating a NEW tag;
    // existing tags keep their stored category. Returns the category used.
    QString addTag(const QString& md5, const QString& tagName, QString* error = nullptr) const;

    bool removeTag(const QString& md5, const QString& tagName, QString* error = nullptr) const;

    // Re-tags all images carrying a secondary onto the primary and deletes
    // the secondaries. Returns the number of images re-tagged, -1 on error.
    int mergeTags(const QString& primary, const QStringList& secondaries,
                  QString* error = nullptr) const;
};

}  // namespace Mosaic
