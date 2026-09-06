#pragma once

#include <QLayout>
#include <QList>
#include <QStyle>

namespace Mosaic {

// Wraps widgets left-to-right onto new lines (thumbnail grid).
// Based on the classic Qt "Flow Layout" example.
class FlowLayout : public QLayout {
    Q_OBJECT
   public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 8, int hSpacing = 8,
                        int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;

    void insertWidget(int index, QWidget* widget);

    int horizontalSpacing() const { return m_hSpace; }
    int verticalSpacing() const { return m_vSpace; }
    void setSpacing(int spacing) override {
        m_hSpace = spacing;
        m_vSpace = spacing;
    }
    void setItemSizeRange(int maxSize);
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QLayoutItem* takeAt(int index) override;

   private:
    int computeColumnCount(int availableWidth) const;
    int computeItemSize(int availableWidth, int columns) const;
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem*> m_items;
    int m_hSpace;
    int m_vSpace;
    int m_maxItemSize;
};

}  // namespace Mosaic
