#include "ViewerWindow.h"

#include "Icons.h"
#include "TagSidebar.h"
#include "ViewerWidget.h"
#include "../core/AppState.h"
#include "../core/Paths.h"
#include "../core/Schema.h"
#include "../media/ImageFingerprint.h"
#include "../media/VideoDecoder.h"
#include "../sources/local/LibrarySource.h"

#include <QtMath>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMovie>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QDesktopServices>
#include <QSlider>
#include <QStyle>
#include <QUrl>
#include <QStackedWidget>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstring>
#include <limits>

namespace Mosaic {

namespace {

constexpr int kSlideshowManualTime = 10 * 1000;
constexpr int kMaxCacheEntries = 8;
constexpr int kCacheRadius = 4;

QString cacheKey(const QString& md5, FileKind kind) {
    return md5 + QLatin1Char(':') + QString::number(static_cast<int>(kind));
}

QString formatBytes(qint64 bytes) {
    if (bytes <= 0)
        return QStringLiteral("?");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

QString formatTime(qint64 ms) {
    if (ms < 0)
        ms = 0;
    const qint64 totalSec = ms / 1000;
    return QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

// magnifier loupe

// Circular magnifier overlaid on the displayed image.
class LoupeWidget : public QWidget {
   public:
    explicit LoupeWidget(QWidget* parent) : QWidget(parent) {
        setFixedSize(m_diameter, m_diameter);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

    void setImage(const QImage& image) {
        m_image = image;
        update();
    }

    int diameter() const { return m_diameter; }

    void resizeLens(int diameter) {
        diameter = qBound(kMinDiameter, diameter, kMaxDiameter);
        if (diameter == m_diameter)
            return;
        m_diameter = diameter;
        setFixedSize(diameter, diameter);
        update();
    }

    void setMagnifiedPoint(QPoint point, double zoom) {
        if (m_point != point || !qFuzzyCompare(m_zoom, zoom)) {
            m_point = point;
            m_zoom = zoom;
            update();
        }
    }

    double zoom() const { return m_zoom; }

   protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        const QRectF lens = rect().adjusted(1, 1, -1, -1);

        // Backdrop so the lens reads over any image.
        painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
        painter.setBrush(QColor(24, 24, 28, 235));
        painter.drawEllipse(lens);

        if (m_image.isNull())
            return;

        // Draw scaled so the magnified point is at the lens center.
        const double zoom = qMax(m_zoom, 0.0001);
        painter.save();
        QPainterPath clip;
        clip.addEllipse(lens);
        painter.setClipPath(clip);
        painter.translate(lens.center());
        painter.scale(zoom, zoom);
        painter.drawImage(-m_point, m_image);
        painter.restore();
    }

   private:
    static constexpr int kMinDiameter = 200;
    static constexpr int kMaxDiameter = 640;
    int m_diameter = 250;
    QImage m_image;
    QPoint m_point;
    double m_zoom = 2.0;
};

// construction

ViewerWindow::ViewerWindow(Source* source, const QList<SourceImage>& images, int index,
                           QWidget* parent)
    : QWidget(parent, Qt::Window), m_source(source), m_images(images) {
    m_index = qBound(0, index, qMax(0, static_cast<int>(m_images.size()) - 1));
    buildUi();

    // Mouse moves over children aren't forwarded to this window; watch at
    // application level to restore a hidden cursor only on real movement.
    qApp->installEventFilter(this);
    const QList<QWidget*> children = findChildren<QWidget*>();
    for (QWidget* child : children) child->setMouseTracking(true);

    // Restore last geometry and loupe settings.
    AppState state;
    if (state.load()) {
        const QRect geo = state.viewerGeometry();
        if (geo.width() > 0 && geo.height() > 0)
            setGeometry(geo);
        m_loupeZoom = state.viewerLoupeZoom();
        m_loupeDiameter = state.viewerLoupeDiameter();
    }

    if (!m_images.isEmpty())
        showAt(m_index);
}

ViewerWindow::~ViewerWindow() { stopMedia(); }

void ViewerWindow::setImages(Source* source, const QList<SourceImage>& images, int index) {
    m_source = source;
    m_images = images;
    m_index = qBound(0, index, qMax(0, static_cast<int>(m_images.size()) - 1));
    if (!m_images.isEmpty())
        showAt(m_index);
}

void ViewerWindow::closeEvent(QCloseEvent* event) {
    AppState state;
    if (state.load()) {
        state.setViewerGeometry(isMaximized() || isFullScreen() ? normalGeometry() : geometry());
        state.setViewerLoupeZoom(m_loupeZoom);
        state.setViewerLoupeDiameter(m_loupeDiameter);
        state.save();
    }
    QWidget::closeEvent(event);
}

void ViewerWindow::buildUi() {
    setWindowTitle(QStringLiteral("Mosaic Viewer"));
    resize(1280, 800);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tagSidebar = new TagSidebar(this);

    // --- center column ---
    auto* center = new QWidget(this);
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(6, 6, 6, 6);
    centerLayout->setSpacing(4);

    m_mediaStack = new QStackedWidget(center);
    m_viewer = new ViewerWidget(m_mediaStack);

    // Keep an open loupe in sync when the displayed frame changes.
    connect(m_viewer, &ViewerWidget::imageChanged, this, [this](const QImage&) {
        if (m_loupe && m_loupe->isVisible()) {
            m_loupe->setImage(m_viewer->image());
            if (m_lastLoupePos != QPoint())
                updateLoupe(m_lastLoupePos);
        }
    });

    m_mediaStack->addWidget(m_viewer);
    centerLayout->addWidget(m_mediaStack, 1);

    m_videoControls = new QWidget(center);
    auto* videoLayout = new QHBoxLayout(m_videoControls);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(6);
    m_playButton = new QToolButton(m_videoControls);
    m_playButton->setIcon(assetIcon(QStringLiteral("pause.svg")));
    m_playButton->setToolTip(QStringLiteral("Play/pause (Space)"));
    m_progress = new QSlider(Qt::Horizontal, m_videoControls);
    m_progress->setRange(0, 0);
    m_timeLabel = new QLabel(m_videoControls);
    auto* volumeIcon = new QLabel(m_videoControls);
    volumeIcon->setPixmap(assetIcon(QStringLiteral("volume.svg")).pixmap(16, 16));
    m_volumeSlider = new QSlider(Qt::Horizontal, m_videoControls);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setFixedWidth(96);
    m_volumeSlider->setToolTip(QStringLiteral("Volume"));
    videoLayout->addWidget(m_playButton);
    videoLayout->addWidget(m_progress, 1);
    videoLayout->addWidget(m_timeLabel);
    videoLayout->addWidget(volumeIcon);
    videoLayout->addWidget(m_volumeSlider);
    m_videoControls->hide();
    centerLayout->addWidget(m_videoControls);

    // Info row
    auto* infoRow = new QHBoxLayout();
    m_infoLabel = new QLabel(center);
    m_infoLabel->setTextFormat(Qt::RichText);
    m_infoLabel->setOpenExternalLinks(true);
    m_statusLabel = new QLabel(center);
    infoRow->addWidget(m_infoLabel, 1);
    infoRow->addWidget(m_statusLabel);
    centerLayout->addLayout(infoRow);

    // Bottom toolbar
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(4);
    const QSize iconSize(16, 16);
    m_leftButton = new QToolButton(center);
    m_leftButton->setIcon(assetIcon(QStringLiteral("prev.svg")));
    m_leftButton->setIconSize(iconSize);
    m_leftButton->setToolTip(QStringLiteral("Previous (Left)"));
    m_rightButton = new QToolButton(center);
    m_rightButton->setIcon(assetIcon(QStringLiteral("next.svg")));
    m_rightButton->setIconSize(iconSize);
    m_rightButton->setToolTip(QStringLiteral("Next (Right)"));
    m_upButton = new QToolButton(center);
    m_upButton->setIcon(assetIcon(QStringLiteral("up.svg")));
    m_upButton->setToolTip(QStringLiteral("Parent (Up)"));
    m_upButton->setIconSize(iconSize);
    m_downButton = new QToolButton(center);
    m_downButton->setIcon(assetIcon(QStringLiteral("down.svg")));
    m_downButton->setIconSize(iconSize);
    m_downButton->setToolTip(QStringLiteral("Child (Down)"));
    toolbar->addWidget(m_leftButton);
    toolbar->addWidget(m_rightButton);
    toolbar->addWidget(m_upButton);
    toolbar->addWidget(m_downButton);
    toolbar->addStretch(1);
    m_slideshowButton = new QToolButton(center);
    m_slideshowButton->setIcon(assetIcon(QStringLiteral("images.svg")));
    m_slideshowButton->setCheckable(true);
    m_slideshowButton->setToolTip(
        QStringLiteral("Slideshow (F5) - dwell time scales with image complexity"));
    m_repeatButton = new QToolButton(center);
    m_repeatButton->setIcon(assetIcon(QStringLiteral("repeat.svg")));
    m_repeatButton->setCheckable(true);
    m_repeatButton->setChecked(true);
    m_repeatButton->setToolTip(QStringLiteral("Repeat video/gif at the end"));
    m_saveButton = new QPushButton(QStringLiteral("Save"), center);
    m_saveButton->setToolTip(QStringLiteral("Save to library (Ctrl+S)"));
    m_saveFavButton = new QPushButton(QStringLiteral("Save"), center);
    m_saveFavButton->setIcon(assetIcon(QStringLiteral("save-favorite.svg")));
    m_saveFavButton->setToolTip(QStringLiteral("Save to library and mark favorite (Ctrl+Shift+S)"));
    m_deleteButton = new QPushButton(QStringLiteral("Delete"), center);
    m_deleteButton->setToolTip(QStringLiteral("Remove from library"));
    m_revealButton = new QToolButton(center);
    m_revealButton->setIcon(assetIcon(QStringLiteral("file.svg")));
    m_revealButton->setToolTip(QStringLiteral("Reveal in File Explorer"));
    connect(m_revealButton, &QToolButton::clicked, this, [this]() {
        if (m_current.localPath.isEmpty())
            return;
#ifdef Q_OS_WIN
        QProcess::startDetached(
            QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,"), QDir::toNativeSeparators(m_current.localPath)});
#else
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(m_current.localPath).absolutePath()));
#endif
    });
    m_favoriteButton = new QToolButton(center);
    m_favoriteButton->setIcon(assetIcon(QStringLiteral("fav.svg")));
    m_favoriteButton->setIconSize(iconSize);
    m_favoriteButton->setCheckable(true);
    m_favoriteButton->setToolTip(QStringLiteral("Toggle favorite"));
    toolbar->addWidget(m_slideshowButton);
    toolbar->addWidget(m_repeatButton);
    toolbar->addWidget(m_saveButton);
    toolbar->addWidget(m_saveFavButton);
    toolbar->addWidget(m_deleteButton);
    toolbar->addWidget(m_revealButton);
    toolbar->addWidget(m_favoriteButton);
    centerLayout->addLayout(toolbar);

    root->addWidget(m_tagSidebar);
    root->addWidget(center, 1);

    // --- playback + timers ---
    m_playback = new MediaPlaybackController(this);
    connect(m_playback, &MediaPlaybackController::frameReady, this, &ViewerWindow::onVideoFrame);
    m_slideshowTimer = new QTimer(this);
    m_slideshowTimer->setSingleShot(true);

    // --- wiring ---
    connect(m_slideshowTimer, &QTimer::timeout, this, &ViewerWindow::onSlideshowTick);

    connect(m_tagSidebar, &TagSidebar::addTagRequested, this, &ViewerWindow::onAddTag);
    connect(m_tagSidebar, &TagSidebar::removeTagRequested, this, &ViewerWindow::onRemoveTag);
    connect(m_tagSidebar, &TagSidebar::tagSearchRequested, this, &ViewerWindow::tagSearchRequested);

    connect(m_leftButton, &QToolButton::clicked, this, [this]() { showAt(m_index - 1); });
    connect(m_rightButton, &QToolButton::clicked, this, [this]() { showAt(m_index + 1); });
    connect(m_upButton, &QToolButton::clicked, this, [this]() { navigateChain(true); });
    connect(m_downButton, &QToolButton::clicked, this, [this]() { navigateChain(false); });

    connect(m_saveButton, &QPushButton::clicked, this, &ViewerWindow::onSaveClicked);
    connect(m_saveFavButton, &QPushButton::clicked, this, [this]() { saveToLibrary(true); });
    connect(m_deleteButton, &QPushButton::clicked, this, &ViewerWindow::onDeleteClicked);
    connect(m_favoriteButton, &QToolButton::clicked, this, &ViewerWindow::onFavoriteClicked);
    connect(m_slideshowButton, &QToolButton::clicked, this, &ViewerWindow::toggleSlideshow);

    connect(m_playButton, &QToolButton::clicked, this, &ViewerWindow::onPlayPauseClicked);
    connect(m_progress, &QSlider::valueChanged, this, &ViewerWindow::onProgressMoved);
    connect(m_progress, &QSlider::sliderPressed, this, [this]() { m_scrubbing = true; });
    connect(m_progress, &QSlider::sliderReleased, this, [this]() { m_scrubbing = false; });
    // Click-to-seek on the track.
    m_progress->installEventFilter(this);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &ViewerWindow::onVolumeChanged);

    m_viewer->installEventFilter(this);
}

// navigation

void ViewerWindow::showAt(int index) {
    if (index < 0 || index >= m_images.size())
        return;

    if (m_slideshow) {
        m_slideshowTimer->stop();
        m_slideshowTimer->start(kSlideshowManualTime);
    }

    m_index = index;
    m_chainStack.clear();
    m_current = m_images.at(index);
    m_animatedMedia = false;
    if (m_loupe)
        hideLoupe();

    // Title: md5.ext (X/XX)
    QString ext = m_current.format.toLower();
    if (ext.isEmpty() && !m_current.localPath.isEmpty())
        ext = QFileInfo(m_current.localPath).suffix().toLower();
    const QString stem = m_current.md5.isEmpty() ? QStringLiteral("post") : m_current.md5;
    setWindowTitle(QStringLiteral("%1.%2 (%3/%4) - Mosaic Viewer")
                       .arg(stem, ext.isEmpty() ? QStringLiteral("img") : ext)
                       .arg(m_index + 1)
                       .arg(m_images.size()));

    setInfo(m_current);
    updateSidebars();
    checkLibraryStatus();
    loadMedia(m_current);
    preloadAround(index);
    evictDistantCache();
}

void ViewerWindow::checkLibraryStatus() {
    if (m_current.md5.isEmpty() || currentIsLocal())
        return;

    const QString md5 = m_current.md5;
    QPointer<ViewerWindow> self(this);
    QThreadPool::globalInstance()->start([self, md5]() {
        LibrarySource library;
        const bool inLibrary = library.hasImage(md5);
        const bool favorite = inLibrary ? library.isFavorite(md5) : false;
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, inLibrary, favorite]() {
                if (!self || self->m_current.md5 != md5)
                    return;
                if (!inLibrary)
                    return;
                self->m_statusLabel->setText(QStringLiteral("Already in library ✓"));
                self->m_current.localPath = QStringLiteral("library:") + md5;
                self->m_current.favorite = favorite;
                for (SourceImage& img : self->m_images) {
                    if (img.md5 == md5) {
                        if (img.localPath.isEmpty())
                            img.localPath = QStringLiteral("library:") + md5;
                        img.favorite = favorite;
                    }
                }
                self->updateSidebars();
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::navigateChain(bool up) {
    auto arrive = [this]() {
        setInfo(m_current);
        updateSidebars();
        loadMedia(m_current);
    };

    if (up) {
        // Retracing a descent beats climbing further.
        if (!m_chainStack.isEmpty() && chainTopIsParent()) {
            m_current = m_chainStack.takeLast();
            arrive();
            return;
        }
        if (m_current.parentId == 0) {
            m_statusLabel->setText(QStringLiteral("No parent"));
            return;
        }
        m_statusLabel->setText(QStringLiteral("Loading parent…"));
        QPointer<ViewerWindow> self(this);
        Source* source = m_source;
        const SourceImage from = m_current;
        QThreadPool::globalInstance()->start([self, source, from]() {
            QString error;
            const SourceImage parent = source->parentOf(from, &error);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, parent, error]() {
                    if (!self)
                        return;
                    if (parent.md5.isEmpty() && parent.remoteId == 0) {
                        self->m_statusLabel->setText(
                            error.isEmpty() ? QStringLiteral("Parent not found") : error);
                        return;
                    }
                    self->m_chainStack.append(self->m_current);
                    self->m_current = parent;
                    self->setInfo(self->m_current);
                    self->updateSidebars();
                    self->loadMedia(self->m_current);
                },
                Qt::QueuedConnection);
        });
        return;
    }

    // Down: retrace a climb before descending.
    if (!m_chainStack.isEmpty() && chainTopIsChild()) {
        m_current = m_chainStack.takeLast();
        arrive();
        return;
    }
    m_statusLabel->setText(QStringLiteral("Loading children…"));
    QPointer<ViewerWindow> self(this);
    Source* source = m_source;
    const SourceImage from = m_current;
    QThreadPool::globalInstance()->start([self, source, from]() {
        QString error;
        const QList<SourceImage> children = source->childrenOf(from, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, children, error]() {
                if (!self)
                    return;
                if (children.isEmpty()) {
                    self->m_statusLabel->setText(error.isEmpty() ? QStringLiteral("No children")
                                                                 : error);
                    return;
                }
                self->m_chainStack.append(self->m_current);
                self->m_current = children.first();
                self->setInfo(self->m_current);
                self->updateSidebars();
                self->loadMedia(self->m_current);
            },
            Qt::QueuedConnection);
    });
}

bool ViewerWindow::chainTopIsParent() const {
    if (m_chainStack.isEmpty())
        return false;
    const SourceImage& top = m_chainStack.last();
    return (m_current.parentId != 0 && m_current.parentId == top.remoteId);
}

bool ViewerWindow::chainTopIsChild() const {
    if (m_chainStack.isEmpty())
        return false;
    const SourceImage& top = m_chainStack.last();
    return (top.parentId != 0 && top.parentId == m_current.remoteId);
}

// media loading

void ViewerWindow::loadMedia(const SourceImage& image) {
    stopMedia();
    stopStreamProbe();
    ++m_loadGeneration;
    m_loading = true;
    m_statusLabel->setText(QStringLiteral("Loading…"));

    // 0) Already in the preload cache.
    const QString fullKey = cacheKey(image.md5, FileKind::Full);
    const QByteArray cached = m_cache.value(fullKey);
    if (!cached.isEmpty()) {
        deliverMedia(cached);
        return;
    }

    // Videos stream straight to disk and start playing as soon as enough
    // data has arrived.
    if (mediaKindOf(image) == MediaKind::Video) {
        loadVideoProgressively(image);
        return;
    }

    // 1) Thumbnail first so there's never a blank pane.
    if (!image.isLocal() && !image.md5.isEmpty()) {
        const QByteArray thumb = m_cache.value(cacheKey(image.md5, FileKind::Preview));
        QImage placeholder;
        if (!thumb.isEmpty() && placeholder.loadFromData(thumb)) {
            m_mediaStack->setCurrentWidget(m_viewer);
            m_viewer->showImage(placeholder);
        } else if (!image.previewUrl.isEmpty()) {
            QPointer<ViewerWindow> self(this);
            Source* source = m_source;
            const QString md5 = image.md5;
            QThreadPool::globalInstance()->start([self, source, image, md5]() {
                const QByteArray bytes = source->fetch(image, FileKind::Preview);
                if (!self || bytes.isEmpty())
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, md5, bytes]() {
                        if (!self || !self->m_loading || self->m_current.md5 != md5)
                            return;  // user moved on / full media already arrived
                        QImage img;
                        if (!img.loadFromData(bytes))
                            return;
                        self->m_mediaStack->setCurrentWidget(self->m_viewer);
                        self->m_viewer->showImage(img);
                    },
                    Qt::QueuedConnection);
            });
        }
    }

    // 2) The real media - library first, then remote.
    if (m_inFlight.contains(fullKey))
        return;
    m_inFlight.insert(fullKey);

    QPointer<ViewerWindow> self(this);
    Source* source = m_source;
    const QString md5 = image.md5;
    QThreadPool::globalInstance()->start([self, source, image, md5]() {
        QString error;
        QByteArray bytes;
        if (!md5.isEmpty()) {
            LibrarySource library;
            if (library.hasImage(md5)) {
                bytes = library.fetch(image.withMd5(md5), FileKind::Full, &error);
            }
        }
        if (bytes.isEmpty())
            bytes = source->fetch(image, FileKind::Full, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, bytes, error]() {
                if (!self)
                    return;
                if (bytes.isEmpty()) {
                    self->m_inFlight.remove(cacheKey(md5, FileKind::Full));
                    if (self->m_current.md5 != md5)
                        return;  // moved on
                    self->m_loading = false;
                    self->m_statusLabel->setText(QStringLiteral("Load failed: %1").arg(error));
                    return;
                }
                self->cacheAndDeliver(md5, FileKind::Full, bytes);
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::loadVideoProgressively(const SourceImage& image) {
    if (image.isLocal() && QFile::exists(image.localPath)) {
        m_loading = false;
        m_statusLabel->clear();
        showVideoPath(image.localPath);
        return;
    }

    const QString fullKey = cacheKey(image.md5, FileKind::Full);
    if (m_inFlight.contains(fullKey))
        return;
    m_inFlight.insert(fullKey);

    QString ext = image.format.toLower();
    if (ext.isEmpty() && !image.localPath.isEmpty()) {
        ext = QFileInfo(image.localPath).suffix().toLower();
    }

    if (ext.isEmpty())
        ext = QStringLiteral("mp4");

    QDir dir(Paths::tempDir());
    dir.mkpath(QStringLiteral("."));
    const QString stem = image.md5.isEmpty() ? QStringLiteral("media") : image.md5;
    const QString finalPath = dir.filePath(stem + QLatin1Char('.') + ext);
    const QString partPath = finalPath + QStringLiteral(".part");

    if (QFileInfo::exists(finalPath)) {
        m_inFlight.remove(fullKey);
        m_loading = false;
        m_statusLabel->clear();
        showVideoPath(finalPath);
        return;
    }

    const int generation = m_loadGeneration;
    QPointer<ViewerWindow> self(this);
    Source* source = m_source;

    const QString md5 = image.md5;
    m_statusLabel->setText(QStringLiteral("Downloading video…"));

    // Probe the growing .part file so playback starts before the download
    // finishes.
    m_probeAttempts = 0;
    m_streamProbePath = partPath;
    m_streamProbeMd5 = md5;
    if (!m_streamProbeTimer) {
        m_streamProbeTimer = new QTimer(this);
        m_streamProbeTimer->setInterval(2000);
        connect(m_streamProbeTimer, &QTimer::timeout, this, &ViewerWindow::onStreamProbeTick);
    }
    m_streamProbeTimer->start();

    QThreadPool::globalInstance()->start([self, source, image, md5, finalPath, partPath,
                                          generation]() {
        QString error;
        bool ok = false;
        QFile::remove(partPath);
        if (!md5.isEmpty()) {
            LibrarySource library;
            if (library.hasImage(md5)) {
                ok = library.downloadToFile(image.withMd5(md5), FileKind::Full, partPath, nullptr,
                                            &error);
            }
        }

        if (!ok) {
            ok = source->downloadToFile(
                image, FileKind::Full, partPath,
                [self, md5, generation](qint64 done, qint64 total) {
                    if (!self)
                        return;
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self, md5, generation, done, total]() {
                            if (!self)
                                return;
                            if (self->m_loadGeneration != generation ||
                                self->m_current.md5 != md5) {
                                return;
                            }
                            const QString text = total > 0
                                                     ? QStringLiteral("Downloading video… %1%")
                                                           .arg(done * 100 / total)
                                                     : QStringLiteral("Downloading video… %1")
                                                           .arg(formatBytes(done));
                            self->m_statusLabel->setText(text);
                        },
                        Qt::QueuedConnection);
                },
                &error);
        }

        if (!self)
            return;

        // The probe check touches GUI members; must run on the UI thread.
        const QString partCopy = partPath;
        const QString finalCopy = finalPath;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, finalPath, partPath, ok, error, generation, partCopy, finalCopy]() mutable {
                if (!self)
                    return;
                const QString key = cacheKey(md5, FileKind::Full);
                self->m_inFlight.remove(key);
                if (self->m_loadGeneration != generation || self->m_current.md5 != md5)
                    return;

                if (ok) {
                    // If the probe is playing the .part file, defer the
                    // rename to stopMedia() - the decoder holds it open.
                    const bool streaming =
                        self->m_streamingPart && self->m_playback && self->m_playback->isActive();
                    qDebug().noquote()
                        << "[feed/finalize] download done:" << partPath
                        << "size:" << QFileInfo(partPath).size() << "streaming:" << streaming;
                    if (streaming) {
                        self->m_pendingFinalizePart = partPath;
                        self->m_pendingFinalizeFinal = finalPath;
                        self->stopStreamProbe();
                        return;
                    }
                    QFile::remove(finalPath);
                    if (!QFile::rename(partPath, finalPath)) {
                        // A just-closed handle can linger briefly.
                        QThread::msleep(100);
                        if (!QFile::rename(partPath, finalPath)) {
                            ok = false;
                            if (error.isEmpty())
                                error = QStringLiteral("Unable to finalize video file");
                            qWarning().noquote()
                                << "[feed/finalize] rename failed:" << partPath << "->" << finalPath
                                << "part size:" << QFileInfo(partPath).size();
                        }
                    }
                }

                if (!ok) {
                    QFile::remove(partPath);
                    self->m_loading = false;
                    self->m_statusLabel->setText(
                        QStringLiteral("Video download failed: %1").arg(error));
                    return;
                }

                self->m_loading = false;
                self->m_statusLabel->clear();
                self->showVideoPath(finalPath);
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::stopStreamProbe() {
    if (m_streamProbeTimer)
        m_streamProbeTimer->stop();
    m_streamProbePath.clear();
    m_streamProbeMd5.clear();
    m_probeBusy = false;
}

void ViewerWindow::onStreamProbeTick() {
    // Try to open the growing .part file so playback starts early.
    if (!m_streamProbeTimer || !m_streamProbeTimer->isActive())
        return;
    if (m_probeBusy)
        return;
    if (m_playback && m_playback->isActive())
        return;

    const qint64 size =
        QFileInfo::exists(m_streamProbePath) ? QFileInfo(m_streamProbePath).size() : 0;
    if (size < 256 * 1024)
        return;

    m_probeBusy = true;
    auto* decoder = new VideoDecoder();
    if (decoder->open(m_streamProbePath)) {
        m_statusLabel->setText(QStringLiteral("Streaming video…"));
        startVideoPlayback(decoder);
        // Mark AFTER startVideoPlayback, which calls stopMedia() and clears
        // the flag for the old decoder.
        m_streamingPart = true;
    } else {
        delete decoder;
        ++m_probeAttempts;
        // Give up after ~30 attempts; the completion callback plays the file.
        if (m_probeAttempts > 30) {
            stopStreamProbe();
            return;
        }
    }
    m_probeBusy = false;
}

void ViewerWindow::cacheAndDeliver(const QString& md5, FileKind kind, const QByteArray& bytes) {
    const QString key = cacheKey(md5, kind);
    m_inFlight.remove(key);
    if (bytes.isEmpty()) {
        m_cache.remove(key);
        return;
    }
    if (!m_cache.contains(key))
        m_cacheOrder.append(key);
    m_cache.insert(key, bytes);
    evictCache();

    if (kind == FileKind::Full && m_current.md5 == md5 && m_loading)
        deliverMedia(bytes);
}

ViewerWindow::MediaKind ViewerWindow::mediaKindOf(const SourceImage& image) const {
    QString f = image.format.toLower();

    if (f.isEmpty() && !image.localPath.isEmpty())
        f = QFileInfo(image.localPath).suffix().toLower();

    if (f == QLatin1String("webm") || f == QLatin1String("mp4") || f == QLatin1String("mov") ||
        f == QLatin1String("mkv") || f == QLatin1String("avi") || f == QLatin1String("m4v")) {
        return MediaKind::Video;
    }

    return MediaKind::Image;
}

void ViewerWindow::deliverMedia(const QByteArray& bytes) {
    m_loading = false;
    m_statusLabel->clear();

    QString ext = m_current.format.toLower();

    if (ext.isEmpty() && !m_current.localPath.isEmpty())
        ext = QFileInfo(m_current.localPath).suffix().toLower();

    const QString path = writeTempFile(bytes, ext.isEmpty() ? QStringLiteral("media") : ext);
    VideoDecoder decoder;
    if (decoder.open(path)) {
        if (decoder.isAnimated()) {
            m_animatedMedia = true;
            showVideoPath(path);
            return;
        }

        const auto frame = decoder.nextFrame();
        if (!frame.image.isNull()) {
            m_animatedMedia = false;
            m_viewer->showImage(frame.image);
            return;
        }
    }

    m_animatedMedia = false;
    showImageBytes(bytes);
}

void ViewerWindow::stopMedia() {
    stopStreamProbe();
    m_streamingPart = false;

    // Release decoder/audio first: on Windows, renaming an open file fails.
    if (m_playback)
        m_playback->stop();

    m_scrubbing = false;

    // Finalize a streamed download deferred while the .part file was open.
    if (!m_pendingFinalizePart.isEmpty()) {
        QFile::remove(m_pendingFinalizeFinal);
        if (!QFile::rename(m_pendingFinalizePart, m_pendingFinalizeFinal)) {
            QThread::msleep(100);
            if (!QFile::rename(m_pendingFinalizePart, m_pendingFinalizeFinal)) {
                qWarning().noquote()
                    << "[feed/finalize] rename failed:" << m_pendingFinalizePart << "->"
                    << m_pendingFinalizeFinal << "size:" << QFileInfo(m_pendingFinalizePart).size();
                QFile::remove(m_pendingFinalizePart);
            }
        }
        m_pendingFinalizePart.clear();
        m_pendingFinalizeFinal.clear();
    }

    if (m_gif) {
        delete m_gif;
        m_gif = nullptr;
    }

    if (m_videoControls)
        m_videoControls->hide();
}

void ViewerWindow::showImageBytes(const QByteArray& bytes) {
    QImage image;
    if (!image.loadFromData(bytes)) {
        m_statusLabel->setText(QStringLiteral("Cannot decode image"));
        return;
    }
    m_currentMediaFile.clear();
    m_mediaStack->setCurrentWidget(m_viewer);
    m_viewer->showImage(image);
}

void ViewerWindow::showGifPath(const QString& path) {
    delete m_gif;
    m_currentMediaFile = path;
    m_gif = new QMovie(path, QByteArray(), this);
    if (!m_gif->isValid()) {
        delete m_gif;
        m_gif = nullptr;
        m_statusLabel->setText(QStringLiteral("Cannot play gif"));
        return;
    }
    m_animatedMedia = true;
    m_mediaStack->setCurrentWidget(m_viewer);
    connect(m_gif, &QMovie::frameChanged, this, [this](int) {
        if (!m_gif)
            return;
        const QPixmap pixmap = m_gif->currentPixmap();
        if (!pixmap.isNull())
            m_viewer->showImage(pixmap.toImage());
    });
    connect(m_gif, &QMovie::finished, this, [this]() {
        if (m_gif && m_repeatButton && m_repeatButton->isChecked())
            m_gif->start();
    });
    m_gif->start();
}

void ViewerWindow::showVideoPath(const QString& path) {
    m_mediaStack->setCurrentWidget(m_viewer);
    m_currentMediaFile = path;
    auto* decoder = new VideoDecoder();
    if (!decoder->open(path)) {
        delete decoder;
        m_statusLabel->setText(QStringLiteral("Cannot decode video"));
        return;
    }
    startVideoPlayback(decoder);
}

void ViewerWindow::startVideoPlayback(VideoDecoder* decoder) {
    if (!decoder)
        return;

    stopMedia();
    m_animatedMedia = true;

    if (!decoder->isOpen()) {
        decoder->close();
        delete decoder;
        return;
    }

    m_mediaStack->setCurrentWidget(m_viewer);
    m_playback->setVolume(m_volumeSlider->value());
    m_playback->start(decoder);

    const qint64 duration = m_playback->durationMs();
    m_progress->setRange(0,
                         static_cast<int>(qMin<qint64>(duration, std::numeric_limits<int>::max())));
    m_progress->setValue(0);
    m_timeLabel->setText(QStringLiteral("0:00 / %1").arg(formatTime(duration)));
    m_videoControls->show();
    m_playButton->setIcon(assetIcon(QStringLiteral("pause.svg")));

    onVideoFrame(m_playback->nextFrame());
}

QString ViewerWindow::writeTempFile(const QByteArray& bytes, const QString& ext) {
    QDir dir(Paths::tempDir());
    dir.mkpath(QStringLiteral("."));
    const QString stem = m_current.md5.isEmpty() ? QStringLiteral("media") : m_current.md5;
    const QString path = dir.filePath(stem + QLatin1Char('.') + ext);
    if (!QFileInfo::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(bytes);
    }
    return path;
}

// preloading

void ViewerWindow::preloadAround(int index) {
    // Nearest neighbours first so they win the fetch race.
    for (int offset : {1, -1, 2, -2}) {
        const int i = index + offset;
        if (i < 0 || i >= m_images.size())
            continue;
        const SourceImage& img = m_images.at(i);
        if (!img.isLocal())  // local preview == full file
            fetchIntoCache(img, FileKind::Preview);
        if (mediaKindOf(img) == MediaKind::Image)
            fetchIntoCache(img, FileKind::Full);
    }
}

void ViewerWindow::fetchIntoCache(const SourceImage& image, FileKind kind) {
    if (image.md5.isEmpty())
        return;
    const QString key = cacheKey(image.md5, kind);
    if (m_cache.contains(key) || m_inFlight.contains(key))
        return;
    m_inFlight.insert(key);

    QPointer<ViewerWindow> self(this);
    Source* source = m_source;
    QThreadPool::globalInstance()->start([self, source, image, kind]() {
        const QByteArray bytes = source->fetch(image, kind);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5 = image.md5, kind, bytes]() {
                if (self)
                    self->cacheAndDeliver(md5, kind, bytes);
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::evictCache() {
    while (m_cacheOrder.size() > kMaxCacheEntries) {
        const QString key = m_cacheOrder.takeFirst();
        m_cache.remove(key);
    }
}

void ViewerWindow::evictDistantCache() {
    // Unload media of posts far from the one on screen.
    QHash<QString, int> indexOfMd5;
    for (int i = 0; i < m_images.size(); ++i) indexOfMd5.insert(m_images.at(i).md5, i);

    QStringList kept;
    kept.reserve(m_cacheOrder.size());
    for (const QString& key : m_cacheOrder) {
        const QString md5 = key.section(QLatin1Char(':'), 0, 0);
        const auto it = indexOfMd5.constFind(md5);
        const bool near = it != indexOfMd5.constEnd() && qAbs(it.value() - m_index) <= kCacheRadius;
        if (near) {
            kept.append(key);
        } else {
            m_cache.remove(key);
            m_inFlight.remove(key);
        }
    }
    m_cacheOrder = kept;
}

// UI state

void ViewerWindow::setInfo(const SourceImage& image) {
    QString html;
    if (image.remoteId > 0)
        html += QStringLiteral("<b>#%1</b>&nbsp; ").arg(image.remoteId);
    html += QStringLiteral("%1x%2 · %3 · %4")
                .arg(image.width)
                .arg(image.height)
                .arg(formatBytes(image.fileSize),
                     image.format.isEmpty() ? QStringLiteral("?") : image.format);
    if (!image.rating.isEmpty())
        html += QStringLiteral(" · rating %1").arg(image.rating);
    if (image.remoteId > 0)
        html += QStringLiteral(" · ★%1").arg(image.score);
    if (!image.sourceUrl.isEmpty())
        html += QStringLiteral(" · <a href=\"%1\">post page</a>").arg(image.sourceUrl);
    if (image.hasChildren)
        html += QStringLiteral(
            " · <span style=\"color:#1e78d6; font-weight:bold;\">children available</span>");
    m_infoLabel->setText(html);
}

void ViewerWindow::updateSidebars() {
    const bool editable = currentIsLocal();
    m_tagSidebar->setImage(m_current, editable);

    m_saveButton->setEnabled(!editable && !m_current.md5.isEmpty());
    m_saveFavButton->setEnabled(!editable && !m_current.md5.isEmpty());
    m_deleteButton->setEnabled(editable);
    m_revealButton->setEnabled(editable && !m_current.localPath.isEmpty());
    m_favoriteButton->setEnabled(editable);

    // Parent/child buttons only make sense when the post has a parent/children.
    m_upButton->setVisible(m_current.parentId != 0);
    m_downButton->setVisible(m_current.hasChildren);

    if (editable) {
        QPointer<ViewerWindow> self(this);
        QThreadPool::globalInstance()->start([self]() {
            LibrarySource library;
            const QStringList names = library.allTagNames();
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, names]() {
                    if (self)
                        self->m_tagSidebar->refreshCompletions(names);
                },
                Qt::QueuedConnection);
        });
    }
    updateFavoriteButton();
}

void ViewerWindow::updateFavoriteButton() { m_favoriteButton->setChecked(m_current.favorite); }

bool ViewerWindow::currentIsLocal() const {
    return !m_current.md5.isEmpty() && (m_source->isLocal() || m_current.isLocal());
}

// library actions

void ViewerWindow::saveToLibrary(bool andFavorite) {
    if (m_current.md5.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("Nothing to save"));
        return;
    }
    if (currentIsLocal()) {
        m_statusLabel->setText(QStringLiteral("Already in library"));
        if (andFavorite && !m_current.favorite)
            toggleFavorite(true);
        return;
    }

    m_statusLabel->setText(QStringLiteral("Saving…"));
    const SourceImage meta = m_current;
    const QByteArray cachedBytes = m_cache.value(cacheKey(meta.md5, FileKind::Full));

    QPointer<ViewerWindow> self(this);
    Source* source = m_source;
    QThreadPool::globalInstance()->start([self, source, meta, cachedBytes, andFavorite]() {
        QString error;
        QByteArray data = cachedBytes;
        if (data.isEmpty())
            data = source->fetch(meta, FileKind::Full, &error);

        ImportResult result;
        if (data.isEmpty()) {
            result.error = error.isEmpty() ? QStringLiteral("download failed") : error;
        } else {
            LibrarySource library;
            result = library.importImage(meta, data);
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, result, meta, andFavorite]() {
                if (!self)
                    return;
                if (!result.ok) {
                    self->m_statusLabel->setText(
                        QStringLiteral("Save failed: %1").arg(result.error));
                    return;
                }
                self->m_current.localPath = result.filePath;
                for (SourceImage& img : self->m_images) {
                    if (img.md5 == meta.md5)
                        img.localPath = result.filePath;
                }
                self->m_statusLabel->setText(result.alreadyExisted
                                                 ? QStringLiteral("Already in library")
                                                 : QStringLiteral("Saved to library"));
                self->updateSidebars();
                if (andFavorite && !self->m_current.favorite)
                    self->toggleFavorite(true);
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::toggleFavorite(bool favorite) {
    if (m_current.md5.isEmpty())
        return;
    const QString md5 = m_current.md5;
    QPointer<ViewerWindow> self(this);
    QThreadPool::globalInstance()->start([self, md5, favorite]() {
        LibrarySource library;
        QString error;
        const bool ok = library.setFavorite(md5, favorite, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, favorite, ok, error]() {
                if (!self)
                    return;
                if (!ok) {
                    self->m_statusLabel->setText(QStringLiteral("Favorite failed: %1").arg(error));
                    return;
                }
                self->m_current.favorite = favorite;
                // Keep the in-memory tag list in sync with the
                // usergroup:favorite tag the library maintains.
                QList<SourceTag> tags;
                for (const SourceTag& t : self->m_current.tags) {
                    if (t.category == QLatin1String("usergroup") &&
                        t.name == QLatin1String("favorite")) {
                        continue;
                    }
                    tags.append(t);
                }
                if (favorite) {
                    tags.append(SourceTag{QStringLiteral("favorite"), QStringLiteral("usergroup")});
                }
                self->m_current.tags = tags;
                self->updateSidebars();
            },
            Qt::QueuedConnection);
    });
}

// clipboard

void ViewerWindow::copyCurrentMedia() {
    QClipboard* clipboard = QApplication::clipboard();
    if (!clipboard) {
        m_statusLabel->setText(QStringLiteral("Clipboard unavailable"));
        return;
    }

    auto* mime = new QMimeData;

    // Offer the actual file when one exists.
    const QString file = currentMediaFile();
    if (!file.isEmpty())
        mime->setUrls({QUrl::fromLocalFile(QDir::toNativeSeparators(file))});

    // Also include the displayed frame as an image.
    const QImage image = m_viewer->image();
    if (!image.isNull())
        mime->setImageData(image);

    if (!mime->hasUrls() && !mime->hasImage()) {
        delete mime;
        m_statusLabel->setText(QStringLiteral("Nothing to copy"));
        return;
    }

    clipboard->setMimeData(mime);
    const bool isVideo = mediaKindOf(m_current) == MediaKind::Video;
    m_statusLabel->setText(isVideo ? QStringLiteral("Video copied to clipboard")
                                   : QStringLiteral("Image copied to clipboard"));
}

QString ViewerWindow::currentMediaFile() const {
    if (!m_currentMediaFile.isEmpty() && QFile::exists(m_currentMediaFile))
        return m_currentMediaFile;
    if (m_current.isLocal() && !m_current.localPath.isEmpty() && QFile::exists(m_current.localPath))
        return m_current.localPath;
    return QString();
}

// events

void ViewerWindow::keyPressEvent(QKeyEvent* event) {
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    switch (event->key()) {
        case Qt::Key_F11:
            if (isFullScreen())
                showNormal();
            else
                showFullScreen();
            raise();
            activateWindow();
            setFocus();
            break;
        case Qt::Key_Left:
            hideCursorIdle();
            showAt(m_index - 1);
            break;
        case Qt::Key_Right:
            hideCursorIdle();
            showAt(m_index + 1);
            break;
        case Qt::Key_Up:
            hideCursorIdle();
            navigateChain(true);
            break;
        case Qt::Key_Down:
            hideCursorIdle();
            navigateChain(false);
            break;
        case Qt::Key_Space:
            hideCursorIdle();
            onPlayPauseClicked();
            break;
        case Qt::Key_F5:
            hideCursorIdle();
            toggleSlideshow();
            break;
        case Qt::Key_Comma:
            scrubFrames(event->modifiers() & Qt::ShiftModifier ? -10 : -1);
            break;
        case Qt::Key_Period:
            scrubFrames(event->modifiers() & Qt::ShiftModifier ? 10 : 1);
            break;
        case Qt::Key_Less:
            scrubFrames(-1);
            break;
        case Qt::Key_Greater:
            scrubFrames(1);
            break;
        case Qt::Key_Escape:
            if (m_loupe && m_loupe->isVisible())
                hideLoupe();
            else
                close();
            break;
        case Qt::Key_Q:
            if (ctrl)
                close();
            break;
        case Qt::Key_S:
            if (ctrl)
                saveToLibrary(event->modifiers() & Qt::ShiftModifier);
            break;
        case Qt::Key_C:
            if (ctrl)
                copyCurrentMedia();
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

void ViewerWindow::wheelEvent(QWheelEvent* event) {
    // Loupe open: wheel zooms, Ctrl+wheel resizes the lens.
    if (m_loupe && m_loupe->isVisible() && m_viewer->isVisible()) {
        const int dir = event->angleDelta().y();
        if (dir != 0) {
            // Windows wheel events often lose the modifier state.
            const bool ctrl =
                (event->modifiers() | QApplication::keyboardModifiers()) & Qt::ControlModifier;
            if (ctrl) {
                m_loupeDiameter = qBound(200, m_loupeDiameter + (dir > 0 ? 24 : -24), 640);
                m_loupe->resizeLens(m_loupeDiameter);
                AppState state;
                if (state.load()) {
                    state.setViewerLoupeDiameter(m_loupeDiameter);
                    state.save();
                }
            } else {
                m_loupeZoom = qBound(0.0, m_loupeZoom + (dir > 0 ? 1.0 : -1.0), 16.0);
                AppState state;
                if (state.load()) {
                    state.setViewerLoupeZoom(m_loupeZoom);
                    state.save();
                }
            }
        }
        if (m_lastLoupePos != QPoint())
            updateLoupe(m_lastLoupePos);
        event->accept();
        return;
    }
    if (event->angleDelta().y() > 0) {
        showAt(m_index - 1);
    } else if (event->angleDelta().y() < 0) {
        showAt(m_index + 1);
    }
}

void ViewerWindow::hideCursorIdle() {
    m_lastMouseGlobalPos = QCursor::pos();
    if (!m_cursorHidden) {
        setCursor(Qt::BlankCursor);
        m_cursorHidden = true;
    }
}

void ViewerWindow::maybeShowCursor() {
    if (!m_cursorHidden)
        return;
    if (m_loupe && m_loupe->isVisible())
        return;
    // Ignore synthetic moves (window moved under a stationary cursor).
    const QPoint pos = QCursor::pos();
    if (pos == m_lastMouseGlobalPos)
        return;
    m_lastMouseGlobalPos = pos;
    unsetCursor();
    m_cursorHidden = false;
}

// magnifier

void ViewerWindow::showLoupeAt(const QPoint& globalPos) {
    const QImage image = m_viewer->image();
    if (image.isNull())
        return;

    if (!m_loupe) {
        m_loupe = new LoupeWidget(this);
        m_loupe->resizeLens(m_loupeDiameter);
        m_loupe->setImage(image);
    } else {
        m_loupe->setImage(image);
    }
    m_loupe->show();
    m_loupe->raise();
    m_lastLoupePos = globalPos;
    updateLoupe(globalPos);
}

void ViewerWindow::updateLoupe(const QPoint& globalPos) {
    if (!m_loupe)
        return;

    const QImage image = m_viewer->image();
    if (image.isNull()) {
        hideLoupe();
        return;
    }

    // Map the cursor into image pixels.
    const QPoint widgetPos = m_viewer->mapFromGlobal(globalPos);
    const int availW = m_viewer->width();
    const int availH = m_viewer->height();
    const double scale = qMin(double(availW) / image.width(), double(availH) / image.height());
    const double dispW = image.width() * scale;
    const double dispH = image.height() * scale;
    const double originX = (availW - dispW) / 2.0;
    const double originY = (availH - dispH) / 2.0;

    double px = (widgetPos.x() - originX) / scale;
    double py = (widgetPos.y() - originY) / scale;
    if (px < 0 || py < 0 || px >= image.width() || py >= image.height()) {
        hideLoupe();
        return;
    }

    m_loupe->setMagnifiedPoint(QPoint(int(px), int(py)), scale * (1.0 + m_loupeZoom));

    const int d = m_loupe->width();
    m_loupe->move(mapFromGlobal(globalPos) - QPoint(d / 2, d / 2));
    m_loupe->raise();
    m_lastLoupePos = globalPos;
    if (!m_cursorHidden) {
        setCursor(Qt::BlankCursor);
        m_cursorHidden = true;
    }
}

void ViewerWindow::hideLoupe() {
    if (m_loupe)
        m_loupe->hide();
    m_lastLoupePos = QPoint();
    if (m_cursorHidden) {
        unsetCursor();
        m_cursorHidden = false;
    }
}

void ViewerWindow::mousePressEvent(QMouseEvent* event) {
    switch (event->button()) {
        case Qt::MiddleButton:
            close();
            break;
        default:
            QWidget::mousePressEvent(event);
    }
}

bool ViewerWindow::eventFilter(QObject* watched, QEvent* event) {
    // Click-to-seek on the progress track.
    if (watched == m_progress && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && m_progress->maximum() > 0) {
            const int handle = m_progress->style()->pixelMetric(QStyle::PM_SliderLength);
            const int usable = qMax(1, m_progress->width() - handle);
            const int x = qBound(0, mouse->pos().x() - handle / 2, usable);
            const int value = qRound(double(x) / usable * m_progress->maximum());
            m_scrubbing = true;
            seekAndShow(value);
            m_scrubbing = false;
            return true;
        }
    }
    if (event->type() == QEvent::MouseMove) {
        if (rect().contains(mapFromGlobal(QCursor::pos())) && m_cursorHidden)
            maybeShowCursor();
    }
    if (watched == m_viewer) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                close();
                return true;
            }
            if (mouse->button() == Qt::RightButton) {
                if (m_loupe && m_loupe->isVisible())
                    hideLoupe();
                else
                    showLoupeAt(m_viewer->mapToGlobal(mouse->pos()));
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (m_loupe && m_loupe->isVisible())
                updateLoupe(m_viewer->mapToGlobal(static_cast<QMouseEvent*>(event)->pos()));
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_revealButton->click();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// slots

void ViewerWindow::onSaveClicked() { saveToLibrary(false); }

void ViewerWindow::onFavoriteClicked() { toggleFavorite(!m_current.favorite); }

void ViewerWindow::onPlayPauseClicked() {
    if (!m_playback || !m_playback->isActive())
        return;

    const bool playing = m_playback->togglePlayPause();

    if (playing && m_progress->maximum() > 0 && m_progress->value() >= m_progress->maximum()) {
        m_playback->seekTo(0);
        m_progress->setValue(0);
    }

    m_playButton->setIcon(
        assetIcon(playing ? QStringLiteral("pause.svg") : QStringLiteral("play.svg")));
    if (playing)
        onVideoFrame(m_playback->nextFrame());
}

void ViewerWindow::onVideoFrame(const VideoDecoder::Frame& frame) {
    if (!m_playback || !m_playback->isActive() || m_scrubbing)
        return;

    if (frame.image.isNull()) {
        if (m_repeatButton && m_repeatButton->isChecked()) {
            // Avoid re-entrant null frames (corrupt media) overflowing the
            // stack; restart from the top instead.
            m_playback->seekTo(0);
            m_progress->setValue(0);
            return;
        }

        m_playback->pause();
        m_playButton->setIcon(assetIcon(QStringLiteral("play.svg")));
        return;
    }

    m_viewer->showImage(frame.image);
    const qint64 pos = frame.ptsMs >= 0 ? frame.ptsMs : m_progress->value();
    m_progress->setValue(static_cast<int>(qBound<qint64>(0, pos, m_playback->durationMs())));
    m_timeLabel->setText(
        QStringLiteral("%1 / %2").arg(formatTime(pos), formatTime(m_playback->durationMs())));
}

void ViewerWindow::seekAndShow(qint64 ms) {
    if (!m_playback || !m_playback->isActive())
        return;

    const VideoDecoder::Frame frame = m_playback->seekTo(ms);
    const qint64 duration = m_playback->durationMs();
    if (!frame.image.isNull()) {
        m_viewer->showImage(frame.image);
        const qint64 shown = frame.ptsMs >= 0 ? frame.ptsMs : ms;
        m_progress->setValue(static_cast<int>(shown));
        m_timeLabel->setText(
            QStringLiteral("%1 / %2").arg(formatTime(shown), formatTime(duration)));
    } else {
        m_progress->setValue(static_cast<int>(ms));
        m_timeLabel->setText(QStringLiteral("%1 / %2").arg(formatTime(ms), formatTime(duration)));
    }
}

void ViewerWindow::scrubFrames(int frames) {
    if (!m_playback || !m_playback->isActive())
        return;

    const qint64 from = static_cast<qint64>(m_progress->value());
    const VideoDecoder::Frame frame =
        m_playback->stepFrames(frames, from, static_cast<qint64>(m_progress->maximum()));

    if (!frame.image.isNull()) {
        m_viewer->showImage(frame.image);
        const qint64 shown = frame.ptsMs >= 0 ? frame.ptsMs : from;
        m_progress->setValue(static_cast<int>(shown));
        m_timeLabel->setText(
            QStringLiteral("%1 / %2").arg(formatTime(shown), formatTime(m_playback->durationMs())));
    }
}

void ViewerWindow::toggleSlideshow() {
    m_slideshow = !m_slideshow;
    m_slideshowButton->setChecked(m_slideshow);
    if (m_slideshow) {
        m_statusLabel->setText(QStringLiteral("Slideshow started (F5 to stop)"));
        onSlideshowTick();
    } else {
        m_slideshowTimer->stop();
        m_statusLabel->setText(QStringLiteral("Slideshow stopped"));
    }
}

void ViewerWindow::onSlideshowTick() {
    if (!m_slideshow)
        return;

    if (m_index + 1 < m_images.size())
        showAt(m_index + 1);
    else {
        m_slideshow = false;
        m_slideshowButton->setChecked(false);
        m_statusLabel->setText(QStringLiteral("Slideshow finished"));
        return;
    }

    // Dwell based on visual complexity (Laplacian variance).
    const SourceImage image = m_current;

    int dwellMs = 3000;
    if (mediaKindOf(image) == MediaKind::Video) {
        const qint64 duration =
            (m_playback && m_playback->isActive()) ? m_playback->durationMs() : 0;
        dwellMs = duration > 0 ? static_cast<int>(qMin<qint64>(duration + 500, 15000)) : 5000;
    } else {
        const QByteArray bytes = m_cache.value(cacheKey(image.md5, FileKind::Full));
        if (!bytes.isEmpty()) {
            const double complexity = computeComplexity(bytes);
            dwellMs = qBound(1500, 1500 + static_cast<int>(complexity * 3.2), 8000);
        }
    }
    m_slideshowTimer->start(dwellMs);
}

void ViewerWindow::onDeleteClicked() {
    if (m_current.md5.isEmpty() || !currentIsLocal())
        return;

    const QString md5 = m_current.md5;
    m_statusLabel->setText(QStringLiteral("Deleting…"));
    QPointer<ViewerWindow> self(this);
    QThreadPool::globalInstance()->start([self, md5]() {
        LibrarySource library;
        QString error;
        const bool ok = library.removeImage(md5, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, ok, error]() {
                if (!self)
                    return;
                if (!ok) {
                    self->m_statusLabel->setText(QStringLiteral("Delete failed: %1").arg(error));
                    return;
                }
                // Drop the post and show the neighbour.
                int index = self->m_index;
                for (int i = 0; i < self->m_images.size(); ++i) {
                    if (self->m_images.at(i).md5 == md5) {
                        self->m_images.removeAt(i);
                        if (i < index)
                            --index;
                        break;
                    }
                }
                self->m_cache.remove(cacheKey(md5, FileKind::Full));
                self->m_cache.remove(cacheKey(md5, FileKind::Preview));
                if (self->m_images.isEmpty()) {
                    self->close();
                    return;
                }
                self->showAt(qBound(0, index, self->m_images.size() - 1));
                self->m_statusLabel->setText(QStringLiteral("Removed from library"));
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::onProgressMoved(int value) {
    if (!m_scrubbing)
        return;
    if (!m_playback || !m_playback->isActive()) {
        return;
    }
    seekAndShow(value);
}

void ViewerWindow::onVolumeChanged(int value) {
    if (m_playback)
        m_playback->setVolume(value);
}

void ViewerWindow::onAddTag(const QString& tag) {
    QString name = Schema::normalizeTagName(tag);
    if (name.isEmpty() || m_current.md5.isEmpty())
        return;
    // Strip an optional "category:" prefix ("species:fox").
    const int colon = name.indexOf(QLatin1Char(':'));
    if (colon > 0)
        name = name.mid(colon + 1);
    if (name.isEmpty())
        return;
    for (const SourceTag& t : m_current.tags) {
        if (t.name == name)
            return;  // already tagged
    }
    const QString md5 = m_current.md5;
    QPointer<ViewerWindow> self(this);
    QThreadPool::globalInstance()->start([self, md5, name]() {
        LibrarySource library;
        QString error;
        const QString category = library.addTag(md5, name, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, name, category, error]() {
                if (!self)
                    return;
                if (category.isEmpty()) {
                    self->m_statusLabel->setText(QStringLiteral("Add tag failed: %1").arg(error));
                    return;
                }
                self->m_current.tags.append(SourceTag{name, category});
                self->updateSidebars();
            },
            Qt::QueuedConnection);
    });
}

void ViewerWindow::onRemoveTag(const QString& tag) {
    const QString name = tag.trimmed().toLower();
    if (name.isEmpty() || m_current.md5.isEmpty())
        return;
    const QString md5 = m_current.md5;
    QPointer<ViewerWindow> self(this);
    QThreadPool::globalInstance()->start([self, md5, name]() {
        LibrarySource library;
        QString error;
        const bool ok = library.removeTag(md5, name, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, name, ok, error]() {
                if (!self)
                    return;
                if (!ok) {
                    self->m_statusLabel->setText(
                        QStringLiteral("Remove tag failed: %1").arg(error));
                    return;
                }
                QList<SourceTag> tags;
                for (const SourceTag& t : self->m_current.tags) {
                    if (t.name != name)
                        tags.append(t);
                }
                self->m_current.tags = tags;
                self->updateSidebars();
            },
            Qt::QueuedConnection);
    });
}

}  // namespace Mosaic
