#include "LibrarySource.h"

#include "LibraryImporter.h"

namespace Mosaic {

// identity

QString LibrarySource::id() const { return QStringLiteral("library"); }

QString LibrarySource::displayName() const { return QStringLiteral("Library"); }

bool LibrarySource::isLocal() const { return true; }

// Source interface

SourceResult LibrarySource::search(const SourceQuery& query) { return m_catalog.search(query); }

QByteArray LibrarySource::fetch(const SourceImage& image, FileKind kind, QString* error) {
    return m_catalog.fetch(image, kind, error);
}

bool LibrarySource::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                   const std::function<void(qint64, qint64)>& progress,
                                   QString* error) {
    return m_catalog.downloadToFile(image, kind, path, progress, error);
}

SourceImage LibrarySource::parentOf(const SourceImage& image, QString* error) {
    return m_catalog.parentOf(image, error);
}

QList<SourceImage> LibrarySource::childrenOf(const SourceImage& image, QString* error) {
    return m_catalog.childrenOf(image, error);
}

// catalog facade

/**
 * Maps a ghost md5 (the deleted side of a resolved duplicate pair) onto the
 * image that actually still exists in the library; non-ghost md5s pass through.
 */
QString LibrarySource::resolveDuplicateSurvivor(const QString& md5) const {
    const QString key = md5.trimmed();
    if (key.isEmpty() || !m_duplicates.isMd5Resolved(key))
        return key;

    // Look up the other side of the resolved pair; if it is still in the
    // library, that is the image operations must land on.
    QString error;
    const QPair<QString, QString> pair = m_duplicates.retrieveDuplicate(key, &error);
    const QString other = pair.first == key ? pair.second : pair.first;
    if (!other.isEmpty() && m_catalog.hasImage(other))
        return other;

    return key;
}

ImportResult LibrarySource::importImage(const SourceImage& meta, const QByteArray& data) {
    const QString md5 = meta.md5.trimmed();

    // A ghost md5 (deleted side of a resolved duplicate pair) counts as
    // already saved: never re-download/re-store it or the dismissed
    // duplicate would be recreated. When its counterpart still lives in
    // the library, report success pointing at that surviving file so
    // callers transparently work with the kept copy.
    if (m_duplicates.isMd5Resolved(md5)) {
        ImportResult result;
        result.ok = true;
        result.alreadyExisted = true;

        const QString survivor = resolveDuplicateSurvivor(md5);
        if (survivor != md5) {
            SourceImage kept;
            if (m_catalog.lookupByMd5(survivor, &kept)) {
                result.filePath = LibraryCatalog::filePathFor(survivor, kept.format);
            }
        }
        return result;
    }

    // The same post already saved (possibly under another md5, e.g. the
    // site re-encoded the file): don't store a second copy. Tag-refresh
    // passes (empty data) still go through to update the existing row.
    if (!data.isEmpty()) {
        const QString saved = m_catalog.md5ForPost(meta.sourceId, meta.remoteId);
        if (!saved.isEmpty()) {
            ImportResult result;
            result.ok = true;
            result.alreadyExisted = true;
            SourceImage kept;
            if (m_catalog.lookupByMd5(saved, &kept))
                result.filePath = LibraryCatalog::filePathFor(saved, kept.format);
            return result;
        }
    }
    return m_catalog.importImage(meta, data);
}

bool LibrarySource::hasImage(const QString& md5) const {
    // A ghost md5 answers for its surviving counterpart.
    return m_catalog.hasImage(resolveDuplicateSurvivor(md5));
}

bool LibrarySource::hasImage(const SourceImage& image) const {
    return !savedMd5(image).isEmpty();
}

QString LibrarySource::savedMd5(const SourceImage& image) const {
    if (hasImage(image.md5))
        return image.md5;
    return m_catalog.md5ForPost(image.sourceId, image.remoteId);
}

/**
 * True when this md5 is the deleted side of a resolved duplicate pair.
 * Such images count as already saved/favorited/removed - callers can use
 * this to skip downloading them. The surviving copy returns false and is
 * handled like any normal image.
 */
bool LibrarySource::isDuplicateResolvedMd5(const QString& md5) const {
    return m_duplicates.isMd5Resolved(md5);
}

bool LibrarySource::removeImage(const QString& md5, QString* error) {
    // Deleting a ghost md5 deletes the surviving copy of the pair instead -
    // from the caller's point of view the ghost simply behaves like the
    // image that is actually in the library.
    return m_catalog.removeImage(resolveDuplicateSurvivor(md5), error);
}

bool LibrarySource::setFavorite(const QString& md5, bool favorite, QString* error) {
    // Favoriting/unfavoriting a ghost md5 lands on the surviving copy.
    return m_catalog.setFavorite(resolveDuplicateSurvivor(md5), favorite, error);
}

bool LibrarySource::isFavorite(const QString& md5) const {
    // The favorite state of a ghost md5 is the favorite state of the
    // surviving copy of the pair.
    return m_catalog.isFavorite(resolveDuplicateSurvivor(md5));
}

// tag facade

QStringList LibrarySource::allTagNames() const { return m_tags.allTagNames(); }

QHash<QString, QString> LibrarySource::allTagCategories() const {
    return m_tags.allTagCategories();
}

QString LibrarySource::addTag(const QString& md5, const QString& tagName, QString* error) {
    return m_tags.addTag(md5, tagName, error);
}

bool LibrarySource::removeTag(const QString& md5, const QString& tagName, QString* error) {
    return m_tags.removeTag(md5, tagName, error);
}

int LibrarySource::mergeTags(const QString& primary, const QStringList& secondaries,
                             QString* error) {
    return m_tags.mergeTags(primary, secondaries, error);
}

// duplicate facade

QList<DuplicatePair> LibrarySource::findDuplicates(const std::function<void(int, int)>& progress,
                                                   QString* error) {
    return m_duplicates.findDuplicates(progress, error);
}

bool LibrarySource::inParentChildChain(const QString& md5A, const QString& md5B) {
    return m_catalog.inParentChildChain(md5A, md5B);
}

bool LibrarySource::markDuplicateResolved(const QString& md5A, const QString& md5B,
                                          QString* error) {
    return m_duplicates.markResolved(md5A, md5B, error);
}

bool LibrarySource::isDuplicateResolved(const QString& md5A, const QString& md5B) const {
    return m_duplicates.isResolved(md5A, md5B);
}

bool LibrarySource::removeFromDuplicates(const QString& md5) const {
    return m_duplicates.removeFromResolved(md5);
}

QPair<QString, QString> LibrarySource::getDuplicate(const QString& md5, QString* error) const {
    return m_duplicates.retrieveDuplicate(md5, error);
}

// import facade

int LibrarySource::importLibraryFolder(const QString& folder, int* skipped,
                                       const std::function<void(int, int)>& progress,
                                       QString* error, bool markFavorite, const QString& userTag) {
    LibraryImporter importer;
    return importer.importFolder(folder, skipped, progress, error, markFavorite, userTag);
}

int LibrarySource::scanLibrary(const std::function<void(int, int)>& progress, QString* error) {
    LibraryImporter importer;
    return importer.scanLibrary(progress, error);
}

int LibrarySource::refreshUntagged(const std::function<void(int, int)>& progress, QString* error) {
    LibraryImporter importer;
    return importer.refreshUntagged(progress, error);
}

}  // namespace Mosaic
