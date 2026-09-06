#pragma once

#include <QOpenGLWidget>
#include <QImage>
#include <QString>

namespace Mosaic {

// Renders the current image or video frame (QOpenGLWidget / Qt RHI).
class ViewerWidget : public QOpenGLWidget {
    Q_OBJECT

   public:
    explicit ViewerWidget(QWidget* parent = nullptr);

    void showImage(const QImage& image);
    void clear();

    const QImage& image() const { return m_image; }

   signals:
    void imageChanged(const QImage& image);

   protected:
    void paintEvent(QPaintEvent* event) override;

   private:
    QImage m_image;
};

}  // namespace Mosaic
