#include "SourceManager.h"

namespace Mosaic {

bool SourceImage::isLocal() const {
    if (!localPath.isEmpty()) {
        return true;
    }

    LibrarySource library;
    return library.hasImage(md5);
}

QString SourceImage::tooltipHtml() const {
    // "Saved" is a best-effort heuristic; isLocal() would query the DB.
    const bool saved = !localPath.isEmpty();
    const QString savedText = saved ? QStringLiteral("yes") : QStringLiteral("no");
    const QString formatText = format.toHtmlEscaped();
    const QString sourceIdText = sourceId.toHtmlEscaped();
    const QString md5Text = md5.toHtmlEscaped();
    const int w = width;
    const int h = height;

    QString tagHtml;
    for (const SourceTag& tag : tags) {
        tagHtml += QStringLiteral("<span style=\"color:%1; white-space:nowrap;\">%2</span> ")
                       .arg(tag.categoryColorHtml())
                       .arg(tag.name.toHtmlEscaped());
    }

    const QString tagSection =
        tags.isEmpty() ? QString() : QStringLiteral("<br><br>Tags: ") + tagHtml;

    return QStringLiteral(
               "<div style=\"width:80ch; text-align:justify;\">"
               "Saved: %1<br>%2.%3<br>%4x%5<br>%6# %7<br>%8</div>")
        .arg(savedText)
        .arg(md5Text)
        .arg(formatText)
        .arg(w)
        .arg(h)
        .arg(sourceIdText)
        .arg(remoteId)
        .arg(tagSection);
}

SourceManager::SourceManager() {
    m_library = std::make_unique<LibrarySource>();
    m_e621 = std::make_unique<E621Source>();
    m_r34 = std::make_unique<R34Source>();
    m_gelbooru = std::make_unique<GelbooruSource>(
        QStringLiteral("gelbooru"), QStringLiteral("gelbooru.com"),
        QStringLiteral("https://gelbooru.com"), QStringLiteral("https://gelbooru.com"));
    m_rule34xxx = std::make_unique<GelbooruSource>(
        QStringLiteral("r34xxx"), QStringLiteral("rule34.xxx"),
        QStringLiteral("https://api.rule34.xxx"), QStringLiteral("https://rule34.xxx"));
    m_safebooru = std::make_unique<GelbooruSource>(
        QStringLiteral("safebooru"), QStringLiteral("safebooru.org"),
        QStringLiteral("https://safebooru.org"), QStringLiteral("https://safebooru.org"));

    // Display order: local library first, then remote sites. Merged
    // "All sources" tabs are created on demand via createMultiSource().
    m_sources = {m_library.get(), m_e621.get(),      m_gelbooru.get(),
                 m_r34.get(),     m_rule34xxx.get(), m_safebooru.get()};
}

QList<Source*> SourceManager::remoteSources() const {
    QList<Source*> result;
    for (Source* s : m_sources) {
        if (s->isLocal())
            continue;
        if (!s->isEnabled())
            continue;  // e.g. Gelbooru-family site without credentials
        result.append(s);
    }
    return result;
}

MultiSource* SourceManager::createMultiSource(const QList<Source*>& members) {
    if (members.isEmpty())
        return nullptr;
    m_multis.push_back(std::make_unique<MultiSource>(members));
    return m_multis.back().get();
}

void SourceManager::applyGelbooruCredentials(
    const QHash<QString, QPair<QString, QString>>& credentials) {
    for (Source* source :
         {static_cast<Source*>(m_gelbooru.get()), static_cast<Source*>(m_rule34xxx.get()),
          static_cast<Source*>(m_safebooru.get())}) {
        const auto it = credentials.find(source->id());
        if (it != credentials.end())
            static_cast<GelbooruSource*>(source)->setCredentials(it.value().first,
                                                                 it.value().second);
    }
}

Source* SourceManager::source(const QString& id) const {
    for (Source* s : m_sources) {
        if (s->id() == id)
            return s;
    }
    return nullptr;
}

}  // namespace Mosaic
