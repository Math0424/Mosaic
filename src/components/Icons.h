#pragma once

#include <QHash>
#include <QIcon>
#include <QString>

namespace Mosaic {

// Loads an icon embedded from assets/ (assets.qrc, prefix /icons). Cached:
// thumbnails ask for their badge icons on every repaint. GUI thread only.
inline QIcon assetIcon(const QString& name) {
    static QHash<QString, QIcon> cache;
    auto it = cache.constFind(name);
    if (it == cache.constEnd())
        it = cache.insert(name, QIcon(QStringLiteral(":/icons/") + name));
    return *it;
}

}  // namespace Mosaic
