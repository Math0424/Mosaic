#include "MultiSource.h"

#include <QDebug>
#include <QFuture>
#include <QSet>
#include <QtConcurrentRun>

namespace Mosaic {

MultiSource::MultiSource(const QList<Source*>& members) : m_members(members) {}

QString MultiSource::id() const { return QStringLiteral("multi"); }

QString MultiSource::displayName() const { return QStringLiteral("All sources"); }

SourceResult MultiSource::search(const SourceQuery& query) {
    SourceResult result;
    result.page = query.page;

    // Query every member in parallel.
    QList<QFuture<SourceResult>> futures;
    futures.reserve(m_members.size());
    for (Source* member : m_members) {
        futures.append(QtConcurrent::run([member, query]() { return member->search(query); }));
    }

    QSet<QString> seen;
    QStringList errors;
    QStringList failed;
    for (int i = 0; i < futures.size(); ++i) {
        futures[i].waitForFinished();
        const SourceResult part = futures[i].result();
        if (!part.ok) {
            failed << m_members[i]->displayName();
            errors << QStringLiteral("%1: %2").arg(m_members[i]->displayName(), part.error);
            qWarning().noquote() << "MultiSource:" << m_members[i]->displayName()
                                 << "search failed:" << part.error;
            continue;
        }
        for (const SourceImage& img : part.images) {
            if (!img.md5.isEmpty()) {
                if (seen.contains(img.md5))
                    continue;
                seen.insert(img.md5);
            }
            result.images.append(img);
        }
    }

    if (result.images.isEmpty() && !m_members.isEmpty() && failed.size() == m_members.size()) {
        result.error = errors.join(QStringLiteral(" | "));
        return result;
    }
    if (!failed.isEmpty()) {
        result.warning =
            QStringLiteral("no results from %1").arg(failed.join(QStringLiteral(", ")));
    }

    // Any non-empty merged page counts as "full".
    if (!result.images.isEmpty())
        result.pageLimit = result.images.size();

    result.ok = true;
    return result;
}

Source* MultiSource::sourceFor(const QString& sourceId) const {
    for (Source* s : m_members) {
        if (s->id() == sourceId)
            return s;
    }
    qWarning().noquote() << "MultiSource: unknown source id" << sourceId;
    return nullptr;
}

QByteArray MultiSource::fetch(const SourceImage& image, FileKind kind, QString* error) {
    Source* source = sourceFor(image.sourceId);
    if (!source) {
        if (error)
            *error = QStringLiteral("Unknown source \"%1\" for this image").arg(image.sourceId);
        return {};
    }
    return source->fetch(image, kind, error);
}

bool MultiSource::downloadToFile(const SourceImage& image, FileKind kind, const QString& path,
                                 const std::function<void(qint64, qint64)>& progress,
                                 QString* error) {
    Source* source = sourceFor(image.sourceId);
    if (!source) {
        if (error)
            *error = QStringLiteral("Unknown source \"%1\" for this image").arg(image.sourceId);
        return false;
    }
    return source->downloadToFile(image, kind, path, progress, error);
}

SourceImage MultiSource::parentOf(const SourceImage& image, QString* error) {
    Source* source = sourceFor(image.sourceId);
    if (!source) {
        if (error)
            *error = QStringLiteral("Unknown source \"%1\" for this image").arg(image.sourceId);
        return {};
    }
    return source->parentOf(image, error);
}

QList<SourceImage> MultiSource::childrenOf(const SourceImage& image, QString* error) {
    Source* source = sourceFor(image.sourceId);
    if (!source) {
        if (error)
            *error = QStringLiteral("Unknown source \"%1\" for this image").arg(image.sourceId);
        return {};
    }
    return source->childrenOf(image, error);
}

}  // namespace Mosaic
