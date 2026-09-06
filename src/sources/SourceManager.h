#pragma once

#include <QHash>
#include <QPair>
#include <QString>

#include <memory>
#include <vector>

#include "web/E621Source.h"
#include "web/GelbooruSource.h"
#include "local/LibrarySource.h"
#include "MultiSource.h"
#include "web/R34Source.h"
#include "Source.h"

namespace Mosaic {

// Registry of all available image sources. Library first; remote sites
// register here. Resolves tab sourceIds and supplies the new-tab menu.
class SourceManager {
   public:
    SourceManager();

    const QList<Source*>& sources() const { return m_sources; }

    // Usable remote sources; disabled ones (missing credentials) omitted.
    QList<Source*> remoteSources() const;

    // Creates a merged source over the members; manager-owned.
    MultiSource* createMultiSource(const QList<Source*>& members);

    Source* source(const QString& id) const;

    LibrarySource* library() const { return m_library.get(); }

    void applyGelbooruCredentials(const QHash<QString, QPair<QString, QString>>& credentials);

   private:
    std::unique_ptr<LibrarySource> m_library;
    std::unique_ptr<E621Source> m_e621;
    std::unique_ptr<R34Source> m_r34;
    std::unique_ptr<GelbooruSource> m_gelbooru;
    std::unique_ptr<GelbooruSource> m_rule34xxx;
    std::unique_ptr<GelbooruSource> m_safebooru;
    std::vector<std::unique_ptr<MultiSource>> m_multis;
    QList<Source*> m_sources;  // non-owning, stable pointers
};

}  // namespace Mosaic
