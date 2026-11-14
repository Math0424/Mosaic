#pragma once

#include "FeedDatabase.h"
#include "FeedRecommender.h"

#include "../components/MediaCache.h"
#include "../components/ViewerWindow.h"

#include <QFuture>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <atomic>
#include <memory>

class QLabel;
class QCompleter;
class QLineEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTextBrowser;
class QToolButton;

namespace Mosaic {

class FeedDatabase;
class FeedEngine;
class FeedRecommender;
class VisualFeatures;
class AppState;
class SourceManager;
class TagSidebar;
class MultiSource;
class MediaPlaybackController;

// TikTok-style vertical feed of recommended posts. One post fills the
// viewport; wheel/Space/Next advance. Interactions (watch, like, save,
// skip) train the recommender. State lives in feed.duckdb.
class FeedPage : public QWidget {
    Q_OBJECT

   public:
    FeedPage(SourceManager& sources, std::shared_ptr<FeedEngine> engine, const AppState& state,
             QWidget* parent = nullptr);
    ~FeedPage() override;

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

   private slots:
    void goBack();
    void onLikeClicked();
    void onDislikeClicked();
    void onSaveClicked();
    void toggleDebug();
    void toggleLiked();
    void onSoftReset();
    void onHardReset();

   private:
   signals:
    void tagSearchRequested(const QString& tag, const QString& sourceId);

   private:
    void buildUi();
    void showNext();
    // `train` false when explicit actions already recorded a stronger signal.
    void advance(bool train = true);
    void showPost(const SourceImage& image);
    void markLoved();
    void applyDecodedMedia(const SourceImage& img, const QImage& decoded);
    void preloadNext();
    void skipFailedPost(const QString& reason);
    void requestMore();
    void recordCurrent(FeedDatabase::Event event);
    void updateDebugPanel();
    void refreshHeavyStats();
    void applySwayTags();
    void applyExplorationLevel();
    void updateExplorationLabel();
    void updateSwayCompletion();
    void toggleDropped();
    // Debug: opens every liked/favorited post in a viewer, paging the DB
    // as the user walks toward the end of the loaded window.
    void openLikedViewer();
    void loadLikedPage();

    SourceManager& m_sources;
    // Shared with the web server; owns feed.duckdb and the warm start.
    std::shared_ptr<FeedEngine> m_engine;
    FeedDatabase* m_db = nullptr;  // m_engine->db()

    // UI
    TagSidebar* m_tagSidebar = nullptr;  // collapsible tags, left of the stage
    QWidget* m_stage = nullptr;          // media + info + status column
    QLabel* m_mediaLabel = nullptr;      // full-quality image of the current post
    QLabel* m_infoLabel = nullptr;       // title/tags line
    QLabel* m_statusLabel = nullptr;     // loading / empty-state text
    QPushButton* m_likeButton = nullptr;
    QPushButton* m_dislikeButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_nextButton = nullptr;
    QPushButton* m_prevButton = nullptr;       // back to the previous post
    QWidget* m_debugPanel = nullptr;           // right-hand debug sidebar (hidden, F12)
    QTextBrowser* m_debugText = nullptr;       // score breakdown / model stats
    QPushButton* m_softResetButton = nullptr;  // rebuild profile from favorites
    QPushButton* m_hardResetButton = nullptr;  // full wipe
    QPushButton* m_droppedButton = nullptr;    // debug: list stage-1 dropped images
    QPushButton* m_likedButton = nullptr;      // debug: view every liked post
    QLineEdit* m_swayEdit = nullptr;           // temporary taste-sway tags
    QPushButton* m_swayButton = nullptr;
    QCompleter* m_swayCompleter = nullptr;
    QStringList m_swayTagNames;              // full library tag list, filtered per word
    QSlider* m_explorationSlider = nullptr;  // comfort .. adventurous
    QLabel* m_explorationLabel = nullptr;

    // Feed state
    QList<SourceImage> m_queue;      // upcoming posts (already ranked)
    QSet<QString> m_seenMd5s;        // md5s + "source:id" keys queued this session
    QSet<QString> m_interactedMd5s;  // posts that already recorded a signal -
                                     // (explicit like/dislike/save) - never double-train
    QSet<QString> m_dwelledMd5s;     // dwell already recorded (once per post); does NOT
                                     // block an explicit reaction after going BACK
    int m_position = -1;             // index into m_queue of the current post
    qint64 m_shownAt = 0;            // when the current post went on screen
    bool m_fetching = false;
    bool m_awaitingAdvance = false;
    bool m_mediaLoaded = false;     // dwell time only counts once media shows
    int m_consecutiveFailures = 0;  // drives backoff when a server is down
    int m_sessionPosts = 0;         // posts shown this session (retention stat)
    qint64 m_sessionStartMs = 0;    // when the first post of this session showed
    qint64 m_lastWheelMs = 0;       // wheel debounce timestamp (0 = never wheeled)

    // Shared byte-cache + prefetch pipeline (same machinery as the viewer
    // window): raw bytes per "<md5>:<kind>", preview shown instantly while
    // the full file downloads in parallel.
    MediaCache* m_media = nullptr;
    int m_mediaIndex = -1;  // queue index whose media is on screen

    // Animated posts play (muted, looping) on the stage once their full
    // file has arrived; the still frame shows until then.
    MediaPlaybackController* m_playback = nullptr;
    QString m_playbackFile;  // temp copy the decoder reads from
    QString m_playbackMd5;
    // Play time counts toward "viewed" only while actually playing.
    qint64 m_playedMs = 0;         // accumulated before the current run
    qint64 m_playRunStartMs = 0;   // wall time the current run began, 0 = paused
    qint64 playedMs() const;
    void pausePlayback();
    void resumePlayback();

    // Controls under the stage, shown only for animated posts.
    QWidget* m_videoBar = nullptr;
    QToolButton* m_playButton = nullptr;
    QSlider* m_progress = nullptr;
    QLabel* m_timeLabel = nullptr;
    QSlider* m_volume = nullptr;
    bool m_progressUpdating = false;  // programmatic progress updates
    void updatePlayButton();
    void startAnimation(const SourceImage& image, const QByteArray& bytes);
    void stopAnimation();

    // Debug panel: tier-2 (heavy DB) stats cached between async loads.
    QString m_heavyStatsHtml;  // last rendered "Totals & profile" HTML
    bool m_heavyStatsPending = false;
    qint64 m_lastHeavyStatsLoad = 0;
    FeedRecommender::ModelStats::TasteDetails m_tasteDetails;  // hover tooltips
    QHash<QString, QString> m_postTagTips;  // upcoming-post md5 prefix -> tag tooltip
    QString m_lastDebugHtml;                // last fully-rendered debug panel HTML
    bool m_debugStatsPending = false;       // debug snapshot job in flight

    // Debug "liked posts" viewer: pages the DB as the user walks the list.
    QPointer<ViewerWindow> m_likedViewer;
    MultiSource* m_likedSource = nullptr;  // routes fetches back per sourceId
    int m_likedOffset = 0;                 // DB rows already handed to the viewer
    int m_likedTotal = 0;                  // distinct liked posts (0 = not counted yet)
    bool m_likedLoading = false;
    bool m_likedDone = false;
    int m_likedGeneration = 0;  // invalidates pages from a closed viewer
    // Shuffle seed for the liked-posts walk. Generated once per session so
    // each launch surfaces a different subset first, while paging within a
    // session stays consistent.
    quint32 m_likedSeed = 0;
};

}  // namespace Mosaic
