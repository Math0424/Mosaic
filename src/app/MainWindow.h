#pragma once

#include <QMainWindow>
#include <QPointer>

#include <memory>

#include "../core/AppState.h"
#include "../sources/SourceManager.h"

class QLabel;
class QTabWidget;
class QLineEdit;
class QToolButton;
class QEvent;
class QString;

namespace Mosaic {

class Database;
class FavoritesPage;
class FeedEngine;
class FeedPage;
class WebServer;
class Source;
class SourceBrowser;

// Application shell: tabbed workspace of SourceBrowser tabs + status bar.
// UI state (geometry, tabs) persists as JSON in AppData.
class MainWindow : public QMainWindow {
    Q_OBJECT

   public:
    explicit MainWindow(Database& db, QWidget* parent = nullptr);
    ~MainWindow() override;

    static MainWindow* instance;

    // --serve: runs the web server this session without changing the saved
    // setting (port 0 = the saved port).
    bool startWebServerForSession(quint16 port, QString* error = nullptr);

   protected:
    void closeEvent(QCloseEvent* event) override;

   private slots:
    void showNewTabMenu();
    void onTabChanged(int index);
    void renameSourceTab(int index);
    void finishTabNameEdit();
    void cancelTabNameEdit();
    void onTabMoved(int from, int to);

   private:
    void buildUi();
    void restoreState();
    void saveState();
    void applyGridSettings();
    void applyBlacklist();
    void applyLibraryPath(const QString& newPath);
    SourceBrowser* addSourceTab(Source* source, const QString& tags = {},
                                const QString& title = {});
    SourceBrowser* addMultiSourceTab(const QList<Source*>& members, const QString& title = {});
    void addFavoritesTab();
    void applyFeedTabSetting();
    void showFeedPage();
    // Shared feed state, created on first use (feed tab or web feed).
    std::shared_ptr<FeedEngine> feedEngine();
    // Starts / restarts / stops the web server to match the settings.
    bool applyWebServer(QString* error = nullptr);
    void showWebServerDialog();
    void showTabContextMenu(const QPoint& pos);
    void reopenClosedTab();  // Ctrl+Shift+T
    void setStatus(const QString& text);
    void bringToFront();
    void startLibraryScan();
    void startTabNameEdit(int index);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    Database& m_db;
    AppState m_state;
    SourceManager m_sources;
    QTabWidget* m_tabs = nullptr;
    FavoritesPage* m_favoritesTab = nullptr;
    FeedPage* m_feedPage = nullptr;
    std::shared_ptr<FeedEngine> m_feedEngine;
    WebServer* m_webServer = nullptr;
    QToolButton* m_feedButton = nullptr;
    QList<TabState> m_closedTabs;
    QLabel* m_status = nullptr;
    QToolButton* m_newTabButton = nullptr;
    QLineEdit* m_tabNameEditor = nullptr;
    QPointer<QWidget> m_editingTabWidget;
    QString m_originalTabTitle;
};

}  // namespace Mosaic
