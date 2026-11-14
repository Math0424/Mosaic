#include "FeedPage.h"

#include "FeedDatabase.h"
#include "FeedEngine.h"
#include "FeedRecommender.h"
#include "FeedWarmStart.h"
#include "VisualFeatures.h"
#include "../core/AppState.h"
#include "../core/Paths.h"
#include "../core/ThreadPools.h"
#include "../components/FlowLayout.h"
#include "../components/Icons.h"
#include "../components/MediaPlaybackController.h"
#include "../components/SourceBrowser.h"
#include "../components/TagSidebar.h"
#include "../components/ViewerWindow.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QApplication>
#include <QCompleter>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QtConcurrentRun>
#include <QDebug>
#include <QLoggingCategory>
Q_DECLARE_LOGGING_CATEGORY(lcFeed)
#include <QStringListModel>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QToolTip>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "../media/VideoDecoder.h"

namespace Mosaic {

namespace {

constexpr int kBatchSize = 15;

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

QString formatClock(qint64 ms) {
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// Formats that may hold more than one frame. Plain .png only counts when
// the post is tagged animated (APNG), so ordinary PNGs skip the decoder.
bool mayBeAnimated(const SourceImage& img, QString* extOut) {
    QString ext = img.format.toLower();
    if (ext.isEmpty()) {
        const int dot = img.fileUrl.lastIndexOf(QLatin1Char('.'));
        if (dot >= 0)
            ext = img.fileUrl.mid(dot + 1).toLower();
    }
    if (extOut)
        *extOut = ext;
    static const QSet<QString> animated = {
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("webm"),
        QStringLiteral("mp4"), QStringLiteral("mov"),  QStringLiteral("mkv"),
        QStringLiteral("m4v"), QStringLiteral("avi"),  QStringLiteral("apng"),
    };
    if (animated.contains(ext))
        return true;
    if (ext == QLatin1String("png")) {
        for (const SourceTag& t : img.tags) {
            if (t.name == QLatin1String("animated"))
                return true;
        }
    }
    return false;
}

// Display name of the site a post came from. Posts carry the id of the
// member source that supplied them, even inside a merged "All sources" tab.
QString sourceLabel(SourceManager& sources, const SourceImage& image) {
    Source* source = sources.source(image.sourceId);
    return source ? source->displayName() : QString();
}

}  // namespace

FeedPage::FeedPage(SourceManager& sources, std::shared_ptr<FeedEngine> engine,
                   const AppState& state, QWidget* parent)
    : QWidget(parent), m_sources(sources), m_engine(std::move(engine)) {
    // One shuffle seed per launch: the liked-posts walk starts somewhere
    // different every boot, but stays consistent while paging this session.
    m_likedSeed = QRandomGenerator::global()->generate() | 1u;
    m_media = new MediaCache(this);
    m_media->setMaxEntries(24);
    Q_UNUSED(state);  // feed sources now live on the shared FeedEngine
    m_db = &m_engine->db();
    buildUi();
    m_engine->startWarmStart();

    // Kick off the first batch once the page is visible.
    QTimer::singleShot(0, this, [this]() {
        requestMore();
        showNext();
    });
}

FeedPage::~FeedPage() { stopAnimation(); }

void FeedPage::stopAnimation() {
    if (m_playback)
        m_playback->stop();  // closes the decoder, releasing the file
    if (!m_playbackFile.isEmpty())
        QFile::remove(m_playbackFile);
    m_playbackFile.clear();
    m_playbackMd5.clear();
    m_playedMs = 0;
    m_playRunStartMs = 0;
    if (m_videoBar)
        m_videoBar->hide();
}

qint64 FeedPage::playedMs() const {
    const qint64 run =
        m_playRunStartMs > 0 ? QDateTime::currentMSecsSinceEpoch() - m_playRunStartMs : 0;
    return m_playedMs + run;
}

void FeedPage::pausePlayback() {
    if (!m_playback || !m_playback->isActive())
        return;
    m_playedMs = playedMs();
    m_playRunStartMs = 0;
    m_playback->pause();
    updatePlayButton();
}

void FeedPage::resumePlayback() {
    if (!m_playback || !m_playback->isActive())
        return;
    if (m_playRunStartMs == 0)
        m_playRunStartMs = QDateTime::currentMSecsSinceEpoch();
    m_playback->resume();
    updatePlayButton();
}

void FeedPage::updatePlayButton() {
    if (!m_playButton)
        return;
    const bool playing = m_playback && m_playback->isPlaying();
    m_playButton->setIcon(
        assetIcon(playing ? QStringLiteral("pause.svg") : QStringLiteral("play.svg")));
}

void FeedPage::startAnimation(const SourceImage& image, const QByteArray& bytes) {
    QString ext;
    if (bytes.isEmpty() || !mayBeAnimated(image, &ext) || ext.isEmpty())
        return;
    if (m_playbackMd5 == image.md5)
        return;  // already playing this post

    // Opening the decoder probes the file; do it off the GUI thread.
    QPointer<FeedPage> self(this);
    const QString md5 = image.md5;
    QThreadPool::globalInstance()->start([self, md5, ext, bytes]() {
        const QString path = QDir(Paths::tempDir())
                                 .filePath(QStringLiteral("feed-anim-%1.%2").arg(md5, ext));
        auto* decoder = new VideoDecoder();
        bool ok = false;
        {
            QDir().mkpath(Paths::tempDir());
            QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) {
                file.close();
                ok = decoder->open(path) && decoder->isAnimated();
            }
        }
        auto discard = [decoder, path]() {
            decoder->close();
            delete decoder;
            QFile::remove(path);
        };
        if (!ok || !self) {
            discard();
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, md5, path, decoder, discard]() {
                const bool current = self && self->m_position >= 0 &&
                                     self->m_position < self->m_queue.size() &&
                                     self->m_queue.at(self->m_position).md5 == md5;
                if (!current) {
                    discard();  // user moved on
                    return;
                }
                self->stopAnimation();
                self->m_playbackFile = path;
                self->m_playbackMd5 = md5;
                self->m_playback->start(decoder);  // takes ownership
                self->m_playback->setVolume(self->m_volume->value());
                self->m_playRunStartMs = QDateTime::currentMSecsSinceEpoch();

                const qint64 duration = self->m_playback->durationMs();
                self->m_progressUpdating = true;
                self->m_progress->setRange(0, static_cast<int>(qMax<qint64>(0, duration)));
                self->m_progress->setValue(0);
                self->m_progressUpdating = false;
                self->m_progress->setEnabled(duration > 0);
                self->m_timeLabel->setText(
                    QStringLiteral("0:00 / %1").arg(formatClock(duration)));
                self->m_videoBar->show();
                self->updatePlayButton();
                if (!self->isVisible())
                    self->pausePlayback();
            },
            Qt::QueuedConnection);
    });
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

    // Animated posts: frames replace the still image on the stage, looping.
    m_playback = new MediaPlaybackController(this);
    connect(m_playback, &MediaPlaybackController::frameReady, this,
            [this](const VideoDecoder::Frame& frame) {
                VideoDecoder::Frame shown = frame;
                if (shown.image.isNull())
                    shown = m_playback->seekTo(0);  // end reached: loop
                if (shown.image.isNull())
                    return;
                m_mediaLabel->setPixmap(QPixmap::fromImage(shown.image.scaled(
                    m_mediaLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                if (!m_progress->isSliderDown() && shown.ptsMs >= 0) {
                    m_progressUpdating = true;
                    m_progress->setValue(static_cast<int>(shown.ptsMs));
                    m_progressUpdating = false;
                    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(
                        formatClock(shown.ptsMs), formatClock(m_playback->durationMs())));
                }
            });

    // Animated-post controls: play/pause, seekable progress, volume.
    m_videoBar = new QWidget(m_stage);
    auto* videoLayout = new QHBoxLayout(m_videoBar);
    videoLayout->setContentsMargins(0, 4, 0, 0);
    videoLayout->setSpacing(8);
    m_playButton = new QToolButton(m_videoBar);
    m_playButton->setIcon(assetIcon(QStringLiteral("pause.svg")));
    m_playButton->setToolTip(QStringLiteral("Play / pause"));
    connect(m_playButton, &QToolButton::clicked, this, [this]() {
        if (m_playback && m_playback->isPlaying())
            pausePlayback();
        else
            resumePlayback();
    });
    m_progress = new QSlider(Qt::Horizontal, m_videoBar);
    m_progress->setToolTip(QStringLiteral("Seek"));
    auto seekToSlider = [this]() {
        if (!m_playback || !m_playback->isActive())
            return;
        const VideoDecoder::Frame frame = m_playback->seekTo(m_progress->sliderPosition());
        if (!frame.image.isNull()) {
            m_mediaLabel->setPixmap(QPixmap::fromImage(frame.image.scaled(
                m_mediaLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    };
    // Seek once on release (cheap for long videos); clicks on the groove seek too.
    connect(m_progress, &QSlider::sliderReleased, this, seekToSlider);
    connect(m_progress, &QSlider::valueChanged, this, [this, seekToSlider](int) {
        if (!m_progressUpdating && !m_progress->isSliderDown())
            seekToSlider();
    });
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        m_timeLabel->setText(QStringLiteral("%1 / %2").arg(
            formatClock(value), formatClock(m_playback ? m_playback->durationMs() : 0)));
    });
    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), m_videoBar);
    m_timeLabel->setProperty("role", QStringLiteral("hint"));
    auto* volumeIcon = new QLabel(m_videoBar);
    volumeIcon->setPixmap(assetIcon(QStringLiteral("volume.svg")).pixmap(16, 16));
    m_volume = new QSlider(Qt::Horizontal, m_videoBar);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(100);
    m_volume->setToolTip(QStringLiteral("Volume"));
    {
        AppState saved;
        m_volume->setValue(saved.load() ? saved.feedVolume() : 0);
    }
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        if (m_playback)
            m_playback->setVolume(value);
    });
    // Persist once the user lets go, not on every step of a drag.
    auto saveVolume = [this]() {
        AppState saved;
        if (saved.load()) {
            saved.setFeedVolume(m_volume->value());
            saved.save();
        }
    };
    connect(m_volume, &QSlider::sliderReleased, this, saveVolume);
    connect(m_volume, &QSlider::actionTriggered, this, [saveVolume](int action) {
        if (action != QAbstractSlider::SliderMove)
            QTimer::singleShot(0, saveVolume);  // clicks / wheel / keys
    });
    videoLayout->addWidget(m_playButton);
    videoLayout->addWidget(m_progress, 1);
    videoLayout->addWidget(m_timeLabel);
    videoLayout->addSpacing(8);
    videoLayout->addWidget(volumeIcon);
    videoLayout->addWidget(m_volume);
    m_videoBar->hide();
    stageLayout->addWidget(m_videoBar);

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

    // Taste sway: temporary tags steering the feed right now. Session-only,
    // never trains the long-term model. Sits at the right end of the action
    // row - a first-class feature, not debug-only.
    m_swayEdit = new QLineEdit(this);
    m_swayEdit->setPlaceholderText(QStringLiteral("sway tags (space separated)"));
    m_swayEdit->setToolTip(
        QStringLiteral("Temporarily steer the feed toward these tags. Does not change your "
                       "learned profile - clear the box to go back."));
    m_swayEdit->setClearButtonEnabled(true);
    m_swayEdit->setFixedWidth(220);
    m_swayCompleter = new QCompleter(this);
    m_swayCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_swayCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_swayCompleter->setFilterMode(Qt::MatchStartsWith);
    m_swayCompleter->setMaxVisibleItems(12);
    m_swayCompleter->setWidget(m_swayEdit);
    // Do NOT also call m_swayEdit->setCompleter(): QLineEdit's built-in
    // completer handling then suppresses our manual complete() calls after
    // the first popup (suggestions die after a space until re-click).
    m_swayEdit->installEventFilter(this);
    m_swayButton = new QPushButton(QStringLiteral("Sway"), this);
    m_swayButton->setToolTip(
        QStringLiteral("Apply the sway tags (Enter works too). Clear the box to go back."));
    m_swayButton->setFocusPolicy(Qt::NoFocus);
    connect(m_swayButton, &QPushButton::clicked, this, &FeedPage::applySwayTags);
    connect(m_swayEdit, &QLineEdit::returnPressed, this, &FeedPage::applySwayTags);
    // QCompleter matches the whole line text, so after the first space it
    // stops suggesting. Complete the word under the cursor instead (same
    // approach as the source-browser search box).
    connect(m_swayEdit, &QLineEdit::textChanged, this, [this]() { updateSwayCompletion(); });
    connect(m_swayEdit, &QLineEdit::cursorPositionChanged, this,
            [this]() { updateSwayCompletion(); });
    connect(m_swayCompleter, QOverload<const QString&>::of(&QCompleter::activated), this,
            [this](const QString& completion) {
                const int cursor = m_swayEdit->cursorPosition();
                const QString text = m_swayEdit->text();
                int start = cursor;
                while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
                QString updated = text;
                updated.replace(start, cursor - start, completion);
                m_swayEdit->setText(updated);
                m_swayEdit->setCursorPosition(start + completion.size() + 1);
                m_swayCompleter->popup()->hide();
            });
    // Library tag list for completions (same source the search bars use).
    QThreadPool::globalInstance()->start([self = QPointer<FeedPage>(this)]() {
        LibrarySource library;
        const QStringList names = library.allTagNames();
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, names]() {
                if (!self)
                    return;
                self->m_swayTagNames = names;
                self->m_swayCompleter->setModel(new QStringListModel(names, self->m_swayCompleter));
            },
            Qt::QueuedConnection);
    });

    // Exploration level: how much of the feed goes to tangents and how far
    // ranking strays from what the model is sure about. Persisted in the
    // feed DB; applied to the next batch.
    m_explorationSlider = new QSlider(Qt::Horizontal, this);
    m_explorationSlider->setRange(0, 100);
    m_explorationSlider->setPageStep(10);
    m_explorationSlider->setValue(qRound(m_db->explorationLevel() * 100.0));
    m_explorationSlider->setFocusPolicy(Qt::NoFocus);  // arrow keys navigate the feed
    m_explorationSlider->setToolTip(
        QStringLiteral("How adventurous the feed is. Left: stick to what you already like. "
                       "Right: more tangents - new tags and looks the feed is unsure about, "
                       "so it can discover what else you enjoy. The far right also mixes in "
                       "random posts unrelated to anything you like."));
    m_explorationLabel = new QLabel(this);
    m_explorationLabel->setMinimumWidth(m_explorationLabel->fontMetrics().horizontalAdvance(
        QStringLiteral("Explore: Adventurous")));
    updateExplorationLabel();
    connect(m_explorationSlider, &QSlider::valueChanged, this, [this]() {
        updateExplorationLabel();
        // Clicks / page steps apply at once; drags apply on release.
        if (!m_explorationSlider->isSliderDown())
            applyExplorationLevel();
    });
    connect(m_explorationSlider, &QSlider::sliderReleased, this, &FeedPage::applyExplorationLevel);

    // Two equal expanding spacers sandwich the button group, so it stays
    // centered on the row. The exploration group on the left is fixed to
    // the sway group's width for the same reason.
    auto* swayGroup = new QHBoxLayout();
    swayGroup->setContentsMargins(0, 0, 0, 0);
    swayGroup->setSpacing(6);
    swayGroup->addWidget(m_swayEdit);
    swayGroup->addWidget(m_swayButton);

    auto* actionRow = new QHBoxLayout();
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(10);
    auto* exploreGroup = new QWidget(this);
    auto* exploreLayout = new QHBoxLayout(exploreGroup);
    exploreLayout->setContentsMargins(0, 0, 0, 0);
    exploreLayout->setSpacing(6);
    exploreLayout->addWidget(m_explorationLabel);
    exploreLayout->addWidget(m_explorationSlider, 1);
    exploreGroup->setFixedWidth(m_swayEdit->width() + 6 + m_swayButton->sizeHint().width() + 10);
    actionRow->addWidget(exploreGroup);
    actionRow->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Fixed));
    actionRow->addWidget(m_prevButton);
    actionRow->addWidget(m_dislikeButton);
    actionRow->addWidget(m_nextButton);
    actionRow->addWidget(m_likeButton);
    actionRow->addWidget(m_saveButton);
    actionRow->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Fixed));
    actionRow->addLayout(swayGroup);
    actionRow->setStretch(1, 1);
    actionRow->setStretch(7, 1);
    stageLayout->addLayout(actionRow);

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

    m_debugPanel = new QWidget(this);
    m_debugPanel->setFixedWidth(320);
    auto* debugLayout = new QVBoxLayout(m_debugPanel);
    debugLayout->setContentsMargins(6, 6, 6, 6);
    m_debugText = new QTextBrowser(m_debugPanel);
    m_debugText->setOpenLinks(false);
    debugLayout->addWidget(m_debugText, 1);

    // Hovering a taste-profile line shows its full tag-weight breakdown.
    connect(m_debugText, &QTextBrowser::highlighted, this, [this](const QUrl& url) {
        const QString link = url.toString();
        if (link.startsWith(QLatin1String("post:"))) {
            const QString md5 = link.mid(5);
            const QString tip = m_postTagTips.value(md5);
            if (tip.isEmpty()) {
                QToolTip::hideText();
                return;
            }
            QToolTip::showText(
                QCursor::pos(),
                QStringLiteral("<b style='color:#e8a33d;'>%1</b><br>%2").arg(md5, tip),
                m_debugText);
            return;
        }
        if (!link.startsWith(QLatin1String("taste:"))) {
            QToolTip::hideText();
            return;
        }
        bool ok = false;
        const int index = link.mid(6).toInt(&ok);
        if (!ok || index < 0 || index >= m_tasteDetails.size()) {
            QToolTip::hideText();
            return;
        }
        const auto& t = m_tasteDetails.at(index);
        QString tip = QStringLiteral(
                          "<b style='color:#e8a33d;'>Taste #%1</b><br>"
                          "<span style='color:#999;'>strength %2 · confidence %3 · "
                          "exposure %4 · engagement %5</span><br>")
                          .arg(t.id)
                          .arg(t.strength, 0, 'f', 2)
                          .arg(t.confidence, 0, 'f', 2)
                          .arg(t.exposure, 0, 'f', 2)
                          .arg(t.engagement, 0, 'f', 2);
        if (t.tags.isEmpty()) {
            tip += QStringLiteral("<span style='color:#999;'>no tag vector</span>");
        } else {
            tip += QStringLiteral(
                "<b>Tags</b> <span style='color:#999;'>(niche = query "
                "rank score, w = taste vector weight)</span><br>");
            QStringList rows;
            for (int i = 0; i < t.tags.size(); ++i) {
                const auto& tag = t.tags.at(i);
                // The top-niche tag is the query anchor for this taste.
                const char* color = i == 0 ? "#e8a33d" : (tag.weight >= 0 ? "#5fbf6f" : "#e05a5a");
                rows << QStringLiteral(
                            "<span style='color:%1'>%2</span> "
                            "<span style='color:#999;'>niche %3 · w %4</span>")
                            .arg(QLatin1String(color), tag.name.toHtmlEscaped())
                            .arg(tag.niche, 0, 'f', 3)
                            .arg(tag.weight, 0, 'f', 3);
            }
            tip += rows.join(QStringLiteral("<br>"));
        }
        QToolTip::showText(QCursor::pos(), tip, m_debugText);
    });

    m_droppedButton = new QPushButton(QStringLiteral("Dropped images"), m_debugPanel);
    m_droppedButton->setToolTip(
        QStringLiteral("Show the posts the funnel dropped this batch, with the reason"));
    debugLayout->addWidget(m_droppedButton);
    connect(m_droppedButton, &QPushButton::clicked, this, &FeedPage::toggleDropped);

    m_likedButton = new QPushButton(QStringLiteral("Liked posts"), m_debugPanel);
    m_likedButton->setToolTip(
        QStringLiteral("Open every post you liked or saved in the viewer, loaded as you "
                       "scroll through them"));
    debugLayout->addWidget(m_likedButton);
    connect(m_likedButton, &QPushButton::clicked, this, &FeedPage::toggleLiked);

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
    // Never steal focus from widgets that take keyboard input themselves
    // (e.g. the sway line edit) - otherwise typed keys fall through to
    // keyPressEvent and navigate the feed, and QCompleter::complete()
    // silently no-ops because its widget lost focus.
    if (event->type() == QEvent::MouseButtonPress && watched != m_tagSidebar) {
        auto* w = qobject_cast<QWidget*>(watched);
        if (!w || w->focusPolicy() == Qt::NoFocus)
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
    // Defensive: if the sway edit (or its completer popup) somehow holds
    // focus, let it handle the key instead of navigating the feed.
    QWidget* focus = QApplication::focusWidget();
    if (focus && focus != this &&
        (focus == m_swayEdit || focus->isAncestorOf(m_swayEdit) ||
         (m_swayCompleter->popup() &&
          (focus == m_swayCompleter->popup() || focus->isAncestorOf(m_swayCompleter->popup()))))) {
        QWidget::keyPressEvent(event);
        return;
    }
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
        case Qt::Key_L:
            onLikeClicked();
            advance(false);
            break;
        case Qt::Key_D:
            if (!(event->modifiers() & Qt::ControlModifier)) {
                break;
            }
        case Qt::Key_Down:
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
        case Qt::Key_F11:
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
    if (!m_db->isOpen()) {
        // Without the feed DB there is no profile to recommend from.
        m_statusLabel->setText(QStringLiteral(
            "Feed unavailable: the feed database could not be opened (see Mosaic.log)."));
        return;
    }
    m_fetching = true;
    m_statusLabel->setText(QStringLiteral("Finding posts you might like…"));

    QPointer<FeedPage> self(this);
    std::shared_ptr<FeedEngine> engine = m_engine;
    QThreadPool::globalInstance()->start([self, engine]() {
        const QList<SourceImage> batch = engine->nextBatch(kBatchSize);
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
                    // Post ids share the set with md5s ("source:id" never
                    // collides with a hex hash).
                    const QString postKey =
                        img.remoteId > 0
                            ? img.sourceId + QLatin1Char(':') + QString::number(img.remoteId)
                            : QString();
                    if (self->m_seenMd5s.contains(img.md5) ||
                        (!postKey.isEmpty() && self->m_seenMd5s.contains(postKey))) {
                        ++droppedSeen;
                        continue;
                    }
                    if (!img.isInLibrary() && img.fileUrl.isEmpty() && img.sampleUrl.isEmpty() &&
                        img.previewUrl.isEmpty()) {
                        ++droppedNoUrl;
                        continue;
                    }
                    self->m_seenMd5s.insert(img.md5);
                    if (!postKey.isEmpty())
                        self->m_seenMd5s.insert(postKey);
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
        if (!nextImg.isInLibrary() && nextImg.fileUrl.isEmpty() && nextImg.sampleUrl.isEmpty() &&
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
    stopAnimation();

    // Impression: marks the post as seen; carries no taste signal.
    if (!image.md5.isEmpty() && !m_interactedMd5s.contains(image.md5)) {
        QStringList tags;
        for (const SourceTag& t : image.tags) tags << t.name;
        FeedDatabase* db = m_db;
        const SourceImage img = image;
        const int pos = m_position;
        QThreadPool::globalInstance()->start([db, img, tags, pos]() {
            db->recordEvent(FeedDatabase::Event::Impression, img.sourceId, img.md5, tags, 0.0, pos,
                            img.remoteId);
        });
    }

    m_tagSidebar->setImage(image, /*editable=*/false);
    updateDebugPanel();

    m_infoLabel->setText(QStringLiteral("<b>%1</b> · %2 · %3 · ★%4")
                             .arg(image.md5.left(12),
                                  image.format.isEmpty() ? QStringLiteral("?") : image.format,
                                  sourceLabel(m_sources, image).toHtmlEscaped())
                             .arg(image.score));
    m_mediaLabel->setText(QStringLiteral("…"));
    m_mediaLabel->setPixmap(QPixmap());

    // Posts with no usable URL can never load - fail fast.
    if (!image.isInLibrary() && image.fileUrl.isEmpty() && image.sampleUrl.isEmpty() &&
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
        QPointer<FeedPage> self(this);
        QThreadPool::globalInstance()->start([self, image]() {
            LibrarySource library;
            const bool inLibrary = library.hasImage(image);
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
    FeedDatabase* db = m_db;
    auto decodeOffThread = [db](const SourceImage& post, const QByteArray& bytes,
                                bool allowSkipOnFailure, QPointer<FeedPage> self) {
        QThreadPool::globalInstance()->start([self, db, post, bytes, allowSkipOnFailure]() {
            const QImage frame = decodeBytes(post, bytes);
            // The frame is decoded anyway: its style descriptor comes free,
            // so every shown post can train the model's visual weights.
            if (!frame.isNull() && db && !db->hasVisual(post.md5))
                db->storeVisual(post.md5, VisualFeatures::compute(frame));
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, post, frame, allowSkipOnFailure]() {
                    if (!self)
                        return;
                    // Position check first: a late result (even a failure)
                    // for a post the user already left must not touch, or
                    // skip, the post now on screen.
                    if (self->m_position < 0 || self->m_position >= self->m_queue.size())
                        return;
                    if (self->m_queue.at(self->m_position).md5 != post.md5)
                        return;  // user moved on
                    if (frame.isNull()) {
                        if (!self->m_mediaLoaded && allowSkipOnFailure)
                            self->skipFailedPost(QStringLiteral("decode failed"));
                        return;
                    }
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
            startAnimation(img, bytes);
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
    m_infoLabel->setText(QStringLiteral("<b>%1</b> · %2 · %3 · ★%4")
                             .arg(img.md5.left(12),
                                  img.format.isEmpty() ? QStringLiteral("?") : img.format,
                                  sourceLabel(m_sources, img).toHtmlEscaped())
                             .arg(img.score));
    m_statusLabel->clear();
}

void FeedPage::hideEvent(QHideEvent* event) {
    // Tab switch voids this post's dwell time.
    m_shownAt = 0;
    pausePlayback();
    QWidget::hideEvent(event);
}

void FeedPage::showEvent(QShowEvent* event) {
    // Dwell timer restarts from 0 s: if media is already on screen, start
    // counting now; otherwise applyDecodedMedia() starts it on render.
    if (m_mediaLoaded)
        m_shownAt = QDateTime::currentMSecsSinceEpoch();
    if (m_playback && m_playback->isActive() && !m_playback->isPlaying())
        resumePlayback();
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
    const qint64 dwell = m_shownAt > 0 ? QDateTime::currentMSecsSinceEpoch() - m_shownAt : 0;

    // Training writes run off the GUI thread (DB write lock).
    FeedEngine* engine = m_engine.get();
    const SourceImage imgCopy = img;
    QThreadPool::globalInstance()->start(
        [engine, event, imgCopy, dwell]() { engine->recordReaction(event, imgCopy, dwell); });
}

void FeedPage::advance(bool train) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Dwell is recorded once per post, ever: revisiting via Back never
    // re-records it. It is tracked separately from explicit reactions so
    // moving on doesn't block a like/dislike after going back.
    if (train && m_mediaLoaded && m_position >= 0 && m_position < m_queue.size() && m_shownAt > 0 &&
        !m_interactedMd5s.contains(m_queue.at(m_position).md5) &&
        !m_dwelledMd5s.contains(m_queue.at(m_position).md5)) {
        // Mark before the async write so a fast double-swipe can't double-train.
        m_dwelledMd5s.insert(m_queue.at(m_position).md5);
        const qint64 dwell = now - m_shownAt;
        const SourceImage img = m_queue.at(m_position);
        // Animated posts that started playing are judged on play time vs
        // length; ones that never got past the still frame count as images.
        const bool played = m_playback && m_playback->isActive() && m_playbackMd5 == img.md5;
        const qint64 duration = played ? m_playback->durationMs() : 0;
        const qint64 playTime = played ? playedMs() : 0;

        // Training writes happen off the GUI thread.
        FeedEngine* engine = m_engine.get();
        QThreadPool::globalInstance()->start([engine, img, dwell, duration, playTime]() {
            engine->recordDwell(img, dwell, duration, playTime);
        });
    }

    showNext();
}

// actions

void FeedPage::onSoftReset() {
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Soft reset"),
        QStringLiteral("Wipe the learned profile and rebuild it from your library favorites?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    // A running warm start would keep writing into the wiped profile.
    m_engine->stopWarmStart();
    m_db->reset(/*rerunWarmStart=*/true);
    m_engine->startWarmStart();
    m_queue.clear();
    m_seenMd5s.clear();
    m_position = -1;
    m_statusLabel->setText(QStringLiteral("Profile rebuilt from your library favorites."));
    updateDebugPanel();
    requestMore();
    showNext();
}

void FeedPage::onHardReset() {
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Hard reset"),
        QStringLiteral("Wipe the ENTIRE feed profile? All learned tastes, tag preferences and "
                       "history are gone for good."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_engine->stopWarmStart();
    m_db->reset(/*rerunWarmStart=*/false);
    m_queue.clear();
    m_seenMd5s.clear();
    m_position = -1;
    m_statusLabel->setText(QStringLiteral("Profile wiped - starting fresh."));
    updateDebugPanel();
    requestMore();
    showNext();
}

void FeedPage::toggleDropped() {
    const auto dropped = m_engine->recommender().lastFunnelStats().dropped;
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Dropped images (last funnel run)"));
    dialog->resize(900, 600);
    auto* layout = new QVBoxLayout(dialog);

    // Thumbnail grid like the source-browser tabs; click opens the viewer.
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* grid = new FlowLayout(content, 0, 1, 1);
    grid->setItemSizeRange(220);
    scroll->setWidget(content);
    layout->addWidget(scroll);

    if (dropped.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("Nothing dropped in the last batch."), content);
        grid->addWidget(empty);
    }

    QList<SourceImage> images;
    for (const auto& [img, reason] : dropped) {
        if (img.md5.isEmpty())
            continue;
        const int index = images.size();
        images.append(img);
        auto* thumb = new ThumbnailButton(img, content);
        thumb->setToolTip(QStringLiteral("<b style='color:#e8a33d;'>%1</b><br>%2")
                              .arg(reason.toHtmlEscaped(), img.tooltipHtml()));
        grid->addWidget(thumb);
        connect(thumb, &ThumbnailButton::clicked, this, [this, images, index]() {
            auto* viewer =
                new ViewerWindow(m_sources.source(images.at(index).sourceId.isEmpty()
                                                      ? m_sources.remoteSources().value(0)->id()
                                                      : images.at(index).sourceId),
                                 images, index);
            viewer->setAttribute(Qt::WA_DeleteOnClose);
            viewer->show();
        });
    }

    // Load previews off-thread, same disk cache as the source browser.
    const QString cacheDir = QDir(Paths::tempDir()).filePath(QStringLiteral("thumbcache"));
    for (ThumbnailButton* thumb : content->findChildren<ThumbnailButton*>()) {
        const SourceImage img = thumb->image();
        QPointer<QWidget> self(this);
        QPointer<ThumbnailButton> btn(thumb);
        Source* source = m_sources.source(
            img.sourceId.isEmpty() ? m_sources.remoteSources().value(0)->id() : img.sourceId);
        if (!source)
            continue;
        ThreadPools::network()->start([btn, source, img, cacheDir]() {
            QImage decoded;
            const QString cachePath = cacheDir + QLatin1Char('/') + img.md5 + QLatin1String(".jpg");
            if (!decoded.load(cachePath)) {
                if (QFileInfo::exists(cachePath))
                    QFile::remove(cachePath);
                const FileKind kind =
                    !img.previewUrl.isEmpty() ? FileKind::Preview : FileKind::Sample;
                const QByteArray bytes = source->fetch(img, kind);
                if (!decoded.loadFromData(bytes) && !img.localPath.isEmpty())
                    decoded.load(img.localPath);
                if (!decoded.isNull()) {
                    QDir().mkpath(cacheDir);
                    decoded.save(cachePath, "JPG", 80);
                }
            }
            if (decoded.isNull() || !btn)
                return;
            const QPixmap pixmap = QPixmap::fromImage(decoded);
            QMetaObject::invokeMethod(
                btn.data(),
                [btn, pixmap]() {
                    if (btn)
                        btn->setPixmap(pixmap);
                },
                Qt::QueuedConnection);
        });
    }

    dialog->show();
}

void FeedPage::toggleLiked() {
    if (m_likedViewer) {
        m_likedViewer->raise();
        m_likedViewer->activateWindow();
        return;
    }
    openLikedViewer();
}

void FeedPage::openLikedViewer() {
    // One merged source so fetches route back to whichever site (or the
    // library) each liked post came from.
    if (!m_likedSource)
        m_likedSource = m_sources.createMultiSource(m_sources.sources());
    if (!m_likedSource)
        return;

    m_likedOffset = 0;
    m_likedTotal = 0;
    m_likedLoading = false;
    m_likedDone = false;
    ++m_likedGeneration;

    auto* viewer = new ViewerWindow(m_likedSource, {}, 0);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    m_likedViewer = viewer;
    connect(viewer, &ViewerWindow::nearEnd, this, [this](int) { loadLikedPage(); });
    connect(viewer, &QObject::destroyed, this, [this]() {
        m_likedViewer = nullptr;
        m_likedLoading = false;
        ++m_likedGeneration;  // drop pages still in flight
    });

    // Shown once the first page arrives, so an empty profile never flashes
    // an empty window.
    loadLikedPage();
}

void FeedPage::loadLikedPage() {
    if (m_likedLoading || m_likedDone || !m_likedViewer || !m_db)
        return;
    m_likedLoading = true;

    // Small pages keep the first post on screen fast: each remote post
    // needs an md5 lookup to recover its URLs, and a profile can hold
    // thousands of likes. Posts are handed to the viewer one at a time as
    // they resolve, so the first one shows immediately.
    constexpr int kPageSize = 20;
    const int offset = m_likedOffset;
    const int generation = m_likedGeneration;
    const quint32 seed = m_likedSeed;
    FeedDatabase* db = m_db;
    SourceManager* sources = &m_sources;
    QPointer<FeedPage> self(this);

    QThreadPool::globalInstance()->start([self, db, sources, offset, generation, seed]() {
        const int total = offset == 0 ? db->likedInteractionCount() : -1;
        const auto rows = db->likedInteractions(kPageSize, offset, seed);

        int resolved = 0;
        for (const auto& [md5, sourceId] : rows) {
            SourceImage img;
            // Library catalog first, then the site the like came from, then
            // the others.
            if (!FeedWarmStart::resolvePost(*sources, md5, sourceId, &img,
                                            /*maxRemoteQueries=*/0)) {
                // Keep it in the list so the gap is visible rather than
                // silently dropping a liked post. Route fetches through
                // a real source so the viewer can still try.
                img.md5 = md5;
                img.sourceId = sourceId;
                if (sources->source(img.sourceId) == nullptr) {
                    const QList<Source*> remotes = sources->remoteSources();
                    if (!remotes.isEmpty())
                        img.sourceId = remotes.first()->id();
                }
            }
            ++resolved;

            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, img, generation]() {
                    if (!self || self->m_likedGeneration != generation)
                        return;
                    ViewerWindow* viewer = self->m_likedViewer;
                    if (!viewer)
                        return;
                    viewer->appendImages({img});
                    if (!viewer->isVisible()) {
                        viewer->show();
                        viewer->raise();
                        viewer->activateWindow();
                    }
                },
                Qt::QueuedConnection);
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, total, offset, resolved, generation]() {
                if (!self || self->m_likedGeneration != generation)
                    return;
                self->m_likedLoading = false;
                if (total >= 0)
                    self->m_likedTotal = total;
                ViewerWindow* viewer = self->m_likedViewer;
                if (!viewer)
                    return;
                if (resolved == 0) {
                    self->m_likedDone = true;
                    if (offset == 0) {
                        // Never shown: drop it without touching the saved
                        // viewer geometry (closeEvent would persist it).
                        viewer->deleteLater();
                        self->m_statusLabel->setText(
                            QStringLiteral("No liked posts yet - like or save a few first."));
                    }
                    return;
                }
                self->m_likedOffset = offset + resolved;
                if (self->m_likedTotal > 0 && self->m_likedOffset >= self->m_likedTotal)
                    self->m_likedDone = true;
            },
            Qt::QueuedConnection);
    });
}

void FeedPage::updateSwayCompletion() {
    // Complete the word under the cursor (SourceBrowser::updateCompletion).
    const QString text = m_swayEdit->text();
    const int cursor = m_swayEdit->cursorPosition();
    int start = cursor;
    while (start > 0 && text.at(start - 1) != QLatin1Char(' ')) --start;
    const QString token = text.mid(start, cursor - start);
    if (token.isEmpty()) {
        if (m_swayCompleter->popup())
            m_swayCompleter->popup()->hide();
        return;
    }
    m_swayCompleter->setCompletionPrefix(token);
    if (m_swayCompleter->completionCount() > 0) {
        QRect rect = m_swayEdit->rect();
        const int leftX = m_swayEdit->fontMetrics().horizontalAdvance(text.left(start)) +
                          m_swayEdit->rect().left();
        rect.setLeft(leftX);
        // QCompleter skips complete() when it thinks the popup is already
        // up; hide it first so suggestions reliably reappear (e.g. after a
        // space splits the token).
        if (m_swayCompleter->popup() && m_swayCompleter->popup()->isVisible())
            m_swayCompleter->popup()->hide();
        m_swayCompleter->complete(rect);
    } else if (m_swayCompleter->popup()) {
        m_swayCompleter->popup()->hide();
    }
}

void FeedPage::applySwayTags() {
    const QStringList tags = m_swayEdit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    m_db->setSwayTags(tags);

    // Drop the not-yet-shown part of the queue so the next posts come from
    // the swayed ranking instead of the old one.
    const int keep = qMax(m_position + 1, 0);
    while (m_queue.size() > keep + 3) m_queue.removeLast();
    m_statusLabel->setText(
        tags.isEmpty()
            ? QStringLiteral("Sway cleared.")
            : QStringLiteral("Feed swayed toward: %1").arg(tags.join(QStringLiteral(", "))));
    requestMore();
    updateDebugPanel();
}

void FeedPage::updateExplorationLabel() {
    const int v = m_explorationSlider->value();
    const char* name = v < 20   ? "Comfort"
                       : v < 45 ? "Familiar"
                       : v < 65 ? "Balanced"
                       : v < 85 ? "Curious"
                       : v < 95 ? "Adventurous"
                                : "Wild";
    m_explorationLabel->setText(QStringLiteral("Explore: %1").arg(QLatin1String(name)));
}

void FeedPage::applyExplorationLevel() {
    const double level = m_explorationSlider->value() / 100.0;
    if (qAbs(level - m_db->explorationLevel()) < 0.005)
        return;
    // Like a sway: re-rank what has not been shown yet under the new level.
    const int keep = qMax(m_position + 1, 0);
    while (m_queue.size() > keep + 3) m_queue.removeLast();
    m_statusLabel->setText(QStringLiteral("Exploration set to %1% - %2")
                               .arg(qRound(level * 100.0))
                               .arg(level < 0.35 ? QStringLiteral("sticking close to your tastes.")
                                    : level < 0.65
                                        ? QStringLiteral("a mix of favorites and tangents.")
                                        : QStringLiteral("trying lots of new things.")));

    // The DB write can wait on the training lock: off the GUI thread, and
    // the re-rank starts only once the new level is in place.
    FeedDatabase* db = m_db;
    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([self, db, level]() {
        db->setExplorationLevel(level);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self]() {
                if (self)
                    self->requestMore();
            },
            Qt::QueuedConnection);
    });
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
        QString currentHtml;                  // pre-rendered current-post section
        QString upcomingHtml;                 // pre-rendered upcoming-queue section
        QHash<QString, QString> postTagTips;  // md5-prefix -> tag tooltip html
    };

    const int position = m_position;
    const qint64 sessionStart = m_sessionStartMs;
    const QList<SourceImage> queue = m_queue.mid(0, position + 6);
    const SourceImage current =
        (position >= 0 && position < queue.size()) ? queue.at(position) : SourceImage{};

    FeedDatabase* db = m_db;
    FeedRecommender* rec = &m_engine->recommender();
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
                    (kind == QLatin1String("like") || kind == QLatin1String("favorite") ||
                     kind == QLatin1String("view"))
                        ? kPos
                        : kNeg;
                parts << QStringLiteral("<span style='%1'>%2</span> %3")
                             .arg(color)
                             .arg(kind)
                             .arg(md5.left(6));
            }
            html += parts.join(QStringLiteral(" · ")) + QStringLiteral("<br>");
        }

        // One explanation pass for the current post and the upcoming ones
        // (shares the scoring context).
        QList<SourceImage> explained;
        for (int i = qMax(0, position); i < qMin(position + 6, queue.size()); ++i)
            explained.append(queue.at(i));
        const QList<FeedRecommender::Explanation> explanations = rec->explainPosts(explained);
        const FeedModel model = db->modelSnapshot();
        // Learned per-tag weight (posterior mean) for the tag breakdowns.
        auto learnedWeight = [&model](const QString& tag) {
            return model.belief(QStringLiteral("t:") + tag).mean;
        };
        auto slotLabel = [](const FeedRecommender::Explanation& e) {
            if (e.loggedSlot == QLatin1String("tangent"))
                return QStringLiteral(" · <span style='color:#e8a33d'>tangent</span>");
            return QString();
        };

        // current post: heuristic breakdown + the learned model's verdict
        if (position >= 0 && position < queue.size() && !explanations.isEmpty()) {
            const FeedRecommender::Explanation& e = explanations.first();
            const FeedDatabase::RecommendationRow& bd = e.heuristic;
            html +=
                QStringLiteral(
                    "<br><b style='%1'>Current post</b> %2%3<br>"
                    "heuristic: taste %4 + session %5 + tags %6 = <b>%7</b><br>"
                    "model: <b>p %8</b> ±%9 · %10 learned · taste #%11 · src %12%13<br>")
                    .arg(kTitle)
                    .arg(current.md5.left(8))
                    .arg(slotLabel(e))
                    .arg(bd.userSimilarity, 0, 'f', 2)
                    .arg(bd.sessionSimilarity, 0, 'f', 2)
                    .arg(bd.tagScore, 0, 'f', 2)
                    .arg(e.heuristicScore, 0, 'f', 2)
                    .arg(e.modelProbability, 0, 'f', 2)
                    .arg(e.uncertainty, 0, 'f', 2)
                    .arg(QStringLiteral("%1%").arg(qRound(e.blend * 100.0)))
                    .arg(bd.tasteId)
                    .arg(e.loggedSource.isEmpty() ? QStringLiteral("?")
                                                  : e.loggedSource.toHtmlEscaped())
                    .arg(e.visualKnown
                             ? QString()
                             : QStringLiteral(" · <span style='%1'>no look yet</span>").arg(kDim));

            // Per-tag learned weights for THIS post: negatives in red,
            // positives in green, so it is visible exactly which tags
            // dragged the prediction down.
            QStringList negParts, posParts;
            for (const QString& tag : FeedRecommender::contentTags(current)) {
                const double w = learnedWeight(tag);
                if (w < -0.1)
                    negParts << QStringLiteral("<span style='%1'>%2 (%3)</span>")
                                    .arg(kNeg, tag.toHtmlEscaped())
                                    .arg(w, 0, 'f', 2);
                else if (w > 0.1)
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

        // upcoming queue, with the total fed count on the header line. The
        // model's predicted engagement (± its uncertainty) is the
        // actionable question here. Each line is a link; hovering shows the
        // post's tags with learned weights.
        html += QStringLiteral("<b style='%1'>Upcoming</b> · total fed %2<br>")
                    .arg(kTitle)
                    .arg(static_cast<qint64>(db->totalImpressions()));
        for (int i = position + 1; i < qMin(position + 6, queue.size()); ++i) {
            const SourceImage& img = queue.at(i);
            const int ei = i - qMax(0, position);
            if (ei < 0 || ei >= explanations.size())
                break;
            const FeedRecommender::Explanation& e = explanations.at(ei);
            const char* color =
                e.modelProbability >= 0.55 ? kPos : (e.modelProbability >= 0.4 ? kDim : kNeg);
            html += QStringLiteral(
                        "<a href='post:%1' style='color:#cccccc; "
                        "text-decoration:none;'>%1…</a> <span style='%2'>p %3 ±%4</span> "
                        "<span style='%5'>h %6 · %7</span>%8<br>")
                        .arg(img.md5.left(8), QLatin1String(color))
                        .arg(e.modelProbability, 0, 'f', 2)
                        .arg(e.uncertainty, 0, 'f', 2)
                        .arg(QLatin1String(kDim))
                        .arg(e.heuristicScore, 0, 'f', 2)
                        .arg(e.loggedSource.isEmpty() ? QStringLiteral("?")
                                                      : e.loggedSource.toHtmlEscaped())
                        .arg(slotLabel(e));

            // Cache the tag breakdown for the hover tooltip, strongest first.
            struct TagRow {
                QString tag;
                double w;
            };
            QList<TagRow> ranked;
            for (const QString& tag : FeedRecommender::contentTags(img))
                ranked.append({tag, learnedWeight(tag)});
            std::sort(ranked.begin(), ranked.end(),
                      [](const TagRow& a, const TagRow& b) { return a.w > b.w; });
            QStringList tagRows;
            for (const TagRow& r : ranked) {
                const char* tagColor = r.w < -0.1 ? kNeg : (r.w > 0.1 ? kPos : kDim);
                tagRows << QStringLiteral("<span style='%1'>%2 %3</span>")
                               .arg(QLatin1String(tagColor), r.tag.toHtmlEscaped())
                               .arg(r.w, 0, 'f', 2);
            }
            snap.postTagTips.insert(img.md5.left(8),
                                    tagRows.isEmpty()
                                        ? QStringLiteral("<span style='color:#999;'>no tags</span>")
                                        : tagRows.join(QStringLiteral("<br>")));
        }

        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, html, snap]() {
                if (!self)
                    return;
                self->m_debugStatsPending = false;
                self->m_postTagTips = snap.postTagTips;
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
        FeedRecommender::ModelStats model;
        FeedRecommender::FunnelStats funnel;
    };

    FeedDatabase* db = m_db;
    FeedRecommender* rec = &m_engine->recommender();
    QPointer<FeedPage> self(this);
    QThreadPool::globalInstance()->start([db, rec, self]() {
        HeavyStats stats;
        stats.lingers = db->averageLingers();
        stats.interactions = db->interactionCounts();
        stats.model = rec->modelStats();
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
                const FeedRecommender::ModelStats& ms = stats.model;
                self->m_tasteDetails = ms.tasteDetails;

                auto learnedList = [](const QList<FeedRecommender::ModelStats::LearnedTag>& tags) {
                    QStringList items;
                    for (const auto& t : tags)
                        items << QStringLiteral("%1 (%2%3 ±%4)")
                                     .arg(t.tag.toHtmlEscaped())
                                     .arg(t.mean >= 0 ? QStringLiteral("+") : QString())
                                     .arg(t.mean, 0, 'f', 2)
                                     .arg(t.sd, 0, 'f', 2);
                    return items.join(QStringLiteral(", "));
                };

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
                         .arg(ms.skipStreak)
                         .arg(ms.sessionBlend * 100.0, 0, 'f', 0);

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

                // learned model
                h += QStringLiteral(
                         "<b style='color:#e8a33d; margin-top:8px;'>Learned model</b><br>"
                         "<b>%1% learned</b> <span style='color:#999'>· %2 samples · %3 "
                         "live-validated · %4 beliefs</span><br>")
                         .arg(ms.blend * 100.0, 0, 'f', 0)
                         .arg(ms.modelSamples, 0, 'f', 0)
                         .arg(ms.validationSamples, 0, 'f', 0)
                         .arg(ms.beliefCount);
                {
                    const bool winning = ms.modelLogLoss < ms.heuristicLogLoss;
                    h += QStringLiteral(
                             "<span style='color:#999'>log-loss (lower wins):</span> "
                             "<span style='color:%1'>model %2</span> vs heuristic %3<br>")
                             .arg(QLatin1String(winning ? "#5fbf6f" : "#e05a5a"))
                             .arg(ms.modelLogLoss, 0, 'f', 3)
                             .arg(ms.heuristicLogLoss, 0, 'f', 3);
                }
                h += QStringLiteral(
                         "<span style='color:#999'>exploration %1% · tangents shown %2 · liked "
                         "%3%4</span><br>")
                         .arg(ms.exploration * 100.0, 0, 'f', 0)
                         .arg(ms.tangentShown, 0, 'f', 0)
                         .arg(ms.tangentHits, 0, 'f', 0)
                         .arg(ms.tangentShown > 0
                                  ? QStringLiteral(" (%1%)").arg(
                                        100.0 * ms.tangentHits / ms.tangentShown, 0, 'f', 0)
                                  : QString());
                if (ms.warmStartDone) {
                    h += QStringLiteral("<span style='color:#999'>warm start: done</span><br>");
                } else {
                    h += QStringLiteral(
                             "<span style='color:#e8a33d'>warm start: replaying history "
                             "%1 / %2</span><br>")
                             .arg(ms.warmStartProgress, 0, 'f', 0)
                             .arg(ms.warmStartTotal, 0, 'f', 0);
                }
                if (!ms.learnedLiked.isEmpty())
                    h += QStringLiteral(
                             "<b style='color:#5fbf6f;'>Learned likes</b><br>"
                             "<span style='color:#5fbf6f'>%1</span><br>")
                             .arg(learnedList(ms.learnedLiked));
                if (!ms.learnedDisliked.isEmpty())
                    h += QStringLiteral(
                             "<b style='color:#e05a5a;'>Learned dislikes</b><br>"
                             "<span style='color:#e05a5a'>%1</span><br>")
                             .arg(learnedList(ms.learnedDisliked));
                if (!ms.tangentCandidates.isEmpty())
                    h += QStringLiteral(
                             "<b style='color:#e8a33d;'>Next tangents</b> "
                             "<span style='color:#999'>(seen on liked posts, still "
                             "uncertain)</span><br>%1<br>")
                             .arg(learnedList(ms.tangentCandidates));
                if (!ms.signalWeights.isEmpty()) {
                    QStringList items;
                    for (const auto& [name, w] : ms.signalWeights)
                        items << QStringLiteral("<span style='color:%1'>%2 %3</span>")
                                     .arg(QLatin1String(w >= 0 ? "#5fbf6f" : "#e05a5a"), name)
                                     .arg(w, 0, 'f', 2);
                    h += QStringLiteral("<span style='color:#999'>signal weights:</span> %1<br>")
                             .arg(items.join(QStringLiteral(", ")));
                }
                {
                    QStringList items;
                    for (const auto& [arm, odds] : ms.armOdds)
                        items << QStringLiteral("%1 %2%").arg(arm).arg(odds * 100.0, 0, 'f', 0);
                    h += QStringLiteral("<span style='color:#999'>arm odds: %1</span><br>")
                             .arg(items.join(QStringLiteral(", ")));
                }

                // Taste profiles, capped at 12 lines. Each line is a link
                // with a hover tooltip showing the full tag-weight breakdown.
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Taste profiles</b><br>");
                if (ms.tasteSummary.isEmpty()) {
                    h += QStringLiteral("<span style='color:#999'>(none learned yet)</span><br>");
                } else {
                    const int shown = qMin(12, ms.tasteSummary.size());
                    for (int i = 0; i < shown; ++i) {
                        h += QStringLiteral(
                                 "<a href='taste:%1' style='color:#cccccc; "
                                 "text-decoration:none;'>%2</a><br>")
                                 .arg(i)
                                 .arg(ms.tasteSummary.at(i).toHtmlEscaped());
                    }
                    if (ms.tasteSummary.size() > shown)
                        h += QStringLiteral("<span style='color:#999'>… +%1 more</span><br>")
                                 .arg(ms.tasteSummary.size() - shown);
                }
                // Name the hard negatives explicitly - a count alone hides
                // which tags are being vetoed from queries and filtering.
                h += QStringLiteral("<span style='color:#999'>hard negatives: %1</span><br>")
                         .arg(ms.hardNegatives);
                if (!ms.hardNegativeTags.isEmpty())
                    h += QStringLiteral("<span style='color:#e05a5a'>%1</span><br>")
                             .arg(ms.hardNegativeTags.join(QStringLiteral(", ")));
                if (!ms.swayTags.isEmpty())
                    h += QStringLiteral("<span style='color:#e8a33d'>sway active: %1</span><br>")
                             .arg(ms.swayTags.join(QStringLiteral(", ")));

                // self-learned tag specificity
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Tag specificity</b><br>");
                if (ms.lowSpecificityTags.isEmpty()) {
                    h +=
                        QStringLiteral("<span style='color:#999'>(not enough data yet)</span><br>");
                } else {
                    h += QStringLiteral(
                             "<span style='color:#999'>near-universal (auto-detected, no "
                             "blacklist): %1</span><br>")
                             .arg(ms.lowSpecificityTags.join(QStringLiteral(", ")));
                }

                // last funnel run - one line per stage, counts in -> out,
                // with every drop reason broken out per stage.
                const auto& f = stats.funnel;
                h += QStringLiteral(
                    "<b style='color:#e8a33d; margin-top:8px;'>Last funnel "
                    "run</b><br>");
                {
                    QStringList arms;
                    for (const auto& [arm, n] : f.armCandidates)
                        arms << QStringLiteral("%1 %2").arg(arm).arg(n);
                    h += QStringLiteral("<b>arms</b> <span style='color:#999'>%1</span><br>")
                             .arg(arms.isEmpty() ? QStringLiteral("-")
                                                 : arms.join(QStringLiteral(" · ")));
                }
                const int stage1Drops = f.dropScore + f.dropHardNeg + f.dropNoMd5;
                h += QStringLiteral(
                         "<b>stage 1</b> fetch + dedup + cheap filter: %1 kept · %2 dropped<br>")
                         .arg(f.stage1Filtered)
                         .arg(stage1Drops);
                h += QStringLiteral(
                         "<span style='color:#999'>lowScore %1 · hardNeg %2 · noMd5 %3</span><br>")
                         .arg(f.dropScore)
                         .arg(f.dropHardNeg)
                         .arg(f.dropNoMd5);
                h += QStringLiteral(
                         "<b>stage 2</b> shortlist: %1 kept · %2 dropped<br>"
                         "<b>stage 3</b> visual descriptors: %3 new<br>")
                         .arg(f.stage2Shortlist)
                         .arg(f.stage1Filtered - f.stage2Shortlist)
                         .arg(f.visualsComputedThisBatch);
                h += QStringLiteral(
                         "<b>batch</b> %1 selected · %2 tangent slots · %3% learned · cold "
                         "start: %4%5<br>")
                         .arg(f.batchSize)
                         .arg(f.tangentSlots)
                         .arg(f.blend * 100.0, 0, 'f', 0)
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
    const qint64 dwell =
        m_mediaLoaded && m_shownAt > 0 ? QDateTime::currentMSecsSinceEpoch() - m_shownAt : 0;

    // Explicit thumbs down: strong negative signal for every tag of the
    // post. OFF the GUI thread - same lock-contention reason as
    // recordCurrent() above.
    FeedEngine* engine = m_engine.get();
    const SourceImage imgCopy = img;
    QThreadPool::globalInstance()->start([engine, imgCopy, dwell]() {
        engine->recordReaction(FeedDatabase::Event::Dislike, imgCopy, dwell);
    });

    m_dislikeButton->setText(QStringLiteral("Disliked"));
    QTimer::singleShot(1200, this,
                       [this]() { m_dislikeButton->setText(QStringLiteral("Dislike")); });
}

void FeedPage::onSaveClicked() {
    if (m_position < 0 || m_position >= m_queue.size())
        return;
    const SourceImage img = m_queue.at(m_position);

    // Strongest training signal + actual library import, off-thread.
    recordCurrent(FeedDatabase::Event::Favorite);
    m_saveButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("Saving…"));

    QPointer<FeedPage> self(this);
    FeedEngine* engine = m_engine.get();
    QThreadPool::globalInstance()->start([self, engine, img]() {
        const ImportResult result = engine->saveToLibrary(img);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, result]() {
                if (!self)
                    return;
                self->m_saveButton->setEnabled(true);
                self->m_statusLabel->setText(
                    !result.ok              ? QStringLiteral("Save failed: %1").arg(result.error)
                    : result.alreadyExisted ? QStringLiteral("Already in library")
                                            : QStringLiteral("Saved to library"));
            },
            Qt::QueuedConnection);
    });
}

}  // namespace Mosaic
