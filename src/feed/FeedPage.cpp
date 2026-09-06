#include "FeedPage.h"

#include "FeedDatabase.h"
#include "EmbeddingBackend.h"
#include "FeedRecommender.h"
#include "../core/AppState.h"
#include "../components/Icons.h"
#include "../components/TagSidebar.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"

#include <QDateTime>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include "../media/VideoDecoder.h"

namespace Mosaic {

namespace {

constexpr int kBatchSize = 10;
constexpr qint64 kSkipMs = 1500;
constexpr qint64 kFullWatchMs = 8000;
constexpr qint64 kAbandonMs = 60000;

// Clamp band for the adaptive skip threshold.
constexpr qint64 kSkipFloorMs = 1000;
constexpr qint64 kSkipCeilingMs = 4000;
constexpr qint64 kWatchFloorMs = 3000;
constexpr qint64 kWatchCeilingMs = 15000;
// The skip cutoff never falls below this fraction of the user's average
// watch time - that's what lets the threshold rise with their pace.
// Skip cutoff never falls below this fraction of the average watch time.
constexpr double kSkipShareOfWatchMs = 0.35;

// mouse wheel debounce
constexpr int kWheelDebounceMs = 250;

// Decodes raw media bytes; videos get their first frame. Off-thread.
QImage decodeBytes(const SourceImage& img, const QByteArray& bytes) {
    QImage decoded;
    if (!decoded.loadFromData(bytes)) {
        QString ext = img.format.toLower();
        if (ext.isEmpty()) {
            const int dot = img.fileUrl.lastIndexOf(QLatin1Char('.'));
            if (dot >= 0)
                ext = img.fileUrl.mid(dot + 1).toLower();
        }
        if (!ext.isEmpty()) {
            QTemporaryFile tmp(QDir::tempPath() + QLatin1String("/Mosaic-feed-XXXXXX.") + ext);
            if (tmp.open()) {
                tmp.write(bytes);
                tmp.flush();
                tmp.close();
                VideoDecoder decoder;
                if (decoder.open(tmp.fileName())) {
                    const VideoDecoder::Frame frame = decoder.nextFrame();
                    decoded = frame.image;
                }
            }
        }
    }
    return decoded;
}

}  // namespace

FeedPage::FeedPage(SourceManager& sources, const AppState& state, QWidget* parent)
    : QWidget(parent), m_sources(sources) {
    m_media = new MediaCache(this);
    m_media->setMaxEntries(24);
    m_db = new FeedDatabase();
    m_db->open();
    m_embeddings = new EmbeddingBackend(*m_db, m_sources);
    m_embeddings->initialize();
    m_recommender = new FeedRecommender(*m_db, m_sources, state.feedSourceIds(), m_embeddings);
    buildUi();

    // Kick off the first batch once the page is visible.
    QTimer::singleShot(0, this, [this]() {
        requestMore();
        showNext();
    });
}

FeedPage::~FeedPage() {
    shutdown();
    delete m_recommender;
    delete m_embeddings;
    delete m_db;
}

void FeedPage::shutdown() {
    // Idempotent; pool workers' writes fail fast instead of blocking exit.
    if (m_db)
        m_db->close();
}

void FeedPage::buildUi() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Collapsible tag sidebar on the left.
    m_tagSidebar = new TagSidebar(this);
    root->addWidget(m_tagSidebar);

    connect(m_tagSidebar, &TagSidebar::tagSearchRequested, this, [this](const QString& tag) {
        const SourceImage img =
            m_position >= 0 && m_position < m_queue.size() ? m_queue.at(m_position) : SourceImage{};
        emit tagSearchRequested(tag, img.sourceId);
    });

    // Center stage: one post fills the page.
    m_stage = new QWidget(this);
    auto* stageLayout = new QVBoxLayout(m_stage);
    stageLayout->setContentsMargins(12, 12, 12, 12);

    m_mediaLabel = new QLabel(m_stage);
    m_mediaLabel->setAlignment(Qt::AlignCenter);
    m_mediaLabel->setMinimumSize(320, 320);
    m_mediaLabel->setScaledContents(false);
    stageLayout->addWidget(m_mediaLabel, 1);

    m_infoLabel = new QLabel(m_stage);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setTextFormat(Qt::RichText);
    stageLayout->addWidget(m_infoLabel);

    m_statusLabel = new QLabel(QStringLiteral("Loading recommendations..."), m_stage);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    stageLayout->addWidget(m_statusLabel);

    // Action row pinned at the bottom of the stage column.
    auto* actions = new QHBoxLayout();
    actions->setContentsMargins(0, 6, 0, 0);
    actions->setSpacing(10);

    m_likeButton = new QPushButton(QStringLiteral("Like"), this);
    m_likeButton->setIcon(assetIcon(QStringLiteral("fav.svg")));
    m_likeButton->setToolTip(QStringLiteral("Like - trains the recommender (L)"));
    m_likeButton->setFocusPolicy(Qt::NoFocus);  // buttons must not eat arrow keys

    m_prevButton = new QPushButton(QStringLiteral("Back"), this);
    m_prevButton->setIcon(assetIcon(QStringLiteral("prev.svg")));
    m_prevButton->setToolTip(QStringLiteral("Previous post (↑ / scroll up)"));
    m_prevButton->setFocusPolicy(Qt::NoFocus);

    m_dislikeButton = new QPushButton(QStringLiteral("Dislike"), this);
    m_dislikeButton->setIcon(assetIcon(QStringLiteral("close.svg")));
    m_dislikeButton->setToolTip(
        QStringLiteral("Dislike - strong negative signal, fewer posts like this (D)"));
    m_dislikeButton->setFocusPolicy(Qt::NoFocus);

    m_saveButton = new QPushButton(QStringLiteral("Save"), this);
    m_saveButton->setIcon(assetIcon(QStringLiteral("save-favorite.svg")));
    m_saveButton->setToolTip(QStringLiteral("Save to library (S) - strongest signal"));
    m_saveButton->setFocusPolicy(Qt::NoFocus);

    m_nextButton = new QPushButton(QStringLiteral("Next"), this);
    m_nextButton->setIcon(assetIcon(QStringLiteral("next.svg")));
    m_nextButton->setToolTip(QStringLiteral("Next post (Space / ↓ / scroll down)"));
    m_nextButton->setFocusPolicy(Qt::NoFocus);

    actions->addStretch(1);
    actions->addWidget(m_prevButton);  // back on the far left
    actions->addWidget(m_dislikeButton);
    actions->addWidget(m_nextButton);
    actions->addWidget(m_likeButton);
    actions->addWidget(m_saveButton);
    actions->addStretch(1);
    stageLayout->addLayout(actions);

    root->addWidget(m_stage, 1);

    connect(m_likeButton, &QPushButton::clicked, this, [this]() {
        onLikeClicked();
        advance(false);  // like already recorded its own signal
        setFocus();      // keep arrow keys working after clicking a button
    });
    connect(m_dislikeButton, &QPushButton::clicked, this, [this]() {
        onDislikeClicked();
        advance(false);  // dislike already recorded its own signal
        setFocus();
    });
    connect(m_saveButton, &QPushButton::clicked, this, [this]() {
        // Saving is the strongest positive signal: favorite + advance too.
        onSaveClicked();
        recordCurrent(FeedDatabase::Event::Favorite);
        advance(false);
        setFocus();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this]() {
        advance();
        setFocus();
    });
    connect(m_prevButton, &QPushButton::clicked, this, [this]() {
        goBack();
        setFocus();
    });

    // Debug sidebar on the right (F12).
    m_debugButton = new QPushButton(this);
    m_debugButton->setIcon(assetIcon(QStringLiteral("bug.svg")));
    m_debugButton->setToolTip(QStringLiteral("Toggle recommendation debug panel (F12)"));
    m_debugButton->setFixedWidth(36);
    m_debugButton->setFocusPolicy(Qt::NoFocus);
    actions->addWidget(m_debugButton);
    connect(m_debugButton, &QPushButton::clicked, this, [this]() {
        toggleDebug();
        setFocus();
    });

    m_debugPanel = new QWidget(this);
    m_debugPanel->setFixedWidth(320);
    auto* debugLayout = new QVBoxLayout(m_debugPanel);
    debugLayout->setContentsMargins(6, 6, 6, 6);
    m_debugText = new QTextBrowser(m_debugPanel);
    debugLayout->addWidget(m_debugText, 1);

    // Profile resets: soft re-learns from the user's library favorites,
    // hard wipes everything for a truly fresh start.
    auto* resetRow = new QHBoxLayout();
    resetRow->setSpacing(6);
    m_softResetButton = new QPushButton(QStringLiteral("Soft reset"), m_debugPanel);
    m_softResetButton->setToolTip(
        QStringLiteral("Wipe the learned profile, then rebuild it from your library favorites"));
    m_hardResetButton = new QPushButton(QStringLiteral("Hard reset"), m_debugPanel);
    m_hardResetButton->setToolTip(
        QStringLiteral("Wipe the entire feed profile - fresh start with no preferences"));
    resetRow->addWidget(m_softResetButton);
    resetRow->addWidget(m_hardResetButton);
    debugLayout->addLayout(resetRow);

    connect(m_softResetButton, &QPushButton::clicked, this, &FeedPage::onSoftReset);
    connect(m_hardResetButton, &QPushButton::clicked, this, &FeedPage::onHardReset);

    m_debugPanel->hide();
    root->addWidget(m_debugPanel);

    // Click anywhere to restore keyboard focus.
    setFocusPolicy(Qt::StrongFocus);
    m_stage->installEventFilter(this);
}

bool FeedPage::eventFilter(QObject* watched, QEvent* event) {
    // Click anywhere to restore keyboard focus; double-click likes + advances.
    if (event->type() == QEvent::MouseButtonPress && watched != m_tagSidebar) {
        setFocus();
    }
    if (event->type() == QEvent::MouseButtonDblClick && watched == m_stage) {
        onLikeClicked();
        advance(false);
        return true;
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        const int dy = wheel->angleDelta().y();
        if (dy == 0)
            return true;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastWheelMs != 0 &&
            (now - m_lastWheelMs < kWheelDebounceMs || (dy > 0 ? -dy : dy) >= 120)) {
            m_lastWheelMs = now;
            return true;
        }
        m_lastWheelMs = now;
        if (dy < 0) {
            advance();
            return true;
        }
        goBack();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void FeedPage::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    switch (event->key()) {
        case Qt::Key_Space:
        case Qt::Key_Right:
            advance();
            break;
        case Qt::Key_Left:
        case Qt::Key_Backspace:
            goBack();
            break;
        case Qt::Key_Up:
            if (!(event->modifiers() & Qt::ControlModifier)) {
                // break;
            }
        case Qt::Key_L:
            onLikeClicked();
            advance(false);
            break;
        case Qt::Key_Down:
            if (!(event->modifiers() & Qt::ControlModifier)) {
                // break;
            }
        case Qt::Key_D:
            onDislikeClicked();
            advance(false);
            break;
        case Qt::Key_S:
            if (event->modifiers() & Qt::ControlModifier) {
                onSaveClicked();
                recordCurrent(FeedDatabase::Event::Favorite);
                advance(false);
            }
            break;
        case Qt::Key_F12:
            toggleDebug();
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

// feed flow

void FeedPage::requestMore() {
    if (m_fetching)
        return;
    m_fetching = true;
    m_statusLabel->setText(QStringLiteral("Finding posts you might like…"));

    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([self]() {
        QList<SourceImage> batch;
        QString error;
        if (self) {
            batch = self->m_recommender->nextBatch(kBatchSize);
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, batch]() {
                if (!self)
                    return;
                self->m_fetching = false;
                int droppedSeen = 0, droppedNoUrl = 0;
                for (const SourceImage& img : batch) {
                    if (self->m_seenMd5s.contains(img.md5)) {
                        ++droppedSeen;
                        continue;
                    }
                    if (!img.isLocal() && img.fileUrl.isEmpty() && img.sampleUrl.isEmpty() &&
                        img.previewUrl.isEmpty()) {
                        ++droppedNoUrl;
                        continue;
                    }
                    self->m_seenMd5s.insert(img.md5);
                    self->m_queue.append(img);
                }
                qDebug().noquote()
                    << "FeedPage: batch" << batch.size() << "-> queue" << self->m_queue.size()
                    << "(dropped: seen" << droppedSeen << "noUrl" << droppedNoUrl << ")";
                if (self->m_queue.isEmpty()) {
                    self->m_statusLabel->setText(
                        QStringLiteral("No recommendations yet - favorite some posts or add "
                                       "source tabs first."));
                } else {
                    self->m_statusLabel->clear();
                }
                // A keypress that arrived while the batch was loading is
                // still pending: honor it now that new posts exist.
                if (self->m_awaitingAdvance && !self->m_queue.isEmpty()) {
                    self->m_awaitingAdvance = false;
                    self->showNext();
                }
            },
            Qt::QueuedConnection);
    });
}

void FeedPage::goBack() {
    if (m_position <= 0)
        return;
    --m_position;
    showPost(m_queue.at(m_position));
}

void FeedPage::showNext() {
    if (m_position + 1 >= m_queue.size()) {
        m_awaitingAdvance = true;
        requestMore();
        return;
    }
    m_awaitingAdvance = false;
    ++m_position;
    showPost(m_queue.at(m_position));

    // Prefetch well before the end so fast swipers never see "Loading…".
    if (m_queue.size() - m_position <= 16)
        requestMore();

    preloadNext();
}

void FeedPage::preloadNext() {
    // Prefetch the next couple of entries (preview + full bytes).
    for (int offset = 1; offset <= 2; ++offset) {
        const int next = m_position + offset;
        if (next < 0 || next >= m_queue.size())
            continue;
        const SourceImage nextImg = m_queue.at(next);
        if (nextImg.md5.isEmpty())
            continue;
        if (!nextImg.isLocal() && nextImg.fileUrl.isEmpty() && nextImg.sampleUrl.isEmpty() &&
            nextImg.previewUrl.isEmpty())
            continue;

        Source* source =
            m_sources.source(nextImg.sourceId.isEmpty() ? m_sources.remoteSources().value(0)->id()
                                                        : nextImg.sourceId);
        if (!source)
            continue;

        m_media->fetchIntoCache(source, nextImg, FileKind::Preview);
        m_media->fetchIntoCache(source, nextImg, FileKind::Full);
    }
}

void FeedPage::showPost(const SourceImage& image) {
    // Dwell timing starts when the media is visible, not when the slot opens.
    m_shownAt = 0;
    m_mediaLoaded = false;

    // Impression: marks the post as seen; carries no taste signal.
    if (!image.md5.isEmpty() && !m_interactedMd5s.contains(image.md5)) {
        QStringList tags;
        for (const SourceTag& t : image.tags) tags << t.name;
        FeedDatabase* db = m_db;
        const SourceImage img = image;
        const int pos = m_position;
        QThreadPool::globalInstance()->start([db, img, tags, pos]() {
            db->recordEvent(FeedDatabase::Event::Impression, img.sourceId, img.md5, tags, 0.0, pos);
        });
    }

    m_tagSidebar->setImage(image, /*editable=*/false);
    updateDebugPanel();

    m_infoLabel->setText(
        QStringLiteral("<b>%1</b> · %2 · ★%3")
            .arg(image.md5.left(12), image.format.isEmpty() ? QStringLiteral("?") : image.format)
            .arg(image.score));
    m_mediaLabel->setText(QStringLiteral("…"));
    m_mediaLabel->setPixmap(QPixmap());

    // Posts with no usable URL can never load - fail fast.
    if (!image.isLocal() && image.fileUrl.isEmpty() && image.sampleUrl.isEmpty() &&
        image.previewUrl.isEmpty()) {
        skipFailedPost(QStringLiteral("no media URL"));
        return;
    }

    // Full-quality media off-thread (network fetch + decode); videos get
    // their first frame rendered at full resolution.
    Source* source = m_sources.source(
        image.sourceId.isEmpty() ? m_sources.remoteSources().value(0)->id() : image.sourceId);
    if (!source) {
        skipFailedPost(QStringLiteral("no source"));
        return;
    }

    m_statusLabel->setText(QStringLiteral("Loading…"));

    // "Love" instead of "Save" when the post is already in the library.
    // The catalog lookup opens a DuckDB connection, so it runs off-thread;
    // default to Save and correct the label when the lookup answers.
    m_saveButton->setText(QStringLiteral("Save"));
    m_saveButton->setIcon(assetIcon(QStringLiteral("save-favorite.svg")));
    m_saveButton->setToolTip(QStringLiteral("Save to library (S) - strongest signal"));
    if (!image.localPath.isEmpty()) {
        markLoved();
    } else {
        const QString md5 = image.md5;
        QPointer<FeedPage> self(this);
        QThreadPool::globalInstance()->start([self, md5]() {
            LibrarySource library;
            const bool inLibrary = library.hasImage(md5);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, inLibrary]() {
                    if (inLibrary)
                        self->markLoved();
                },
                Qt::QueuedConnection);
        });
    }

    QPointer<FeedPage> self(this);
    const SourceImage img = image;
    m_mediaIndex = m_position;

    // Decoding a full-res image (or a video first frame) takes tens to
    // hundreds of ms - do it off-thread, render on the GUI thread.
    auto decodeOffThread = [](const SourceImage& post, const QByteArray& bytes,
                              bool allowSkipOnFailure, QPointer<FeedPage> self) {
        QThreadPool::globalInstance()->start([self, post, bytes, allowSkipOnFailure]() {
            const QImage frame = decodeBytes(post, bytes);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, post, frame, allowSkipOnFailure]() {
                    if (!self)
                        return;
                    if (frame.isNull()) {
                        if (!self->m_mediaLoaded && allowSkipOnFailure)
                            self->skipFailedPost(QStringLiteral("decode failed"));
                        return;
                    }
                    if (self->m_position < 0 || self->m_position >= self->m_queue.size())
                        return;
                    if (self->m_queue.at(self->m_position).md5 != post.md5)
                        return;  // user moved on
                    if (allowSkipOnFailure == false && self->m_mediaLoaded)
                        return;  // full quality already shown
                    self->applyDecodedMedia(post, frame);
                },
                Qt::QueuedConnection);
        });
    };

    // Shared progressive load (same as ViewerWindow): cached full bytes are
    // delivered synchronously; otherwise the preview shows immediately and
    // the full bytes upgrade it when they arrive.
    const bool instant = m_media->request(
        source, img,
        /*onPreview=*/
        [this, img, decodeOffThread](const QByteArray& bytes) {
            if (m_mediaLoaded)
                return;  // something better is already on screen
            if (m_position < 0 || m_position >= m_queue.size() ||
                m_queue.at(m_position).md5 != img.md5)
                return;  // user moved on
            decodeOffThread(img, bytes, /*allowSkipOnFailure=*/false, this);
        },
        /*onFull=*/
        [this, img, decodeOffThread](const QByteArray& bytes) {
            if (m_position < 0 || m_position >= m_queue.size())
                return;
            if (m_queue.at(m_position).md5 != img.md5)
                return;  // user moved on
            decodeOffThread(img, bytes, /*allowSkipOnFailure=*/true, this);
        });

    if (instant) {
        m_statusLabel->clear();
        return;
    }

    // If nothing is cached and nothing is in flight, the stage would stay
    // blank forever - MediaCache.request always starts a full fetch unless
    // one is running, so reaching here means we just wait for its callback.
    if (!m_media->inFlight(image.md5, FileKind::Full) &&
        !m_media->inFlight(image.md5, FileKind::Preview))
        skipFailedPost(QStringLiteral("no media available"));

    // Linger-time training happens in advance(): the post's total dwell
    // time is known only when the user moves on.
}

void FeedPage::markLoved() {
    m_saveButton->setText(QStringLiteral("Love"));
    m_saveButton->setIcon(assetIcon(QStringLiteral("love.svg")));
    m_saveButton->setToolTip(QStringLiteral("Already in your library (S) - love it"));
}

void FeedPage::applyDecodedMedia(const SourceImage& img, const QImage& decoded) {
    // Media is on screen: only now does dwell time count.
    m_mediaLoaded = true;
    m_consecutiveFailures = 0;
    m_shownAt = QDateTime::currentMSecsSinceEpoch();
    if (m_sessionStartMs == 0)
        m_sessionStartMs = m_shownAt;
    ++m_sessionPosts;

    // Fit to the available stage size, keep aspect ratio.
    const QSize avail = m_mediaLabel->size();
    m_mediaLabel->setPixmap(
        QPixmap::fromImage(decoded.scaled(avail, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    QStringList tags;
    for (const SourceTag& t : img.tags) tags << t.name.toHtmlEscaped();
    m_infoLabel->setText(
        QStringLiteral("<b>%1</b> · %2 · ★%3")
            .arg(img.md5.left(12), img.format.isEmpty() ? QStringLiteral("?") : img.format)
            .arg(img.score));
    m_statusLabel->clear();
}

void FeedPage::hideEvent(QHideEvent* event) {
    // Tab switch voids this post's dwell time.
    m_shownAt = 0;
    QWidget::hideEvent(event);
}

void FeedPage::showEvent(QShowEvent* event) {
    // Dwell timer restarts from 0 s: if media is already on screen, start
    // counting now; otherwise applyDecodedMedia() starts it on render.
    if (m_mediaLoaded)
        m_shownAt = QDateTime::currentMSecsSinceEpoch();
    QWidget::showEvent(event);
}

void FeedPage::skipFailedPost(const QString& reason) {
    ++m_consecutiveFailures;

    // Backoff so failures don't burn through the queue in a tight loop.
    const int delayMs =
        m_consecutiveFailures > 1 ? qMin(30000, 500 << qMin(6, m_consecutiveFailures - 2)) : 0;
    m_statusLabel->setText(
        QStringLiteral("Skipping post (%1)%2")
            .arg(reason,
                 delayMs ? QStringLiteral(" - waiting %1s").arg(delayMs / 1000) : QString()));

    QPointer<FeedPage> self(this);
    QTimer::singleShot(delayMs, this, [this, self]() {
        if (!self)
            return;
        // Drop the failed post from the queue and show the next one.
        if (m_position >= 0 && m_position < m_queue.size())
            m_queue.removeAt(m_position);
        --m_position;  // showNext() increments it back
        showNext();
    });
}

void FeedPage::recordCurrent(FeedDatabase::Event event) {
    if (!m_mediaLoaded || m_position < 0 || m_position >= m_queue.size())
        return;
    const SourceImage& img = m_queue.at(m_position);
    // One training signal per post, ever.
    if (m_interactedMd5s.contains(img.md5))
        return;
    m_interactedMd5s.insert(img.md5);
    QStringList tags;
    for (const SourceTag& t : img.tags) tags << t.name;
    const double dwellSecs =
        m_shownAt > 0 ? (QDateTime::currentMSecsSinceEpoch() - m_shownAt) / 1000.0 : 0.0;
    const qint64 dwell = m_shownAt > 0 ? QDateTime::currentMSecsSinceEpoch() - m_shownAt : 0;

    // Training writes run off the GUI thread (DB write lock).
    FeedDatabase* db = m_db;
    QThreadPool::globalInstance()->start([db, event, img, tags, dwellSecs, dwell]() {
        db->recordEvent(event, img.sourceId, img.md5, tags, dwellSecs, -1);
        if (dwell > 0 && dwell < kAbandonMs)
            db->recordLinger(QStringLiteral("liked"), dwell);
    });
}

void FeedPage::advance(bool train) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (train && m_mediaLoaded && m_position >= 0 && m_position < m_queue.size() && m_shownAt > 0 &&
        !m_interactedMd5s.contains(m_queue.at(m_position).md5)) {
        // Mark before the async write so a fast double-swipe can't double-train.
        m_interactedMd5s.insert(m_queue.at(m_position).md5);
        const qint64 dwell = now - m_shownAt;
        const SourceImage img = m_queue.at(m_position);
        QStringList tags;
        for (const SourceTag& t : img.tags) tags << t.name;

        // Training writes happen off the GUI thread: recordEvent runs
        // several SQL updates plus a history prune.
        FeedDatabase* db = m_db;
        QThreadPool::globalInstance()->start([db, img, tags, dwell]() {
            // Adaptive thresholds from this user's linger history.
            const QHash<QString, qint64> lingers = db->averageLingers();
            const qint64 avgWatchedRaw = lingers.value(QStringLiteral("watched"), kFullWatchMs);
            const qint64 skippedAvg = lingers.value(QStringLiteral("skipped"), kSkipMs);
            const qint64 paceFloor = kSkipShareOfWatchMs * avgWatchedRaw;
            const qint64 skipCandidate = qMax(skippedAvg, paceFloor);
            const qint64 skipMs = qBound(kSkipFloorMs, skipCandidate, kSkipCeilingMs);

            const double dwellSecs = dwell / 1000.0;
            if (dwell >= kAbandonMs) {
                // A minute-long "watch" is absence, not engagement.
            } else if (dwell < skipMs) {
                // A skip is a weak negative, not a dislike.
                db->recordEvent(FeedDatabase::Event::Skip, img.sourceId, img.md5, tags, dwellSecs);
                db->recordLinger(QStringLiteral("skipped"), dwell);
            } else {
                db->recordEvent(FeedDatabase::Event::View, img.sourceId, img.md5, tags, dwellSecs);
                db->recordLinger(QStringLiteral("watched"), dwell);
            }
        });
    }

    showNext();
}

// actions

void FeedPage::seedFromFavorites() {
    // Train the (fresh) model on every tag of every library favorite.
    LibrarySource library;
    SourceQuery favQuery;
    favQuery.tags = QStringLiteral("usergroup:favorite");
    favQuery.limit = 200;
    const SourceResult favs = library.search(favQuery);
    if (!favs.ok)
        return;

    struct SeedJob {
        QString sourceId;
        QString md5;
        QStringList tags;
    };
    QList<SeedJob> jobs;
    for (const SourceImage& img : favs.images) {
        QStringList tags;
        for (const SourceTag& t : img.tags) {
            if (t.category == QLatin1String("usergroup") || FeedRecommender::isGenericTag(t.name))
                continue;
            tags << t.name;
        }
        if (!tags.isEmpty())
            jobs.append({img.sourceId, img.md5, tags});
    }
    if (jobs.isEmpty())
        return;

    FeedDatabase* db = m_db;
    QThreadPool::globalInstance()->start([db, jobs]() {
        for (const SeedJob& job : jobs)
            db->recordEvent(FeedDatabase::Event::Favorite, job.sourceId, job.md5, job.tags);
    });
}

void FeedPage::onSoftReset() {
    m_db->reset();
    seedFromFavorites();
    m_queue.clear();
    m_seenMd5s.clear();
    m_position = -1;
    m_statusLabel->setText(QStringLiteral("Profile rebuilt from your library favorites."));
    updateDebugPanel();
    requestMore();
    showNext();
}

void FeedPage::onHardReset() {
    m_db->reset();
    m_queue.clear();
    m_seenMd5s.clear();
    m_position = -1;
    m_statusLabel->setText(QStringLiteral("Profile wiped - starting fresh."));
    updateDebugPanel();
    requestMore();
    showNext();
}

void FeedPage::onLikeClicked() {
    recordCurrent(FeedDatabase::Event::Like);
    m_likeButton->setText(QStringLiteral("Liked!"));
    QTimer::singleShot(1200, this, [this]() { m_likeButton->setText(QStringLiteral("Like")); });
}

void FeedPage::toggleDebug() {
    m_debugPanel->setVisible(!m_debugPanel->isVisible());
    if (m_debugPanel->isVisible())
        updateDebugPanel();
}

void FeedPage::updateDebugPanel() {
    if (!m_debugPanel || !m_debugPanel->isVisible())
        return;

    // Built off-thread: even the "cheap" sections hit DuckDB.
    if (m_debugStatsPending)
        return;
    m_debugStatsPending = true;

    struct DebugSnapshot {
        int sessionPosts = 0;
        qint64 sessionMs = 0;
        int queueSize = 0;
        QList<QPair<QString, QString>> recent;
        QString currentHtml;   // pre-rendered current-post section
        QString upcomingHtml;  // pre-rendered upcoming-queue section
    };

    const int position = m_position;
    const qint64 sessionStart = m_sessionStartMs;
    const QList<SourceImage> queue = m_queue.mid(0, position + 6);
    const SourceImage current =
        (position >= 0 && position < queue.size()) ? queue.at(position) : SourceImage{};

    FeedDatabase* db = m_db;
    FeedRecommender* rec = m_recommender;
    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([self, db, rec, position, sessionStart, queue, current]() {
        DebugSnapshot snap;
        snap.queueSize = queue.size();
        snap.recent = db->recentInteractions(5);
        if (sessionStart > 0)
            snap.sessionMs = QDateTime::currentMSecsSinceEpoch() - sessionStart;
        snap.sessionPosts = qMax(0, position + 1);

        // Shared palette with the heavy-stats renderer below.
        const char* kTitle = "color:#e8a33d; margin-top:8px;";
        const char* kPos = "color:#5fbf6f";
        const char* kNeg = "color:#e05a5a";
        const char* kDim = "color:#999";

        QString html =
            QStringLiteral("<h3 style='color:#e8a33d; margin-bottom:4px;'>Feed debug</h3>");

        // session
        html += QStringLiteral("<b style='%1'>Session</b><br>").arg(kTitle);
        if (sessionStart > 0) {
            const qint64 sessionSecs = snap.sessionMs / 1000;
            html += QStringLiteral("posts: %1 · time: %2m %3s<br>")
                        .arg(snap.sessionPosts)
                        .arg(sessionSecs / 60)
                        .arg(sessionSecs % 60);
        }
        html += QStringLiteral("queue: %1 posts<br>").arg(snap.queueSize);

        if (!snap.recent.isEmpty()) {
            html += QStringLiteral("<span style='%1'>last:</span> ").arg(kDim);
            QList<QString> parts;
            for (const auto& [kind, md5] : snap.recent) {
                const char* color =
                    (kind == QLatin1String("liked") || kind == QLatin1String("saved") ||
                     kind == QLatin1String("watched"))
                        ? kPos
                        : kNeg;
                parts << QStringLiteral("<span style='%1'>%2</span> %3")
                             .arg(color)
                             .arg(kind)
                             .arg(md5.left(6));
            }
            html += parts.join(QStringLiteral(" · ")) + QStringLiteral("<br>");
        }

        // current post (score breakdown + visual verdict)
        if (position >= 0 && position < queue.size()) {
            const FeedDatabase::RecommendationRow bd = rec->explainPost(current);
            html += QStringLiteral(
                        "<br><b style='%1'>Current post</b> %2<br>"
                        "score: user %3 + session %4 + tags %5 = <b>%6</b> · taste #%7 · src "
                        "%8 · hard-neg %9<br>")
                        .arg(kTitle)
                        .arg(current.md5.left(8))
                        .arg(bd.userSimilarity, 0, 'f', 2)
                        .arg(bd.sessionSimilarity, 0, 'f', 2)
                        .arg(bd.tagScore, 0, 'f', 2)
                        .arg(bd.finalScore, 0, 'f', 2)
                        .arg(bd.tasteId)
                        .arg(bd.candidateSource.isEmpty() ? QStringLiteral("?")
                                                          : bd.candidateSource.toHtmlEscaped())
                        .arg(bd.rejectedNegative ? QStringLiteral("yes") : QStringLiteral("no"));

            // Per-tag learned ratings for THIS post (Bayesian-shrunk, same
            // math the ranker uses): negatives in red, positives in green,
            // so it is visible exactly which tags dragged the score down.
            const auto prefRows = db->tagPreferenceRows();
            QStringList negParts, posParts;
            for (const SourceTag& t : current.tags) {
                const QString tag = t.name.trimmed().toLower();
                auto it = prefRows.constFind(tag);
                if (it == prefRows.constEnd())
                    continue;
                const double w =
                    it.value().preference * (it.value().confidence / (it.value().confidence + 3.0));
                if (w < -0.05)
                    negParts << QStringLiteral("<span style='%1'>%2 (%3)</span>")
                                    .arg(kNeg, tag.toHtmlEscaped())
                                    .arg(w, 0, 'f', 2);
                else if (w > 0.05)
                    posParts << QStringLiteral("<span style='%1'>%2 (+%3)</span>")
                                    .arg(kPos, tag.toHtmlEscaped())
                                    .arg(w, 0, 'f', 2);
            }
            if (!negParts.isEmpty())
                html += QStringLiteral("<span style='%1'>disliked tags here:</span> %2<br>")
                            .arg(kDim, negParts.join(QStringLiteral(", ")));
            if (!posParts.isEmpty())
                html += QStringLiteral("<span style='%1'>liked tags here:</span> %2<br><br>")
                            .arg(kDim, posParts.join(QStringLiteral(", ")));
        }

        // upcoming queue, with the total fed count on the header line.
        // The visual-model verdict lives here too: it predicts whether the
        // user will like each upcoming post, which is the actionable
        // question (the current post is already on screen).
        html += QStringLiteral("<b style='%1'>Upcoming</b> · total fed %2<br>")
                    .arg(kTitle)
                    .arg(static_cast<qint64>(db->totalImpressions()));
        for (int i = position + 1; i < qMin(position + 6, queue.size()); ++i) {
            const FeedDatabase::RecommendationRow bd = rec->explainPost(queue.at(i));
            const QString scoreColor = bd.finalScore >= 0 ? kPos : kNeg;
            html += QStringLiteral("%1… <span style='%2'>score %3</span>")
                        .arg(queue.at(i).md5.left(8))
                        .arg(scoreColor)
                        .arg(bd.finalScore, 0, 'f', 2);
            const FeedRecommender::VisualVerdict vv = rec->explainVisual(queue.at(i));
            if (vv.available) {
                const char* verdictColor =
                    vv.bestSim >= 0.5 ? kPos : (vv.bestSim >= 0.3 ? kDim : kNeg);
                const char* verdictText =
                    vv.bestSim >= 0.5 ? "like" : (vv.bestSim >= 0.3 ? "maybe" : "drop");
                html += QStringLiteral(" · <span style='%1'>%2 %3</span>")
                            .arg(verdictColor)
                            .arg(verdictText)
                            .arg(vv.bestSim, 0, 'f', 2);
            }
            html += QStringLiteral("<br>");
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, html]() {
                if (!self)
                    return;
                self->m_debugStatsPending = false;
                if (!self->m_debugPanel || !self->m_debugPanel->isVisible())
                    return;
                // Heavy stats are cached HTML built by the heavy loader.
                QString full = html;
                const QString marker = QStringLiteral(
                    "<br><b style='color:#e8a33d; margin-top:8px;'>Totals &amp; "
                    "profile</b><br>");
                full += marker + (self->m_heavyStatsHtml.isEmpty()
                                      ? QStringLiteral("<span style='color:#999'>loading…</span>")
                                      : self->m_heavyStatsHtml);
                self->m_lastDebugHtml = full;
                self->m_debugText->setHtml(full);

                // Kick the heavy-stats refresh (it has its own 5 s throttle).
                self->refreshHeavyStats();
            },
            Qt::QueuedConnection);
    });
}

void FeedPage::refreshHeavyStats() {
    // Heavy DB queries - off-thread, refreshed at most every 5 s. When the
    // results land, only the cached heavy HTML is replaced; the rest of the
    // panel is not re-rendered (that would re-query everything).
    if (m_heavyStatsPending || QDateTime::currentMSecsSinceEpoch() - m_lastHeavyStatsLoad < 5000)
        return;
    m_heavyStatsPending = true;

    struct HeavyStats {
        QHash<QString, qint64> lingers;
        QHash<QString, int> interactions;
        QStringList tasteSummary;
        QList<QPair<QString, double>> likedTags;
        QList<QPair<QString, double>> dislikedTags;
        int tasteCount = 0;
        int hardNegatives = 0;
        QStringList hardNegativeTags;
        int skipStreak = 0;
        double sessionBlend = 0.0;
        int negativeAnchors = 0;
        QStringList lowSpecificityTags;
        double learnedSamples = 0.0;
        double learnedBlend = 0.0;
        QList<double> learnedWeights;
        FeedRecommender::FunnelStats funnel;
    };

    FeedDatabase* db = m_db;
    FeedRecommender* rec = m_recommender;
    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([db, rec, self]() {
        HeavyStats stats;
        stats.lingers = db->averageLingers();
        stats.interactions = db->interactionCounts();
        {
            const FeedRecommender::ModelStats ms = rec->modelStats();
            stats.tasteSummary = ms.tasteSummary;
            stats.likedTags = ms.likedTags;
            stats.dislikedTags = ms.dislikedTags;
            stats.tasteCount = ms.tasteCount;
            stats.hardNegatives = ms.hardNegatives;
            stats.hardNegativeTags = ms.hardNegativeTags;
            stats.skipStreak = ms.skipStreak;
            stats.sessionBlend = ms.sessionBlend;
            stats.negativeAnchors = ms.negativeAnchors;
            stats.lowSpecificityTags = ms.lowSpecificityTags;
            stats.learnedSamples = ms.learnedSamples;
            stats.learnedBlend = ms.learnedBlend;
            stats.learnedWeights = ms.learnedWeights;
        }
        stats.funnel = rec->lastFunnelStats();

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, stats]() {
                if (!self)
                    return;
                self->m_heavyStatsPending = false;
                self->m_lastHeavyStatsLoad = QDateTime::currentMSecsSinceEpoch();

                QString h;
                // Interactions, compact: positives on one line, negatives on
                // the next, then skip streak / session blend.
                const QStringList kinds = {QStringLiteral("favorite"), QStringLiteral("like"),
                                           QStringLiteral("view"), QStringLiteral("skip"),
                                           QStringLiteral("dislike")};
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Interactions"
                    "</b><br>");
                auto countOf = [&stats](const QString& k) {
                    return stats.interactions.value(k, 0);
                };
                h += QStringLiteral(
                         "<span style='color:#5fbf6f'>%1/%2/%3</span> "
                         "<span style='color:#999'>fav/like/view</span><br>")
                         .arg(countOf(QStringLiteral("favorite")))
                         .arg(countOf(QStringLiteral("like")))
                         .arg(countOf(QStringLiteral("view")));
                h += QStringLiteral(
                         "<span style='color:#e05a5a'>%1/%2</span> "
                         "<span style='color:#999'>skip/dislike</span><br>")
                         .arg(countOf(QStringLiteral("skip")))
                         .arg(countOf(QStringLiteral("dislike")));
                h += QStringLiteral(
                         "<span style='color:#999'>skip streak: %1 · session blend: %2%</span>"
                         "<br>")
                         .arg(stats.skipStreak)
                         .arg(stats.sessionBlend * 100.0, 0, 'f', 0);

                // Avg watch time per kind (empty until the first dwell is
                // recorded - show the placeholder instead of hiding it).
                {
                    QStringList parts;
                    for (const QString& k : kinds) {
                        if (!stats.lingers.contains(k))
                            continue;
                        parts << QStringLiteral("%1 %2s").arg(k).arg(
                            stats.lingers.value(k) / 1000.0, 0, 'f', 1);
                    }
                    h += QStringLiteral("<span style='color:#999'>avg watch: %1</span><br>")
                             .arg(parts.isEmpty() ? QStringLiteral("(no data yet)")
                                                  : parts.join(QStringLiteral(", ")));
                }

                // Taste profiles, capped at 12 lines.
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Taste profiles</b><br>");
                if (stats.tasteSummary.isEmpty()) {
                    h += QStringLiteral("<span style='color:#999'>(none learned yet)</span><br>");
                } else {
                    const int shown = qMin(12, stats.tasteSummary.size());
                    for (int i = 0; i < shown; ++i)
                        h += stats.tasteSummary.at(i).toHtmlEscaped() + QStringLiteral("<br>");
                    if (stats.tasteSummary.size() > shown)
                        h += QStringLiteral("<span style='color:#999'>… +%1 more</span><br>")
                                 .arg(stats.tasteSummary.size() - shown);
                }
                if (!stats.likedTags.isEmpty()) {
                    h += QStringLiteral("<b style='color:#5fbf6f;'>Top 10 liked tags</b><br>");
                    QStringList items;
                    for (const auto& pair : stats.likedTags) {
                        items << QStringLiteral("%1 (+%2)")
                                     .arg(pair.first)
                                     .arg(pair.second, 0, 'f', 2);
                    }
                    h += QStringLiteral("<span style='color:#5fbf6f'>%1</span><br>")
                             .arg(items.join(QStringLiteral(", ")));
                }
                if (!stats.dislikedTags.isEmpty()) {
                    h +=
                        QStringLiteral("<b style='color:#e05a5a;'>Bottom 10 disliked tags</b><br>");
                    QStringList items;
                    for (const auto& pair : stats.dislikedTags) {
                        items << QStringLiteral("%1 (%2)")
                                     .arg(pair.first)
                                     .arg(pair.second, 0, 'f', 2);
                    }
                    h += QStringLiteral("<span style='color:#e05a5a'>%1</span><br>")
                             .arg(items.join(QStringLiteral(", ")));
                }
                // Name the hard negatives explicitly - a count alone hides
                // which tags are being vetoed from queries and filtering.
                h += QStringLiteral(
                         "<span style='color:#999'>strong negatives: %1 · negative visual "
                         "anchors: %2</span><br>")
                         .arg(stats.hardNegatives)
                         .arg(stats.negativeAnchors);
                if (!stats.hardNegativeTags.isEmpty())
                    h += QStringLiteral("<span style='color:#e05a5a'>hard negatives: %1</span><br>")
                             .arg(stats.hardNegativeTags.join(QStringLiteral(", ")));

                // self-learned tag specificity
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Tag specificity</b><br>");
                if (stats.lowSpecificityTags.isEmpty()) {
                    h +=
                        QStringLiteral("<span style='color:#999'>(not enough data yet)</span><br>");
                } else {
                    h += QStringLiteral(
                             "<span style='color:#999'>near-universal (auto-detected, no "
                             "blacklist): %1</span><br>")
                             .arg(stats.lowSpecificityTags.join(QStringLiteral(", ")));
                }

                // stage-2 learned user model
                h += QStringLiteral(
                         "<b style='color:#e8a33d; margin-top:8px;'>Learned model</b><br>"
                         "<span style='color:#999'>samples: %1 · blend: %2% learned</span><br>")
                         .arg(stats.learnedSamples, 0, 'f', 0)
                         .arg(stats.learnedBlend * 100.0, 0, 'f', 0);
                if (stats.learnedWeights.size() == 10) {
                    // Feature names in UserModel::kFeatureCount order (bias last).
                    static const char* kFeatureNames[] = {"userSim", "session", "tags",    "favOv",
                                                          "fresh",   "novel",   "explore", "populr",
                                                          "negAnch", "bias"};
                    QStringList items;
                    for (int i = 0; i < 10; ++i) {
                        const double w = stats.learnedWeights.at(i);
                        const char* color = w >= 0 ? "#5fbf6f" : "#e05a5a";
                        items << QStringLiteral("<span style='color:%3'>%1 %2</span>")
                                     .arg(QString::fromLatin1(kFeatureNames[i]))
                                     .arg(w, 0, 'f', 2)
                                     .arg(QLatin1String(color));
                    }
                    h += items.join(QStringLiteral(", ")) + QStringLiteral("<br>");
                }

                // last funnel run - one line per stage, counts in -> out,
                // with every drop reason broken out per stage.
                const auto& f = stats.funnel;
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Last funnel "
                    "run</b><br>");
                const int stage1Drops = f.dropShown + f.dropDup + f.dropAnimated + f.dropScore +
                                        f.dropHardNeg + f.dropNoMd5;
                h += QStringLiteral(
                         "<b>stage 1</b> fetch + dedup + cheap filter: %1 kept · %2 dropped<br>")
                         .arg(f.stage1Filtered)
                         .arg(stage1Drops);
                h += QStringLiteral(
                         "<span style='color:#999'>shown %1 · dup %2 · animated %3 · lowScore "
                         "%4 · hardNeg %5 · noMd5 %6</span><br>")
                         .arg(f.dropShown)
                         .arg(f.dropDup)
                         .arg(f.dropAnimated)
                         .arg(f.dropScore)
                         .arg(f.dropHardNeg)
                         .arg(f.dropNoMd5);
                if (f.coldStart) {
                    h += QStringLiteral(
                        "<b>stage 2+3</b> <span style='color:#999'>skipped (cold start - "
                        "no tastes learned yet)</span><br>");
                } else {
                    const int stage2Dropped = f.stage1Filtered - f.stage2Shortlist;
                    const int stage3Dropped = f.stage2Shortlist - f.stage3Finalists;
                    h += QStringLiteral(
                             "<b>stage 2</b> taste-vector filter: %1 kept · %2 dropped<br>")
                             .arg(f.stage2Shortlist)
                             .arg(stage2Dropped);
                    h += QStringLiteral(
                             "<b>stage 3</b> visual ranking: %1 kept · %2 dropped · new "
                             "embeddings: %3<br>")
                             .arg(f.stage3Finalists)
                             .arg(stage3Dropped)
                             .arg(f.embeddingsComputedThisBatch);
                }
                h += QStringLiteral("<b>batch</b> %1 selected · cold start: %2%3<br>")
                         .arg(f.batchSize)
                         .arg(f.coldStart ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(f.emergencyFallbackUsed
                                  ? QStringLiteral(
                                        " · <span style='color:#e05a5a'>EMERGENCY FALLBACK "
                                        "USED - funnel produced 0 results</span>")
                                  : QString());

                self->m_heavyStatsHtml = h;
                // Replace only the heavy section in the already-rendered
                // panel: re-running updateDebugPanel() would re-query
                // everything (and recurse), so splice the fresh heavy part
                // into the cached tier-1 html instead - zero DB access.
                if (self->m_debugPanel && self->m_debugPanel->isVisible()) {
                    QString full = self->m_lastDebugHtml;
                    const QString marker = QStringLiteral(
                        "<br><b style='color:#e8a33d; margin-top:8px;'>Totals "
                        "&amp; profile</b><br>");
                    const int cut = full.indexOf(marker);
                    if (cut >= 0)
                        full.truncate(cut);
                    full += marker + h;
                    self->m_debugText->setHtml(full);
                }
            },
            Qt::QueuedConnection);
    });
}

void FeedPage::onDislikeClicked() {
    if (m_position < 0 || m_position >= m_queue.size())
        return;
    const SourceImage& img = m_queue.at(m_position);
    // One dislike per post: going back and disliking again must not
    // stack another -4.0 signal onto every tag.
    if (m_interactedMd5s.contains(img.md5))
        return;
    m_interactedMd5s.insert(img.md5);
    QStringList tags;
    for (const SourceTag& t : img.tags) tags << t.name;
    const qint64 dwell =
        m_mediaLoaded && m_shownAt > 0 ? QDateTime::currentMSecsSinceEpoch() - m_shownAt : 0;

    // Explicit thumbs down: strong negative signal for every tag of the
    // post. OFF the GUI thread - same lock-contention reason as
    // recordCurrent() above.
    FeedDatabase* db = m_db;
    const SourceImage imgCopy = img;
    QThreadPool::globalInstance()->start([db, imgCopy, tags, dwell]() {
        db->recordEvent(FeedDatabase::Event::Dislike, imgCopy.sourceId, imgCopy.md5, tags);
        if (dwell > 0 && dwell < kAbandonMs)
            db->recordLinger(QStringLiteral("disliked"), dwell);
    });

    m_dislikeButton->setText(QStringLiteral("Disliked"));
    QTimer::singleShot(1200, this,
                       [this]() { m_dislikeButton->setText(QStringLiteral("Dislike")); });
}

void FeedPage::onSaveClicked() {
    if (m_position < 0 || m_position >= m_queue.size())
        return;
    const SourceImage img = m_queue.at(m_position);
    QStringList tags;
    for (const SourceTag& t : img.tags) tags << t.name;

    // Strongest training signal + actual library import, off-thread.
    recordCurrent(FeedDatabase::Event::Favorite);
    m_saveButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("Saving…"));

    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([self, img]() {
        // Download from the post's REMOTE source - LibrarySource only reads
        // files already in the library, so fetching through it would fail.
        Source* remote =
            self ? self->m_sources.source(self->m_sources.remoteSources().isEmpty()
                                              ? QString()
                                              : self->m_sources.remoteSources().first()->id())
                 : nullptr;
        const QString sourceId = img.sourceId.isEmpty() && remote ? remote->id() : img.sourceId;
        Source* downloadSource = self ? self->m_sources.source(sourceId) : nullptr;

        QByteArray bytes;
        QString error;
        if (downloadSource) {
            bytes = downloadSource->fetch(img, FileKind::Full, &error);
        } else {
            error = QStringLiteral("source not available");
        }

        ImportResult result;
        if (bytes.isEmpty()) {
            result.error = error;
        } else {
            // Import the downloaded bytes into the local library catalog.
            LibrarySource library;
            result = library.importImage(img, bytes);
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, result]() {
                if (!self)
                    return;
                self->m_saveButton->setEnabled(true);
                self->m_statusLabel->setText(
                    result.ok ? QStringLiteral("Saved to library")
                              : QStringLiteral("Save failed: %1").arg(result.error));
            },
            Qt::QueuedConnection);
    });
}

}  // namespace Mosaic
