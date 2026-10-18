#include "LibraryImporter.h"

#include "LibraryCatalog.h"
#include "LibraryTagService.h"
#include "LibraryUtils.h"
#include "../../core/AppState.h"
#include "../../core/Paths.h"
#include "../../core/Schema.h"
#include "../SourceManager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

#include <type_traits>

namespace Mosaic {

namespace {

/** Parses the grouped tags object of a legacy md5.json sidecar. */
QList<SourceTag> parseLegacyTags(const QJsonObject& root) {
    QList<SourceTag> tags;
    const QJsonObject tagGroups = root.value(QStringLiteral("tags")).toObject();
    for (auto it = tagGroups.begin(); it != tagGroups.end(); ++it) {
        const QString category = it.key().toLower();
        for (const auto& v : it.value().toArray()) {
            const QString name = Schema::normalizeTagName(v.toString());
            if (!name.isEmpty())
                tags.append(SourceTag{name, category});
        }
    }
    return tags;
}

/** Glob patterns of every media extension the library understands. */
QStringList mediaFilters() {
    QStringList filters;
    for (const char* ext :
         {"png", "jpg", "jpeg", "gif", "webp", "bmp", "webm", "mp4", "mov", "mkv", "avi"}) {
        filters << QStringLiteral("*.") + QLatin1String(ext);
    }
    return filters;
}

}  // namespace

int LibraryImporter::importFolder(const QString& folder, int* skipped,
                                  const std::function<void(int, int)>& progress, QString* error,
                                  bool markFavorite, const QString& userTag) {
    if (skipped)
        *skipped = 0;

    QDir dir(folder);
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("Folder does not exist: %1").arg(folder);
        return 0;
    }

    // Collect md5.ext media files (the json sidecars are matched by stem).
    const QFileInfoList files = dir.entryInfoList(mediaFilters(), QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        if (error)
            *error = QStringLiteral("No media files found in %1").arg(folder);
        return 0;
    }

    LibraryCatalog catalog;
    int imported = 0;
    const int total = files.size();
    int done = 0;

    for (const QFileInfo& fi : files) {
        if (progress)
            progress(++done, total);
        switch (importOneFile(fi, markFavorite, userTag, /*removeOriginal=*/false)) {
            case Outcome::Imported:
                ++imported;
                break;
            case Outcome::Skipped:
            case Outcome::Failed:
                if (skipped)
                    ++*skipped;
                break;
        }
    }

    if (progress)
        progress(total, total);
    return imported;
}

int LibraryImporter::scanLibrary(const std::function<void(int, int)>& progress, QString* error) {
    QDir dir(Paths::libraryDir());
    if (!dir.exists()) {
        if (error)
            *error = QStringLiteral("Library folder does not exist: %1").arg(dir.absolutePath());
        return 0;
    }

    LibraryCatalog catalog;

    // Everything that is not already cataloged: files whose name is not a
    // known md5 (strays dropped into the folder) get imported and renamed
    // to their content md5, keeping their original modification date.
    QFileInfoList pending;
    const QFileInfoList files = dir.entryInfoList(mediaFilters(), QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        const QString base = fi.completeBaseName();
        if (LibraryUtils::isValidMd5(base) && catalog.hasImage(base))
            continue;  // already in the catalog
        pending.append(fi);
    }

    int imported = 0;
    const int total = pending.size();
    int done = 0;
    for (const QFileInfo& fi : pending) {
        if (progress)
            progress(++done, total);
        if (importOneFile(fi, /*markFavorite=*/false, QString(), /*removeOriginal=*/true) ==
            Outcome::Imported) {
            ++imported;
        }
    }
    if (progress)
        progress(total, total);
    return imported;
}

int LibraryImporter::refreshUntagged(const std::function<void(int, int)>& progress,
                                     QString* error) {
    LibraryCatalog catalog;
    LibraryTagService tagService;

    // All cataloged images still carrying the notags marker.
    const QList<SourceImage> untagged =
        catalog.imagesWithTag(QStringLiteral("notags"), QStringLiteral("usergroup"));
    const int total = untagged.size();
    int refreshed = 0;
    int done = 0;

    SourceManager manager;
    manager.applyGelbooruCredentials(AppState::readSiteCredentials());
    const QList<Source*> remotes = manager.remoteSources();

    for (const SourceImage& img : untagged) {
        if (progress)
            progress(++done, total);

        // Skip entries whose md5 is missing or malformed - they cannot be
        // looked up remotely. Ghost md5s of resolved duplicate pairs are
        // naturally skipped here too, since their surviving counterpart is
        // the image that actually exists in the catalog.
        const QString md5 = img.md5;
        if (md5.isEmpty() || !LibraryUtils::isValidMd5(md5))
            continue;

        bool match = false;
        QList<SourceTag> tags;
        SourceImage meta;

        for (Source* s : remotes) {
            SourceQuery q;
            q.tags = QStringLiteral("md5:%1").arg(md5);
            q.page = 1;
            q.limit = 1;
            const SourceResult lookup = s->search(q);
            if (lookup.ok && !lookup.images.isEmpty()) {
                meta = lookup.images.first();
                tags = meta.tags;
                match = true;
                break;
            }
        }

        if (!match)
            continue;

        // Replace the notags marker with the resolved tags and metadata.
        meta.md5 = md5;
        meta.tags = tags;
        const ImportResult result = catalog.importImage(meta, QByteArray());
        if (!result.ok) {
            const QString msg = QStringLiteral("Tag refresh failed for %1: %2")
                                    .arg(md5)
                                    .arg(result.error);
            qWarning().noquote() << msg;
            LibraryUtils::logImportFailure(msg);
            continue;
        }
        tagService.removeTag(md5, QStringLiteral("notags"));
        ++refreshed;
    }

    if (progress)
        progress(total, total);
    return refreshed;
}

LibraryImporter::Outcome LibraryImporter::importOneFile(const QFileInfo& fi, bool markFavorite,
                                                        const QString& userTag,
                                                        bool removeOriginal) {
    const QString dirPath = fi.absolutePath();
    QFile mediaFile(fi.absoluteFilePath());
    if (!mediaFile.open(QIODevice::ReadOnly)) {
        const QString msg =
            QStringLiteral("Could not open media file: %1").arg(fi.absoluteFilePath());
        qWarning().noquote() << msg;
        LibraryUtils::logImportFailure(msg);
        return Outcome::Failed;
    }

    // 1. Load the image
    const QByteArray data = mediaFile.readAll();
    mediaFile.close();

    // 2. Check if its valid md5. if not get its md5.
    QString md5 = fi.completeBaseName();
    if (!LibraryUtils::isValidMd5(md5)) {
        md5 = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
    }

    LibraryCatalog catalog;
    if (catalog.hasImage(md5))
        return Outcome::Skipped;

    // Try to resolve post metadata and tags
    QList<SourceTag> tags;
    SourceImage meta;
    bool match = false;

    // 3. Search all remote sources by md5 (e621, r34, gelbooru family, ...)
    SourceManager manager;  // must outlive the pointers used below
    manager.applyGelbooruCredentials(AppState::readSiteCredentials());
    const QList<Source*> remotes = manager.remoteSources();
    for (Source* s : remotes) {
        SourceQuery q;
        q.tags = QStringLiteral("md5:%1").arg(md5);
        q.page = 1;
        q.limit = 1;
        const SourceResult lookup = s->search(q);
        if (lookup.ok && !lookup.images.isEmpty()) {
            meta = lookup.images.first();
            tags = meta.tags;
            match = true;
            break;
        }
    }

    // 4. If no tags are found, search for a .json sidecar (matched by the
    // original file stem, then by md5) and parse its tags and metadata.
    for (const QString& stem : {fi.completeBaseName(), md5}) {
        const QString jsonPath = QDir(dirPath).filePath(stem + QStringLiteral(".json"));
        if (match || !QFile::exists(jsonPath))
            continue;
        QFile jsonFile(jsonPath);
        if (!jsonFile.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);
        jsonFile.close();
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            continue;

        const QJsonObject root = doc.object();
        tags = parseLegacyTags(root);

        // created_at: ISO timestamp -> unix seconds
        const QJsonValue createdVal = root.value(QStringLiteral("created_at"));
        if (createdVal.isString()) {
            const QString s = createdVal.toString();
            QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
            if (dt.isValid())
                meta.createdAt = dt.toSecsSinceEpoch();
            else {
                dt = QDateTime::fromString(s, Qt::ISODate);
                if (dt.isValid())
                    meta.createdAt = dt.toSecsSinceEpoch();
            }
        }

        // relationships: parent_id, has_children, has_active_children
        const QJsonObject rel = root.value(QStringLiteral("relationships")).toObject();
        if (!rel.isEmpty()) {
            const QJsonValue pid = rel.value(QStringLiteral("parent_id"));
            if (!pid.isNull() && pid.isDouble())
                meta.parentId = static_cast<qint64>(pid.toDouble());
            const bool hasChildren = rel.value(QStringLiteral("has_children")).toBool(false);
            const bool hasActive = rel.value(QStringLiteral("has_active_children")).toBool(false);
            meta.hasChildren = hasChildren || hasActive;
        }

        match = true;
    }

    // 5. If no json file is found, search for a .tags file (semicolon
    // delimited set of tags).
    for (const QString& stem : {fi.completeBaseName(), md5}) {
        const QString tagsPath = QDir(dirPath).filePath(stem + QStringLiteral(".tags"));
        if (match || !QFile::exists(tagsPath))
            continue;
        QFile tagsFile(tagsPath);
        if (!tagsFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString content = QString::fromUtf8(tagsFile.readAll()).trimmed();
        tagsFile.close();
        if (content.isEmpty())
            continue;

        const QStringList tagList = content.split(';', Qt::SkipEmptyParts);
        for (const QString& rawTag : tagList) {
            const QString normalizedTag = Schema::normalizeTagName(rawTag);
            if (!normalizedTag.isEmpty())
                tags.append(SourceTag{normalizedTag, QStringLiteral("general")});
        }
        if (!tags.isEmpty())
            match = true;
    }

    // 6. No metadata anywhere: import best-effort, marked with the
    // usergroup "notags" tag so it stays searchable (query "notags"
    // to list them). The file's own modification time is kept as the
    // creation timestamp.
    if (!match) {
        tags.clear();
        tags.append(SourceTag{QStringLiteral("notags"), QStringLiteral("usergroup")});
        if (meta.createdAt <= 0)
            meta.createdAt = fi.lastModified().toSecsSinceEpoch();
        const QString msg =
            QStringLiteral("No match for %1 (md5: %2) - imported with usergroup:notags")
                .arg(fi.absoluteFilePath())
                .arg(md5);
        qInfo().noquote() << msg;
        LibraryUtils::logImportFailure(msg);
    }

    meta.md5 = md5;
    if (meta.format.isEmpty())
        meta.format = fi.suffix().toLower();
    if (meta.fileSize <= 0)
        meta.fileSize = data.size();
    meta.tags = tags;

    const ImportResult result = catalog.importImage(meta, data);
    if (!result.ok) {
        const QString msg =
            QStringLiteral("Import failed for %1: %2").arg(fi.absoluteFilePath()).arg(result.error);
        qWarning().noquote() << msg;
        LibraryUtils::logImportFailure(msg);
        return Outcome::Failed;
    }

    // Apply import-wide options.
    if (markFavorite)
        catalog.setFavorite(md5, true);
    if (!userTag.isEmpty()) {
        LibraryTagService tags;
        tags.addTag(md5, userTag);
    }

    // md5 rename: the catalog copy is md5-named; drop the stray original
    // once it differs from the stored file. The original's modification
    // date was carried over to the stored file by importImage().
    if (removeOriginal) {
        const QString stored = result.filePath;
        const QString original = QDir::toNativeSeparators(fi.absoluteFilePath());
        if (!stored.isEmpty() && QDir::toNativeSeparators(stored) != original) {
            if (!QFile::remove(fi.absoluteFilePath())) {
                const QString msg = QStringLiteral("Could not remove original after rename: %1")
                                        .arg(fi.absoluteFilePath());
                qWarning().noquote() << msg;
                LibraryUtils::logImportFailure(msg);
            }
        }
    }
    return Outcome::Imported;
}

}  // namespace Mosaic
