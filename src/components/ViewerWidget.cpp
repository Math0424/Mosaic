#include "ViewerWidget.h"

#include <QPainter>

namespace Mosaic {

ViewerWidget::ViewerWidget(QWidget* parent) : QOpenGLWidget(parent) { setMinimumSize(320, 240); }

void ViewerWidget::showImage(const QImage& image) {
    if (image == m_image)
        return;
    m_image = image;
    update();
    emit imageChanged(m_image);
}

void ViewerWidget::clear() {
    // Free the decoded frame data explicitly - QImage's implicit sharing
    // would otherwise keep the last image's pixels alive in RAM.
    if (m_image.isNull())
        return;
    m_image = QImage();
    update();
    emit imageChanged(m_image);
}

void ViewerWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 28));

    if (m_image.isNull()) {
        painter.setPen(QColor(140, 140, 150));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("Select an image or video on the left"));
        return;
    }

    const QImage scaled = m_image.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPoint topLeft((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
    painter.drawImage(topLeft, scaled);
}

}  // namespace Mosaic
