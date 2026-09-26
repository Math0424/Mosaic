#pragma once

#include <QHash>
#include <QList>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include "MediaPlaybackController.h"
#include "../sources/Source.h"

class QLabel;
class QMovie;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace Mosaic {

class LoupeWidget;
class TagSidebar;
class ViewerWidget;

// Standalone post viewer opened from a source tab. Collapsible tag sidebar
// on the left; media, video controls and a navigation toolbar at the
// bottom. Neighbouring posts are preloaded for instant Left/Right.

// Keybinds: Left/Right posts, Up/Down parent/child chain, Space
// play/pause, F5 slideshow, ,/. frame step, Ctrl+S save,
// Ctrl+Shift+S save+favorite, Ctrl+C copy, Esc/Ctrl+Q/middle-click close.
class ViewerWindow : public QWidget {
    Q_OBJECT

   public:
    ViewerWindow(Source* source, const QList<SourceImage>& images, int index,
                 QWidget* parent = nullptr);
    ~ViewerWindow() override;

    // Re-points an open viewer at a new result list and jumps to `index`.
    void setImages(Source* source, const QList<SourceImage>& images, int index);

   protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

   signals:
    void tagSearchRequested(const QString& tag);

   private slots:
    void onSaveClicked();
    void onDeleteClicked();
    void onFavoriteClicked();
    void onPlayPauseClicked();
    void onVideoFrame(const Mosaic::VideoDecoder::Frame& frame);
    void onProgressMoved(int value);
    void onVolumeChanged(int value);
    void onAddTag(const QString& tag);
    void onRemoveTag(const QString& tag);
    void onSlideshowTick();

   private:
    enum class MediaKind { Image, AnimatedImage, Video };

    void buildUi();
    void showAt(int index);
    void navigateChain(bool up);
    bool chainTopIsParent() const;
    bool chainTopIsChild() const;

    void loadMedia(const SourceImage& image);
    void loadVideoProgressively(const SourceImage& image);
    MediaKind mediaKindOf(const SourceImage& image) const;
    void deliverMedia(const QByteArray& bytes);
    void stopMedia();
    void onStreamProbeTick();

    void showImageBytes(const QByteArray& bytes);
    void showGifPath(const QString& path);
    void showVideoPath(const QString& path);
    void startVideoPlayback(VideoDecoder* decoder);
    QString writeTempFile(const QByteArray& bytes, const QString& ext);

    // While the download streams into `partPath`, periodically try to open
    // the growing file so playback starts before it is complete.
    void startStreamProbe(const QString& partPath, const QString& md5);
    void stopStreamProbe();

    void preloadAround(int index);
    void fetchIntoCache(const SourceImage& image, FileKind kind);
    void evictCache();
    void evictDistantCache();
    void cacheAndDeliver(const QString& md5, FileKind kind, const QByteArray& bytes);

    void seekAndShow(qint64 ms);

    void setInfo(const SourceImage& image);
    void updateSidebars();
    void updateAnimatedBadge();
    void placeAnimatedBadge();
    void updateFavoriteButton();
    bool currentIsLocal() const;

    void saveToLibrary(bool andFavorite);
    void toggleFavorite(bool favorite);
    void checkLibraryStatus();
    void toggleSlideshow();
    void scrubFrames(int frames);

    void copyCurrentMedia();
    QString currentMediaFile() const;

    // Hides the cursor until the mouse actually moves again.
    void hideCursorIdle();
    void maybeShowCursor();

    // Magnifier loupe: circular zoom lens under the cursor.
    void showLoupeAt(const QPoint& globalPos);
    void updateLoupe(const QPoint& globalPos);
    void hideLoupe();

    Source* m_source = nullptr;
    QList<SourceImage> m_images;
    int m_index = 0;

    // Breadcrumb stack for parent/child chain navigation.
    QList<SourceImage> m_chainStack;
    SourceImage m_current;

    QString m_currentMediaFile;

    // Layout
    TagSidebar* m_tagSidebar = nullptr;
    QStackedWidget* m_mediaStack = nullptr;
    ViewerWidget* m_viewer = nullptr;
    QLabel* m_gifLabel = nullptr;
    QLabel* m_animatedBadge = nullptr;  // "animated" overlay on media
    QLabel* m_infoLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    // Video controls
    QWidget* m_videoControls = nullptr;
    QToolButton* m_playButton = nullptr;
    QSlider* m_progress = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_volumeSlider = nullptr;

    // Bottom toolbar
    QToolButton* m_leftButton = nullptr;
    QToolButton* m_rightButton = nullptr;
    QToolButton* m_upButton = nullptr;
    QToolButton* m_downButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_saveFavButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QToolButton* m_revealButton = nullptr;
    QToolButton* m_favoriteButton = nullptr;
    QToolButton* m_repeatButton = nullptr;
    QToolButton* m_slideshowButton = nullptr;

    // Media playback
    MediaPlaybackController* m_playback = nullptr;
    QMovie* m_gif = nullptr;
    bool m_scrubbing = false;
    bool m_loading = false;

    // Preload cache
    QHash<QString, QByteArray> m_cache;
    QStringList m_cacheOrder;
    QSet<QString> m_inFlight;

    // Progressive video download state
    QTimer* m_streamProbeTimer = nullptr;
    QString m_streamProbePath;
    QString m_streamProbeMd5;
    bool m_probeBusy = false;
    int m_probeAttempts = 0;
    int m_loadGeneration = 0;  // invalidates stale async loads
    QString m_pendingFinalizePart;
    QString m_pendingFinalizeFinal;
    bool m_streamingPart = false;

    // Slideshow (F5)
    QTimer* m_slideshowTimer = nullptr;
    bool m_slideshow = false;

    // Cursor auto-hide
    bool m_cursorHidden = false;
    QPoint m_lastMouseGlobalPos;

    // Magnifier loupe
    LoupeWidget* m_loupe = nullptr;
    double m_loupeZoom = 2.0;
    int m_loupeDiameter = 250;

    bool m_animatedMedia = false;  // loupe disabled for animated media
    QPoint m_lastLoupePos;
};

}  // namespace Mosaic
