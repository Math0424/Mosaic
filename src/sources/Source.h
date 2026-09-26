#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include "../components/TagColors.h"

#include <functional>

namespace Mosaic {

// A tag attached to an image, with its category when the source has one.
struct SourceTag {
    QString name;
    QString category;

    QString categoryColorHtml() const { return categoryColor(category).name(QColor::HexRgb); }
};

enum class FileKind {
    Preview,
    Sample,
    Full,
};

// One image/post as returned by a source; URL fields may be empty.
struct SourceImage {
    QString md5;
    qint64 remoteId = 0;
    qint64 parentId = 0;
    QString sourceId;
    QString fileUrl;
    QString sampleUrl;
    QString previewUrl;
    int width = 0;
    int height = 0;
    qint64 fileSize = 0;
    QString format;
    QString rating;
    QString status;
    QString sourceUrl;
    qint64 createdAt = 0;  // unix seconds
    qint64 score = 0;
    bool hasChildren = false;
    bool favorite = false;
    QList<SourceTag> tags;

    // Set when the image already exists in the local library.
    QString localPath;
    bool isLocal() const;

    // Full copy of every attribute. Use this instead of hand-copying
    // fields when re-targeting an image (e.g. fetching through the local
    // library) - hand copies keep forgetting metadata.
    SourceImage withMd5(const QString& newMd5) const {
        SourceImage copy = *this;
        copy.md5 = newMd5;
        return copy;
    }

    // Best URL for the given kind (falls back to the full file URL).
    QString urlFor(FileKind kind) const {
        switch (kind) {
            case FileKind::Preview:
                return !previewUrl.isEmpty() ? previewUrl : fileUrl;
            case FileKind::Sample:
                return !sampleUrl.isEmpty() ? sampleUrl : fileUrl;
            case FileKind::Full:
                return fileUrl;
        }
        return fileUrl;
    }

    // HTML tooltip shared by thumbnails and conflict rows.
    QString tooltipHtml() const;
};

struct SourceQuery {
    QString tags;
    int page = 1;
    int limit = 75;
};

struct SourceResult {
    bool ok = false;
    QString error;
    QString warning;  // non-fatal notice
    QList<SourceImage> images;
    int page = 1;
    int pageLimit = 0;  // actual page size when the source ignores `limit`
};

// Abstract image source: a remote site or the local library. All methods
// are synchronous and must be thread-safe (called from worker threads).
class Source {
   public:
    virtual ~Source() = default;

    virtual QString id() const = 0;

    virtual QString displayName() const = 0;

    virtual bool isLocal() const { return false; }

    virtual bool isEnabled() const { return true; }

    // Members of a merged source; empty for regular sources.
    virtual QList<Source*> members() const { return {}; }

    virtual SourceResult search(const SourceQuery& query) = 0;

    virtual QByteArray fetch(const SourceImage& image, FileKind kind, QString* error = nullptr) = 0;

    // Streams a file variant to disk; used for videos so playback can
    // start mid-download. Default: fetch() + single write.
    virtual bool downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                const std::function<void(qint64, qint64)>& progress = nullptr,
                                QString* error = nullptr) {
        const QByteArray bytes = fetch(image, kind, error);
        if (bytes.isEmpty())
            return false;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Cannot write %1").arg(path);
            return false;
        }
        file.write(bytes);
        file.close();
        if (progress)
            progress(bytes.size(), bytes.size());
        return true;
    }

    // Resolves the parent/children of an image (chain navigation). Default:
    // unsupported.
    virtual SourceImage parentOf(const SourceImage& image, QString* error = nullptr) {
        Q_UNUSED(image);
        if (error)
            *error = QStringLiteral("This source has no parent info");
        return {};
    }

    virtual QList<SourceImage> childrenOf(const SourceImage& image, QString* error = nullptr) {
        Q_UNUSED(image);
        if (error)
            *error = QStringLiteral("This source has no child info");
        return {};
    }
};

}  // namespace Mosaic

Q_DECLARE_METATYPE(Mosaic::SourceImage)
Q_DECLARE_METATYPE(Mosaic::SourceQuery)
Q_DECLARE_METATYPE(Mosaic::SourceResult)
