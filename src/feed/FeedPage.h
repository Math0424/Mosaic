#pragma once

#include "FeedDatabase.h"
#include "FeedRecommender.h"

#include "../components/MediaCache.h"

#include <QSet>
#include <QWidget>

class QLabel;
class QCompleter;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;

namespace Mosaic {

class FeedDatabase;
class FeedRecommender;
class EmbeddingBackend;
class AppState;
class SourceManager;
class TagSidebar;

// TikTok-style vertical feed of recommended posts. One post fills the
// viewport; wheel/Space/Next advance. Interactions (watch, like, save,
// skip) train the recommender. State lives in feed.duckdb.
class FeedPage : public QWidget {
    Q_OBJECT

   public:
    FeedPage(SourceManager& sources, const AppState& state, QWidget* parent = nullptr);
    ~FeedPage() override;

    // Called at app shutdown before the pool drains: closes the feed DB.
    void shutdown();

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
    void seedFromFavorites();
    void applySwayTags();
    void updateSwayCompletion();
    void toggleDropped();

    SourceManager& m_sources;
    FeedDatabase* m_db = nullptr;
    EmbeddingBackend* m_embeddings = nullptr;  // visual embeddings (optional)
    FeedRecommender* m_recommender = nullptr;

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
    QLineEdit* m_swayEdit = nullptr;           // temporary taste-sway tags
    QPushButton* m_swayButton = nullptr;
    QCompleter* m_swayCompleter = nullptr;
    QStringList m_swayTagNames;  // full library tag list, filtered per word

    // Feed state
    QList<SourceImage> m_queue;      // upcoming posts (already ranked)
    QSet<QString> m_seenMd5s;        // shown this session (dedup against DB too)
    QSet<QString> m_interactedMd5s;  // posts that already recorded a signal -
                                     // going BACK must never double-train them
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

    // Debug panel: tier-2 (heavy DB) stats cached between async loads.
    QString m_heavyStatsHtml;  // last rendered "Totals & profile" HTML
    bool m_heavyStatsPending = false;
    qint64 m_lastHeavyStatsLoad = 0;
    FeedRecommender::ModelStats::TasteDetails m_tasteDetails;  // hover tooltips
    QHash<QString, QString> m_postTagTips;  // upcoming-post md5 prefix -> tag tooltip
    QString m_lastDebugHtml;                // last fully-rendered debug panel HTML
    bool m_debugStatsPending = false;       // debug snapshot job in flight
};

}  // namespace Mosaic
