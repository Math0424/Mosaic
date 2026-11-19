#include "SourceBrowser.h"

#include "FlowLayout.h"
#include "Icons.h"
#include "TagColors.h"
#include "ViewerWindow.h"
#include "../app/MainWindow.h"
#include "../app/Theme.h"
#include "../core/Paths.h"
#include "../core/ThreadPools.h"

#include <QtConcurrentRun>
#include <QAbstractItemView>
#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringListModel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QCompleter>
#include <QScrollBar>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include "../media/VideoDecoder.h"
#include "../sources/local/LibrarySource.h"
#include <QKeyEvent>

namespace Mosaic {

namespace {

constexpr int kThumbSize = 168;      ///< total thumbnail cell size
constexpr int kImageSize = 150;      ///< image area inside the cell
constexpr int kPageSize = 75;        ///< images requested per page
constexpr int kPrefetchPx = 400;     ///< load next page this close to the bottom
constexpr int kRecycleMargin = 800;  ///< keep decoded thumbs this far beyond the viewport
// Longest edge of a decoded thumbnail. Tiles paint at ~168px and stretch a
// little to fill the row; x2 covers HiDPI. Library previews are the original
// file, so without this cap one tile could hold a 100+ MB bitmap.
constexpr int kThumbMaxEdge = 400;

QSize thumbTargetSize(const QSize& size) {
    if (size.width() <= kThumbMaxEdge && size.height() <= kThumbMaxEdge)
        return size;
    return size.scaled(kThumbMaxEdge, kThumbMaxEdge, Qt::KeepAspectRatio);
}

QImage capThumb(const QImage& image) {
    if (image.isNull())
        return image;
    const QSize target = thumbTargetSize(image.size());
    return target == image.size()
               ? image
               : image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Decodes at thumbnail size. JPEG scales during decode, so big originals
// are never fully expanded in memory.
QImage readThumb(QImageReader& reader) {
    const QSize full = reader.size();
    if (full.isValid()) {
        const QSize target = thumbTargetSize(full);
        if (target != full)
            reader.setScaledSize(target);
    }
    return capThumb(reader.read());
}

// On-disk thumbnail cache folder.
QString thumbCacheDir() { return QDir(Paths::tempDir()).filePath(QStringLiteral("thumbcache")); }

bool isAnimatedFormat(const SourceImage& img) {
    QString f = img.format.toLower();
    if (f.isEmpty() && !img.localPath.isEmpty())
        f = QFileInfo(img.localPath).suffix().toLower();
    static const QStringList animated = {
        QStringLiteral("gif"),  QStringLiteral("webm"), QStringLiteral("mp4"),
        QStringLiteral("mov"),  QStringLiteral("mkv"),  QStringLiteral("avi"),
        QStringLiteral("m4v"),  QStringLiteral("apng"), QStringLiteral("swf"),
        QStringLiteral("webp"),
    };
    return animated.contains(f);
}

}  // namespace

// Thumbnail

ThumbnailButton::ThumbnailButton(const SourceImage& image, QWidget* parent)
    : QWidget(parent), m_image(image) {
    setCursor(Qt::PointingHandCursor);
    setToolTip(m_image.tooltipHtml());
}

QSize ThumbnailButton::sizeHint() const { return QSize(kThumbSize, kThumbSize); }

void ThumbnailButton::setPixmap(const QPixmap& pixmap) {
    m_pixmap = pixmap;
    m_scaled = {};
    update();
}

void ThumbnailButton::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Card: a rounded tile per cell so the grid reads evenly whatever the
    // image aspect ratios are. The 2px inset keeps a gutter at spacing 0.
    const QRectF card = QRectF(rect()).adjusted(2, 2, -2, -2);
    const qreal radius = qBound(4.0, width() / 22.0, 10.0);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(m_hovered ? Theme::Color::Hover : Theme::Color::Panel));
    p.drawRoundedRect(card, radius, radius);

    const qreal padding = qBound(4.0, width() / 48.0, 8.0);
    const QRectF imgArea = card.adjusted(padding, padding, -padding, -padding);

    if (m_pixmap.isNull()) {
        // Skeleton block while the thumbnail loads.
        p.setBrush(QColor(Theme::Color::Raised));
        p.drawRoundedRect(imgArea, radius - 2, radius - 2);
    } else {
        // Scaled in device pixels (sharp on HiDPI) and cached across repaints.
        const qreal dpr = devicePixelRatioF();
        const QSize target = (imgArea.size() * dpr).toSize();
        const bool fits = !m_scaled.isNull() && m_scaled.width() <= target.width() &&
                          m_scaled.height() <= target.height() &&
                          (m_scaled.width() == target.width() ||
                           m_scaled.height() == target.height());
        if (!fits) {
            m_scaled = m_pixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_scaled.setDevicePixelRatio(dpr);
        }
        const QSizeF shown = m_scaled.deviceIndependentSize();
        const QRectF imgRect(imgArea.x() + (imgArea.width() - shown.width()) / 2.0,
                             imgArea.y() + (imgArea.height() - shown.height()) / 2.0,
                             shown.width(), shown.height());
        QPainterPath clip;
        clip.addRoundedRect(imgRect, radius - 3, radius - 3);
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(imgRect.topLeft(), m_scaled);
        p.restore();
    }

    if (m_hovered) {
        QPen ring{QColor(Theme::Color::Accent)};
        ring.setWidthF(1.5);
        p.setPen(ring);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(card.adjusted(0.75, 0.75, -0.75, -0.75), radius, radius);
        p.setPen(Qt::NoPen);
    }

    // Badges: icons on dark pills so they stay readable on light images.
    QStringList badges;
    if (isAnimatedFormat(m_image))
        badges << QStringLiteral("play.svg");
    if (m_image.hasChildren)
        badges << QStringLiteral("images.svg");
    if (m_image.localPath.isEmpty() && m_image.isInLibrary())
        badges << QStringLiteral("file.svg");

    const int iconSize = qBound(14, width() / 14, 20);
    const int pill = iconSize + 8;
    const qreal gap = 4;
    qreal x = imgArea.right() - pill - 2;
    const qreal y = imgArea.top() + 2;
    for (const QString& badge : badges) {
        const QRectF pillRect(x, y, pill, pill);
        p.setBrush(QColor(15, 16, 19, 190));
        p.drawRoundedRect(pillRect, pill / 2.0 - 2, pill / 2.0 - 2);
        const QRect iconRect = pillRect.adjusted(4, 4, -4, -4).toRect();
        p.drawPixmap(iconRect, assetIcon(badge).pixmap(iconRect.size(), devicePixelRatioF()));
        x -= pill + gap;
    }
}

void ThumbnailButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked();
    }
    QWidget::mousePressEvent(event);
}

void ThumbnailButton::enterEvent(QEnterEvent* event) {
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void ThumbnailButton::leaveEvent(QEvent* event) {
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

// SourceBrowser

SourceBrowser::SourceBrowser(Source* source, QWidget* parent) : QWidget(parent), m_source(source) {
    buildUi();
}

SourceBrowser::~SourceBrowser() {
    if (m_viewer)
        m_viewer->close();
}

void SourceBrowser::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 4, 0);
    root->setSpacing(8);

    // --- Search bar ---
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(0, 0, 6, 0);
    bar->setSpacing(8);

    // Completes the word under the cursor, not just the whole query.
    m_completer = new QCompleter(this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setFilterMode(Qt::MatchStartsWith);
    m_completer->setMaxVisibleItems(12);
    connect(m_completer, QOverload<const QString&>::of(&QCompleter::activated), this,
            [this](const QString& completion) {
                const int cursor = m_tagEdit->cursorPosition();
                const QString text = m_tagEdit->text();
                int start = cursor;
                while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
                QString updated = text;
                updated.replace(start, cursor - start, completion);
                m_tagEdit->setText(updated);
                m_tagEdit->setCursorPosition(start + completion.size() + 1);
                m_completer->popup()->hide();
            });

    m_tagEdit = new QLineEdit(this);
    m_tagEdit->setPlaceholderText(QStringLiteral("Tags (space separated, -tag to exclude)…"));
    m_tagEdit->setClearButtonEnabled(true);
    m_tagEdit->addAction(assetIcon(QStringLiteral("search.svg")), QLineEdit::LeadingPosition);
    m_completer->setWidget(m_tagEdit);
    if (m_completer->popup())
        m_completer->popup()->installEventFilter(this);
    m_tagEdit->installEventFilter(this);

    // Use the library's tag list for completions, consistent across tabs.
    LibrarySource lib_src;
    refreshCompletions(lib_src.allTagNames());
    m_tagCategories = lib_src.allTagCategories();
    connect(m_tagEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        updateCompletion();
        updateTagColor();
    });
    connect(m_tagEdit, &QLineEdit::cursorPositionChanged, this, [this](int, int) {
        updateCompletion();
        updateTagColor();
    });

    m_searchButton = new QPushButton(QStringLiteral("Search"), this);
    m_searchButton->setDefault(true);

    // Editable page field: type a number and press Enter to jump.
    m_pageLabel = new QLineEdit(this);
    m_pageLabel->setFixedWidth(96);
    m_pageLabel->setProperty("segment", QStringLiteral("middle"));
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setPlaceholderText(QStringLiteral("page"));
    m_pageLabel->setToolTip(QStringLiteral("Type a page number and press Enter to jump to it"));
    connect(m_pageLabel, &QLineEdit::returnPressed, this, [this]() {
        bool ok = false;
        const int page = m_pageLabel->text().trimmed().toInt(&ok);
        if (ok && page >= 1) {
            m_pageLabel->clear();
            searchFromPage(page);
        }
    });

    // Pager: [‹][pages][›] drawn as one joined control (see segment in Theme.cpp).
    m_prevButton = new QPushButton(this);
    m_prevButton->setIcon(assetIcon(QStringLiteral("chevron-left.svg")));
    m_prevButton->setProperty("segment", QStringLiteral("left"));
    m_prevButton->setEnabled(false);
    m_prevButton->setToolTip(QStringLiteral("Load the previous page of results"));

    m_loadMoreButton = new QPushButton(this);
    m_loadMoreButton->setIcon(assetIcon(QStringLiteral("chevron-right.svg")));
    m_loadMoreButton->setProperty("segment", QStringLiteral("right"));
    m_loadMoreButton->setEnabled(false);
    m_loadMoreButton->setToolTip(QStringLiteral("Load the next page of results"));

    auto* pager = new QHBoxLayout();
    pager->setSpacing(0);
    pager->addWidget(m_prevButton);
    pager->addWidget(m_pageLabel);
    pager->addWidget(m_loadMoreButton);

    bar->addWidget(m_tagEdit, 1);
    bar->addWidget(m_searchButton);
    bar->addLayout(pager);

    // Wrapped in a widget so it can be hidden wholesale.
    auto* barWidget = new QWidget(this);
    barWidget->setContentsMargins(0, 0, 0, 0);
    barWidget->setLayout(bar);
    m_searchBar = barWidget;
    root->addWidget(barWidget);


    connect(m_searchButton, &QPushButton::clicked, this, &SourceBrowser::onSearchClicked);
    connect(m_tagEdit, &QLineEdit::returnPressed, this, &SourceBrowser::onSearchClicked);
    connect(m_prevButton, &QPushButton::clicked, this, &SourceBrowser::onLoadPrevClicked);
    connect(m_loadMoreButton, &QPushButton::clicked, this, &SourceBrowser::onLoadMoreClicked);

    // --- Thumbnail grid ---
    m_gridContainer = new QWidget();
    m_gridContainer->setContentsMargins(0, 0, 0, 0);
    m_gridContainer->setAutoFillBackground(true);
    m_gridContainer->setPalette(palette());
    auto* gridLayout = new FlowLayout(m_gridContainer, 0, 1, 1);
    gridLayout->setItemSizeRange(220);
    m_gridContainer->setLayout(gridLayout);
    m_gridLayout = gridLayout;

    m_gridScroll = new QScrollArea(this);
    m_gridScroll->setFrameShape(QFrame::NoFrame);
    m_gridScroll->setWidgetResizable(true);
    m_gridScroll->setWidget(m_gridContainer);
    m_gridScroll->viewport()->installEventFilter(this);
    root->addWidget(m_gridScroll, 1);

    // Shown when a search finished without results.
    m_emptyLabel = new QLabel(QStringLiteral("No results"), m_gridScroll);
    m_emptyLabel->setProperty("role", QStringLiteral("empty"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->hide();

    connect(m_gridScroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &SourceBrowser::onGridScrolled);

    // Debounced thumbnail RAM recycling while scrolling.
    m_recycleTimer = new QTimer(this);
    m_recycleTimer->setSingleShot(true);
    m_recycleTimer->setInterval(200);
    connect(m_recycleTimer, &QTimer::timeout, this, &SourceBrowser::recycleThumbnails);
}

void SourceBrowser::setMinCreatedAt(qint64 unixSeconds) { m_minCreatedAt = unixSeconds; }

void SourceBrowser::setBlacklist(const QStringList& tags) { m_blacklist = tags; }

QString SourceBrowser::tags() const { return m_tagEdit->text().trimmed(); }

void SourceBrowser::setTags(const QString& tags) { m_tagEdit->setText(tags); }

void SourceBrowser::setSearchBarVisible(bool visible) {
    if (m_searchBar)
        m_searchBar->setVisible(visible);
}

void SourceBrowser::search() { runSearch(1, FetchMode::Replace); }

void SourceBrowser::searchFromPage(int page) { runSearch(qMax(1, page), FetchMode::Replace); }

void SourceBrowser::onSearchClicked() { runSearch(1, FetchMode::Replace); }

void SourceBrowser::onLoadMoreClicked() { runSearch(m_nextPage, FetchMode::Append); }

void SourceBrowser::onLoadPrevClicked() {
    if (m_firstPage > 1)
        runSearch(m_firstPage - 1, FetchMode::Prepend);
}

void SourceBrowser::applyGridSettings(int maxSize, int spacing) {
    if (!m_gridLayout)
        return;
    m_gridLayout->setItemSizeRange(qMax(100, maxSize));
    m_gridLayout->setSpacing(spacing);
    m_gridLayout->invalidate();
}

void SourceBrowser::onGridScrolled() {
    if (!m_searching && !m_exhausted && !m_images.isEmpty()) {
        QScrollBar* bar = m_gridScroll->verticalScrollBar();
        if (bar->maximum() - bar->value() <= kPrefetchPx) {
            runSearch(m_nextPage, FetchMode::Append);
        }
    }

    // Unload far-scrolled thumbnails, debounced.
    if (m_recycleTimer)
        m_recycleTimer->start();
}

void SourceBrowser::runSearch(int page, FetchMode mode) {
    if (m_searching)
        return;
    m_searching = true;

    // Stamp page counters immediately so saveState() persists the right
    // page while the fetch is still in flight.
    if (mode == FetchMode::Replace) {
        m_page = page;
        m_firstPage = page;
        m_nextPage = page + 1;
    }

    updateEmptyLabel();

    m_searchButton->setEnabled(false);
    m_prevButton->setEnabled(false);
    m_loadMoreButton->setEnabled(false);
    showSearchProgress(
        QStringLiteral("Loading page %1 from %2").arg(page).arg(m_source->displayName()), 0, -1);

    SourceQuery query;
    query.tags = m_tagEdit->text().trimmed();
    for (const QString& tag : m_blacklist) query.tags += QStringLiteral(" -") + tag;
    query.page = page;
    query.limit = kPageSize;

    QPointer<SourceBrowser> self(this);
    query.progress = [self](const QString& stage, qint64 done, qint64 total) {
        QMetaObject::invokeMethod(
            self.data(),
            [self, stage, done, total]() {
                if (self && self->m_searching)
                    self->showSearchProgress(stage, done, total);
            },
            Qt::QueuedConnection);
    };
    Source* source = m_source;
    const auto future = QtConcurrent::run(ThreadPools::network(), [self, source, query, page, mode]() {
        const SourceResult result = source->search(query);
        if (self) {
            QMetaObject::invokeMethod(
                self.data(),
                [self, result, page, mode]() {
                    if (!self)
                        return;
                    self->appendResult(result, mode, page);
                },
                Qt::QueuedConnection);
        }
    });
    Q_UNUSED(future);
}

void SourceBrowser::showSearchProgress(const QString& stage, qint64 done, qint64 total) {
    if (total < 0) {
        m_loadingText = stage + QStringLiteral("…");
    } else {
        const QLocale locale;
        m_loadingText = QStringLiteral("%1… %2 / %3")
                            .arg(stage, locale.toString(done), locale.toString(total));
    }
    emit statusChanged(m_loadingText);
    updateEmptyLabel();
}

void SourceBrowser::appendResult(const SourceResult& result, FetchMode mode, int page) {
    m_searching = false;
    m_loadingText.clear();
    m_searchButton->setEnabled(true);

    if (!result.ok) {
        // Keep page counters across an HTTP error.
        emit statusChanged(QStringLiteral("Search failed: %1").arg(result.error));
        m_prevButton->setEnabled(m_firstPage > 1);
        m_loadMoreButton->setEnabled(!m_exhausted);
        m_pageLabel->setPlaceholderText(
            m_page > 0 ? QStringLiteral("pages %1–%2").arg(m_firstPage).arg(m_page)
                       : QStringLiteral("page"));
        updateEmptyLabel();
        return;
    }

    if (mode == FetchMode::Replace)
        clearGrid();

    // Keep the viewport stable when prepending.
    QScrollBar* bar = m_gridScroll->verticalScrollBar();
    const int oldHeight = m_gridContainer->height();
    const int oldScroll = bar->value();

    auto* layout = m_gridContainer->layout();
    int insertAt = (mode == FetchMode::Prepend) ? 0 : m_thumbnails.size();

    for (const SourceImage& img : result.images) {
        if (img.previewUrl.isEmpty() && img.sampleUrl.isEmpty() && img.localPath.isEmpty()) {
            continue;
        }
        // Exact Favorites cutoff on top of the day-granular date: metatag.
        if (m_minCreatedAt > 0 && img.createdAt > 0 && img.createdAt < m_minCreatedAt) {
            continue;
        }

        auto* thumb = new ThumbnailButton(img, m_gridContainer);
        if (mode == FetchMode::Prepend) {
            m_images.prepend(img);
            m_thumbnails.prepend(thumb);
            if (auto* flow = qobject_cast<FlowLayout*>(layout))
                flow->insertWidget(insertAt++, thumb);
            else
                layout->addWidget(thumb);
        } else {
            m_images.append(img);
            m_thumbnails.append(thumb);
            layout->addWidget(thumb);
        }
        // Resolve the index at click time so prepending never stales it.
        connect(thumb, &ThumbnailButton::clicked, this,
                [this, thumb]() { openViewer(m_thumbnails.indexOf(thumb)); });
    }

    if (mode == FetchMode::Replace) {
        m_firstPage = page;
        m_page = page;
    } else if (mode == FetchMode::Prepend) {
        m_firstPage = qMin(m_firstPage > 0 ? m_firstPage : page, page);
    } else {
        m_page = qMax(m_page, page);
    }

    if (mode == FetchMode::Prepend) {
        m_gridContainer->updateGeometry();
        m_gridContainer->adjustSize();
        bar->setValue(oldScroll + (m_gridContainer->height() - oldHeight));
    }

    // A short page means no more results.
    const int pageSize = result.pageLimit > 0 ? result.pageLimit : kPageSize;
    m_exhausted = result.images.isEmpty() || result.images.size() < pageSize;
    m_nextPage = m_page + 1;
    m_prevPage = qMax(0, m_firstPage - 1);
    m_prevButton->setEnabled(m_firstPage > 1);
    m_loadMoreButton->setEnabled(!m_exhausted);
    m_pageLabel->setPlaceholderText(QStringLiteral("pages %1–%2").arg(m_firstPage).arg(m_page));

    QString status = QStringLiteral("%1: %2 image(s) loaded (pages %3–%4)")
                         .arg(m_source->displayName())
                         .arg(m_images.size())
                         .arg(m_firstPage)
                         .arg(m_page);
    if (!result.warning.isEmpty())
        status += QStringLiteral(" — %1").arg(result.warning);
    emit statusChanged(status);

    // Deferred one event-loop tick: geometries are only final after the
    // layout pass.
    m_gridContainer->updateGeometry();
    QTimer::singleShot(0, this, [this]() { loadNearbyThumbnails(); });

    updateEmptyLabel();
}

void SourceBrowser::updateEmptyLabel() {
    if (!m_emptyLabel)
        return;
    // While a search runs on an empty grid the label shows its progress;
    // over existing results progress goes to the status bar only.
    if (m_searching && m_images.isEmpty() && !m_loadingText.isEmpty()) {
        m_emptyLabel->setText(m_loadingText);
        m_emptyLabel->show();
    } else {
        m_emptyLabel->setText(QStringLiteral("No results"));
        m_emptyLabel->setVisible(!m_searching && m_images.isEmpty() && m_page > 0);
    }
    m_emptyLabel->raise();
}

void SourceBrowser::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_emptyLabel && m_gridScroll)
        m_emptyLabel->setGeometry(m_gridScroll->rect());
}

void SourceBrowser::clearGrid() {
    for (ThumbnailButton* t : m_thumbnails) {
        t->deleteLater();
    }
    m_thumbnails.clear();
    m_images.clear();
    m_previewInFlight.clear();
    m_firstPage = 0;
    m_prevPage = 0;
    m_page = 0;
    m_nextPage = 1;
    m_exhausted = false;
    m_pageLabel->clear();
    m_pageLabel->setPlaceholderText(QStringLiteral("page"));
    updateEmptyLabel();
}

void SourceBrowser::openViewer(int index) {
    if (index < 0 || index >= m_images.size())
        return;
    // One viewer window per tab, reused across posts.
    if (!m_viewer) {
        m_viewer = new ViewerWindow(m_source, m_images, index);
        m_viewer->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_viewer, &QObject::destroyed, this, [this]() { m_viewer = nullptr; });
        connect(m_viewer, &ViewerWindow::tagSearchRequested, this,
                [this](const QString& tag) { emit tagSearchRequested(tag, m_source->id()); });
    } else {
        m_viewer->setImages(m_source, m_images, index);
    }
    m_viewer->show();
    m_viewer->raise();
    m_viewer->activateWindow();
}

void SourceBrowser::closeViewerWindow() {
    if (m_viewer) {
        m_viewer->close();
        m_viewer = nullptr;
    }
}

void SourceBrowser::requestPreview(const SourceImage& image) {
    if (image.md5.isEmpty())
        return;
    if (m_previewInFlight.contains(image.md5))
        return;
    m_previewInFlight.insert(image.md5);

    // Fetch off the UI thread; decoded previews are cached on disk. QImage
    // here - QPixmap is GUI-thread only.
    QPointer<SourceBrowser> self(this);
    Source* source = m_source;
    const QString md5 = image.md5;
    const QString cacheDir = thumbCacheDir();
    const auto future = QtConcurrent::run(ThreadPools::network(), [self, source, image, md5, cacheDir]() {
        QImage decoded;
        const QString cachePath = cacheDir + QLatin1Char('/') + md5 + QLatin1String(".jpg");

        bool shrinkCached = false;
        {
            QImageReader cached(cachePath);
            const QSize stored = cached.size();
            decoded = readThumb(cached);
            shrinkCached = !decoded.isNull() && stored.isValid() &&
                           thumbTargetSize(stored) != stored;
        }  // reader closed before the file is rewritten
        if (shrinkCached)
            decoded.save(cachePath, "JPG", 80);  // entry from before the size cap

        if (decoded.isNull()) {
            if (QFileInfo::exists(cachePath))
                QFile::remove(cachePath);

            const FileKind kind =
                !image.previewUrl.isEmpty() ? FileKind::Preview : FileKind::Sample;
            QByteArray bytes = source->fetch(image, kind);
            {
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                decoded = readThumb(reader);
            }
            if (decoded.isNull()) {
                QString ext = QFileInfo(image.localPath).suffix().toLower();
                if (ext.isEmpty()) {
                    ext = image.format.toLower();
                }
                if (!ext.isEmpty()) {
                    QTemporaryFile tmp(QDir::tempPath() + QLatin1String("/Mosaic-thumb-XXXXXX.") +
                                       ext);
                    if (tmp.open()) {
                        tmp.write(bytes);
                        tmp.flush();
                        tmp.close();
                        Mosaic::VideoDecoder decoder;
                        if (decoder.open(tmp.fileName())) {
                            const Mosaic::VideoDecoder::Frame frame = decoder.nextFrame();
                            if (!frame.image.isNull()) {
                                decoded = capThumb(frame.image);
                            } else {
                                qDebug() << "SourceBrowser::requestPreview: VideoDecoder returned "
                                            "no frame for"
                                         << md5;
                            }
                        } else {
                            qDebug() << "SourceBrowser::requestPreview: VideoDecoder failed to "
                                        "open"
                                     << tmp.fileName();
                        }
                    } else {
                        qDebug() << "SourceBrowser::requestPreview: failed to create temp "
                                    "thumbnail file"
                                 << tmp.fileName();
                    }
                }
            }

            if (!decoded.isNull()) {
                QDir().mkpath(cacheDir);
                decoded.save(cachePath, "JPG", 80);
            }
        }

        if (self) {
            QMetaObject::invokeMethod(
                self.data(),
                [self, md5, decoded]() {
                    if (!self)
                        return;
                    self->m_previewInFlight.remove(md5);
                    const QPixmap pixmap =
                        decoded.isNull() ? QPixmap() : QPixmap::fromImage(decoded);
                    for (ThumbnailButton* t : self->m_thumbnails) {
                        if (t->image().md5 == md5) {
                            if (!pixmap.isNull())
                                t->setPixmap(pixmap);
                            break;
                        }
                    }
                },
                Qt::QueuedConnection);
        }
    });
    Q_UNUSED(future);
}

void SourceBrowser::recycleThumbnails() {
    if (!m_gridScroll || m_thumbnails.isEmpty())
        return;

    // Visible part of the grid in container coordinates.
    const QRect visible(-m_gridContainer->pos(), m_gridScroll->viewport()->size());
    const QRect keep =
        visible.adjusted(-kRecycleMargin, -kRecycleMargin, kRecycleMargin, kRecycleMargin);

    for (ThumbnailButton* t : m_thumbnails) {
        const bool near = keep.intersects(t->geometry());
        if (!near && t->hasPixmap()) {
            t->releasePixmap();
        } else if (near && !t->hasPixmap()) {
            requestPreview(t->image());
        }
    }
}

void SourceBrowser::loadNearbyThumbnails() {
    if (!m_gridScroll || m_thumbnails.isEmpty())
        return;

    const QRect visible(-m_gridContainer->pos(), m_gridScroll->viewport()->size());
    const QRect keep =
        visible.adjusted(-kRecycleMargin, -kRecycleMargin, kRecycleMargin, kRecycleMargin);

    for (ThumbnailButton* t : m_thumbnails) {
        if (!t)
            continue;
        const bool near = keep.intersects(t->geometry());
        if (near && !t->hasPixmap()) {
            requestPreview(t->image());
        } else if (!near && t->hasPixmap()) {
            t->releasePixmap();
        }
    }
}

void SourceBrowser::refreshCompletions(const QStringList& tagNames) {
    m_completer->setModel(new QStringListModel(tagNames, m_completer));
}

bool SourceBrowser::eventFilter(QObject* watched, QEvent* event) {
    // Tab accepts the completer's highlighted item.
    if (m_completer && watched == m_completer->popup() && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Tab) {
            QString completion;
            const QModelIndex idx = m_completer->popup()->currentIndex();
            if (idx.isValid())
                completion = idx.data().toString();
            if (completion.isEmpty())
                completion = m_completer->currentCompletion();
            if (completion.isEmpty() && m_completer->completionCount() == 1)
                completion = m_completer->completionModel()->index(0, 0).data().toString();
            if (!completion.isEmpty()) {
                const int cursor = m_tagEdit->cursorPosition();
                const QString text = m_tagEdit->text();
                int start = cursor;
                while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
                QString updated = text;
                updated.replace(start, cursor - start, completion);
                m_tagEdit->setText(updated);
                m_tagEdit->setCursorPosition(start + completion.size() + 1);
            }
            m_completer->popup()->hide();
            m_tagEdit->setFocus();
            return true;
        }
    }

    if (watched == m_tagEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Tab) {
            QString completion;
            if (m_completer && m_completer->popup() && m_completer->popup()->isVisible()) {
                const QModelIndex idx = m_completer->popup()->currentIndex();
                if (idx.isValid())
                    completion = idx.data().toString();
            }
            if (completion.isEmpty() && m_completer) {
                completion = m_completer->currentCompletion();
                if (completion.isEmpty() && m_completer->completionCount() == 1) {
                    completion = m_completer->completionModel()->index(0, 0).data().toString();
                }
            }
            if (!completion.isEmpty()) {
                const int cursor = m_tagEdit->cursorPosition();
                const QString text = m_tagEdit->text();
                int start = cursor;
                while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
                QString updated = text;
                updated.replace(start, cursor - start, completion);
                m_tagEdit->setText(updated);
                m_tagEdit->setCursorPosition(start + completion.size() + 1);
            }
            return true;
        }
    }

    // Fullscreen fix, 'virtual' scroll if no scrollbar.
    if (m_gridScroll && watched == m_gridScroll->viewport() && event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        QScrollBar* bar = m_gridScroll->verticalScrollBar();
        const bool noScrollbar = bar->maximum() <= bar->minimum();
        const bool nearBottom = bar->maximum() - bar->value() <= kPrefetchPx;
        if (wheel->angleDelta().y() < 0 && (noScrollbar || nearBottom)) {
            if (!m_searching && !m_exhausted && !m_images.isEmpty()) {
                runSearch(m_nextPage, FetchMode::Append);
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SourceBrowser::updateCompletion() {
    // Complete the word under the cursor.
    const QString text = m_tagEdit->text();
    const int cursor = m_tagEdit->cursorPosition();

    int start = cursor;
    while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;

    QString token = text.mid(start, cursor - start);
    if (token.startsWith(QLatin1Char('-')))
        token.remove(0, 1);

    if (token.isEmpty()) {
        if (m_completer->popup())
            m_completer->popup()->hide();
        return;
    }

    m_completer->setCompletionPrefix(token);
    if (m_completer->completionCount() > 0) {
        QRect rect = m_tagEdit->rect();
        const int leftX =
            m_tagEdit->fontMetrics().horizontalAdvance(text.left(start)) + m_tagEdit->rect().left();
        rect.setLeft(leftX);
        m_completer->complete(rect);
    } else if (m_completer->popup()) {
        m_completer->popup()->hide();
    }
}

void SourceBrowser::updateTagColor() {
    if (!m_tagEdit)
        return;

    const QString text = m_tagEdit->text();
    const int cursor = m_tagEdit->cursorPosition();

    int start = cursor;
    while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
    int end = cursor;
    while (end < text.size() && text.at(end) != QLatin1Char(' ')) ++end;

    QString token = text.mid(start, end - start);
    if (token.startsWith(QLatin1Char('-')))
        token.remove(0, 1);

    // Qualified "usergroup:favorite" terms color by their group.
    QString category;
    const int colon = token.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString group = token.left(colon).toLower();
        if (group == QLatin1String("artist") || group == QLatin1String("character") ||
            group == QLatin1String("species") || group == QLatin1String("meta") ||
            group == QLatin1String("model") || group == QLatin1String("usergroup")) {
            category = group;
        }
    }
    if (category.isEmpty())
        category = m_tagCategories.value(token.toLower());

    // Stylesheet colors the text via the MosaicTagColor property.
    m_tagEdit->setProperty("MosaicTagColor", category.isEmpty() ? QVariant() : QVariant(category));
    if (m_tagEdit->style()) {
        m_tagEdit->style()->unpolish(m_tagEdit);
        m_tagEdit->style()->polish(m_tagEdit);
    }
}

}  // namespace Mosaic
