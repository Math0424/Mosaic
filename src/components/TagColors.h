#pragma once

#include <QColor>
#include <QString>

namespace Mosaic {

// Color per tag category (shared by tag lists and the search bar).
inline QColor categoryColor(const QString& category) {
    if (category == QLatin1String("artist"))
        return QColor(120, 170, 255);
    if (category == QLatin1String("character"))
        return QColor(150, 220, 150);
    if (category == QLatin1String("species"))
        return QColor(240, 190, 120);
    if (category == QLatin1String("meta"))
        return QColor(200, 160, 220);
    if (category == QLatin1String("usergroup"))
        return QColor(255, 200, 100);
    if (category == QLatin1String("model"))
        return QColor(120, 210, 210);
    return QColor(170, 170, 180);  // general / unknown
}

// "#rrggbb" form of categoryColor(), for rich-text tooltips.
inline QString categoryColorHtml(const QString& category) {
    return categoryColor(category).name(QColor::HexRgb);
}

}  // namespace Mosaic
