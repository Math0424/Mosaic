#pragma once

#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <QStringList>

#include "../sources/Source.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QCompleter;
class QTimer;

namespace Mosaic {

// One clickable thumbnail in the results grid. Shows a placeholder until
// the preview arrives; caption shows post id and score.
class ThumbnailButton : public QWidget {
    Q_OBJECT

   public:
    explicit ThumbnailButton(const SourceImage& image, QWidget* parent = nullptr);

    const SourceImage& image() const { return m_image; }

    void setPixmap(const QPixmap& pixmap);
    void releasePixmap() {
        m_pixmap = {};
        m_scaled = {};
        update();
    }
    bool hasPixmap() const { return !m_pixmap.isNull(); }
    QSize sizeHint() const override;

   signals:
    void clicked();

   protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

   private:
    SourceImage m_image;
    QPixmap m_pixmap;
    QPixmap m_scaled;  // m_pixmap fitted to the last paint size
    bool m_hovered = false;
};

// A browsing tab bound to one Source: tag search bar on top, infinitely
// scrolling thumbnail grid below. Clicking a thumbnail opens a ViewerWindow.
// All source I/O runs off the UI thread.
class SourceBrowser : public QWidget {
    Q_OBJECT

   public:
    explicit SourceBrowser(Source* source, QWidget* parent = nullptr);
    ~SourceBrowser() override;

    Source* source() const { return m_source; }

    QString tags() const;
    void setTags(const QString& tags);

    void setSearchBarVisible(bool visible);

    // Hides results created before the given unix time (seconds). 0 disables.
    void setMinCreatedAt(qint64 unixSeconds);

    void setBlacklist(const QStringList& tags);

    void search();

    // Runs the query anchored at `page`: Prev loads pages below, Next above.
    void searchFromPage(int page);

    int page() const { return m_page; }

    void applyGridSettings(int maxSize, int spacing);

    void closeViewerWindow();

   signals:
    void statusChanged(const QString& text);
    void tagSearchRequested(const QString& tag, const QString& sourceId);

   private slots:
    void onSearchClicked();
    void onGridScrolled();
    void onLoadMoreClicked();
    void onLoadPrevClicked();

   private:
    enum class FetchMode { Replace, Append, Prepend };

    void buildUi();
    void runSearch(int page, FetchMode mode);
    void appendResult(const SourceResult& result, FetchMode mode, int page);
    void clearGrid();
    void openViewer(int index);
    void requestPreview(const SourceImage& image);
    // Unloads thumbnails far outside the viewport; reloads them when they
    // scroll back into view.
    void recycleThumbnails();
    // Decodes only thumbnails in (or near) the viewport.
    void loadNearbyThumbnails();
    void refreshCompletions(const QStringList& tagNames);
    void updateCompletion();
    void updateTagColor();
    void updateEmptyLabel();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    Source* m_source = nullptr;

    // Search bar
    QWidget* m_searchBar = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QPushButton* m_searchButton = nullptr;
    QLineEdit* m_pageLabel = nullptr;
    QPushButton* m_prevButton = nullptr;
    QPushButton* m_loadMoreButton = nullptr;
    QCompleter* m_completer = nullptr;

    // Results grid
    QScrollArea* m_gridScroll = nullptr;
    QWidget* m_gridContainer = nullptr;
    QLabel* m_emptyLabel = nullptr;

    // 0 = use kPageSize; sources that ignore the limit (r34) report theirs.
    int m_effectivePageSize = 0;
    class FlowLayout* m_gridLayout = nullptr;

    int m_firstPage = 0;
    int m_prevPage = 0;
    int m_page = 0;
    int m_nextPage = 1;
    bool m_searching = false;
    bool m_exhausted = false;
    qint64 m_minCreatedAt = 0;  // unix seconds; 0 = off
    QStringList m_blacklist;
    QHash<QString, QString> m_tagCategories;
    QList<SourceImage> m_images;
    QList<ThumbnailButton*> m_thumbnails;

    class ViewerWindow* m_viewer = nullptr;
    QSet<QString> m_previewInFlight;
    QTimer* m_recycleTimer = nullptr;
};

}  // namespace Mosaic
