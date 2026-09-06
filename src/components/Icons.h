#pragma once

#include <QIcon>
#include <QString>

namespace Mosaic {

// Loads an icon from the runtime assets/ folder.
inline QIcon assetIcon(const QString& name) { return QIcon(QStringLiteral("assets/") + name); }

}  // namespace Mosaic
