#include "FlowLayout.h"

#include <QWidget>

namespace Mosaic {

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing), m_maxItemSize(220) {
    setContentsMargins(margin, margin, margin, margin);
}

void FlowLayout::setItemSizeRange(int maxSize) { m_maxItemSize = qMax(1, maxSize); }

int FlowLayout::computeColumnCount(int availableWidth) const {
    if (availableWidth <= 0)
        return 1;

    const int desired = m_maxItemSize;
    const int minSize = qMax(1, desired * 90 / 100);

    // Start with the columns that fit at the desired size.
    int columns = qMax(1, (availableWidth + m_hSpace) / (desired + m_hSpace));

    // Prefer another column if items would still be >= 90% of desired size.
    while (true) {
        const int nextColumns = columns + 1;

        const int availableForItems = availableWidth - (nextColumns - 1) * m_hSpace;

        if (availableForItems <= 0)
            break;

        const int nextItemSize = availableForItems / nextColumns;

        if (nextItemSize >= minSize)
            columns = nextColumns;
        else
            break;
    }

    return columns;
}

int FlowLayout::computeItemSize(int availableWidth, int columns) const {
    if (availableWidth <= 0 || columns <= 0)
        return 1;

    const int totalSpacing = (columns - 1) * m_hSpace;

    const int availableForItems = availableWidth - totalSpacing;

    return qMax(1, availableForItems / columns);
}

FlowLayout::~FlowLayout() {
    QLayoutItem* item = nullptr;

    while ((item = takeAt(0)) != nullptr) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { m_items.append(item); }

void FlowLayout::insertWidget(int index, QWidget* widget) {
    addChildWidget(widget);
    const int at = qBound(0, index, m_items.size());
    m_items.insert(at, new QWidgetItem(widget));
    invalidate();
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const { return doLayout(QRect(0, 0, width, 0), true); }

int FlowLayout::count() const { return m_items.size(); }

QLayoutItem* FlowLayout::itemAt(int index) const { return m_items.value(index); }

QSize FlowLayout::minimumSize() const {
    QSize size;

    for (const QLayoutItem* item : m_items) size = size.expandedTo(item->minimumSize());

    const QMargins margins = contentsMargins();

    size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());

    return size;
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);

    return nullptr;
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(left, top, -right, -bottom);
    if (effective.width() <= 0)
        return top + bottom;

    const int columns = computeColumnCount(effective.width());
    const int itemSize = computeItemSize(effective.width(), columns);
    int y = effective.y();
    int lineHeight = itemSize;
    QList<QLayoutItem*> rowItems;
    rowItems.reserve(columns);

    auto flushRow = [&](bool completeRow) {
        if (rowItems.isEmpty())
            return;

        const int count = rowItems.size();
        int baseSize = itemSize;
        int extraPixels = 0;

        if (completeRow) {
            const int totalSpacing = (count - 1) * m_hSpace;
            const int availableForItems = effective.width() - totalSpacing;
            baseSize = availableForItems / count;
            extraPixels = availableForItems % count;
        }

        int x = effective.x();
        int maxRowHeight = 0;
        for (int i = 0; i < count; ++i) {
            const int size = baseSize + (completeRow && i < extraPixels ? 1 : 0);
            QLayoutItem* item = rowItems.at(i);
            if (!testOnly) {
                item->setGeometry(QRect(x, y, size, size));
            }

            x += size + m_hSpace;
            maxRowHeight = qMax(maxRowHeight, size);
        }

        lineHeight = maxRowHeight;
        rowItems.clear();
    };

    int currentColumns = 0;
    for (QLayoutItem* item : m_items) {
        if (currentColumns >= columns) {
            flushRow(true);
            y += lineHeight + m_vSpace;
            currentColumns = 0;
        }

        rowItems.append(item);
        ++currentColumns;
    }

    flushRow(currentColumns == columns);
    return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    const QObject* p = parent();

    if (p == nullptr)
        return -1;

    if (p->isWidgetType()) {
        const auto* widget = static_cast<const QWidget*>(p);

        return widget->style()->pixelMetric(pm, nullptr, widget);
    }

    return static_cast<const QLayout*>(p)->spacing();
}

}  // namespace Mosaic