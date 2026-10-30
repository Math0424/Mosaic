#include "MainWindow.h"

#include "BuildNumber.h"

#include "../components/Icons.h"
#include "../components/ConflictsPage.h"
#include "../components/TagMergePage.h"
#include "../components/FavoritesPage.h"
#include "../components/ImportPage.h"
#include "../components/MultiSourceDialog.h"
#include "../components/SettingsDialog.h"
#include "../components/SourceBrowser.h"
#include "../feed/FeedPage.h"
#include "../core/Database.h"
#include "../core/Paths.h"
#include "../core/Shutdown.h"
#include "../core/ThreadPools.h"
#include "../sources/SourceManager.h"
#include "../sources/local/LibrarySource.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QThreadPool>
#include <QToolButton>

#include <cstdlib>
#include <thread>

namespace {

QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

// Purge cached media older than a day; returns remaining size in bytes.
qint64 purgeOldCache() {
    QDir dir(Mosaic::Paths::tempDir());
    if (!dir.exists())
        return 0;

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    qint64 total = 0;
    const QFileInfoList files = dir.entryInfoList(QDir::Files);
    for (const QFileInfo& fi : files) {
        if (fi.lastModified() < cutoff) {
            QFile::remove(fi.absoluteFilePath());
            continue;
        }
        total += fi.size();
    }
    return total;
}

}  // namespace

namespace Mosaic {

MainWindow* MainWindow::instance = nullptr;

MainWindow::MainWindow(Database& db, QWidget* parent) : QMainWindow(parent), m_db(db) {
    instance = this;
    setWindowTitle(QStringLiteral("Mosaic (v.%1)").arg(Mosaic_BUILD_NUMBER));
    resize(1100, 700);

    buildUi();
    restoreState();

    const qint64 cacheBytes = purgeOldCache();
    statusBar()->addPermanentWidget(
        new QLabel(QStringLiteral("cache: %1").arg(formatBytes(cacheBytes)), this));

    startLibraryScan();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget* w = m_tabs->widget(index);
        // The Favorites tab is permanent.
        if (w == m_favoritesTab)
            return;
        // Remember the tab so Ctrl+Shift+T can bring it back.
        TabState closed;
        closed.title = m_tabs->tabText(index);
        if (auto* browser = qobject_cast<SourceBrowser*>(w)) {
            closed.sourceId = browser->source()->id();
            const QList<Source*> members = browser->source()->members();
            if (!members.isEmpty()) {
                QStringList ids;
                for (Source* member : members) ids << member->id();
                closed.multiSourceIds = ids.join(QLatin1Char(','));
            }
            closed.tags = browser->tags();
            closed.page = qMax(1, browser->page());
        }
        m_closedTabs.append(closed);
        m_tabs->removeTab(index);
        cancelTabNameEdit();
        w->deleteLater();
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs->tabBar(), &QTabBar::tabBarDoubleClicked, this, &MainWindow::renameSourceTab);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, &MainWindow::onTabMoved);
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QTabBar::customContextMenuRequested, this,
            &MainWindow::showTabContextMenu);

    m_tabNameEditor = new QLineEdit(m_tabs->tabBar());
    m_tabNameEditor->setFrame(false);
    m_tabNameEditor->setAlignment(Qt::AlignCenter);
    m_tabNameEditor->setStyleSheet(
        QStringLiteral("QLineEdit { background: #23262d; color: #e8e9ec; border: 1px solid #4f8cff;"
                       " padding: 0 4px; }"));
    m_tabNameEditor->hide();
    m_tabNameEditor->installEventFilter(this);
    m_tabs->tabBar()->installEventFilter(this);
    connect(m_tabNameEditor, &QLineEdit::editingFinished, this, &MainWindow::finishTabNameEdit);
    connect(m_tabNameEditor, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_editingTabWidget || !m_tabs)
            return;
        const int index = m_tabs->indexOf(m_editingTabWidget);
        if (index >= 0)
            m_tabs->setTabText(index, text);
    });

    m_newTabButton = new QToolButton(m_tabs);
    m_newTabButton->setIcon(Mosaic::assetIcon(QStringLiteral("plus.svg")));
    m_newTabButton->setAutoRaise(true);
    m_newTabButton->setToolTip(QStringLiteral("New tab (choose a source)"));
    connect(m_newTabButton, &QToolButton::clicked, this, &MainWindow::showNewTabMenu);
    m_tabs->setCornerWidget(m_newTabButton, Qt::TopLeftCorner);

    // Not a real tab: opens the feed page with no tab entry.
    m_feedButton = new QToolButton(m_tabs);
    m_feedButton->setText(QStringLiteral("For You"));
    m_feedButton->setIcon(Mosaic::assetIcon(QStringLiteral("feed.svg")));
    m_feedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_feedButton->setAutoRaise(true);
    m_feedButton->setToolTip(QStringLiteral("For You feed"));
    connect(m_feedButton, &QToolButton::clicked, this, &MainWindow::showFeedPage);
    m_tabs->setCornerWidget(m_feedButton, Qt::TopRightCorner);

    setCentralWidget(m_tabs);

    m_status = new QLabel(this);
    statusBar()->addWidget(m_status);

    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&Exit"), this, &QWidget::close);

    QMenu* toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->addAction(QStringLiteral("&Conflicts (duplicates)…"), this, [this]() {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (qobject_cast<ConflictsPage*>(m_tabs->widget(i))) {
                m_tabs->setCurrentIndex(i);
                return;
            }
        }
        const int index = m_tabs->addTab(new ConflictsPage(m_tabs), QStringLiteral("Conflicts"));
        m_tabs->setCurrentIndex(index);
    });
    toolsMenu->addAction(QStringLiteral("&Merge tags\u2026"), this, [this]() {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (qobject_cast<TagMergePage*>(m_tabs->widget(i))) {
                m_tabs->setCurrentIndex(i);
                return;
            }
        }
        const int index = m_tabs->addTab(new TagMergePage(m_tabs), QStringLiteral("Merge tags"));
        m_tabs->setCurrentIndex(index);
    });
    toolsMenu->addAction(QStringLiteral("&Import library…"), this, [this]() {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (qobject_cast<ImportPage*>(m_tabs->widget(i))) {
                m_tabs->setCurrentIndex(i);
                return;
            }
        }
        const int index = m_tabs->addTab(new ImportPage(m_tabs), QStringLiteral("Import"));
        m_tabs->setCurrentIndex(index);
    });
    toolsMenu->addAction(QStringLiteral("&For You feed…"), this, &MainWindow::showFeedPage);
    toolsMenu->addSeparator();
    toolsMenu->addAction(QStringLiteral("&Settings…"), this, [this]() {
        const QString oldLibrary = m_state.libraryPath();
        SettingsDialog dialog(m_state, m_sources, this);
        if (dialog.exec() == QDialog::Accepted) {
            applyGridSettings();
            applyBlacklist();
            applyFeedTabSetting();
            m_sources.applyGelbooruCredentials(m_state.siteCredentials());
            if (m_state.libraryPath() != oldLibrary)
                applyLibraryPath(m_state.libraryPath());
            m_state.save();
        }
    });
}

void MainWindow::applyGridSettings() {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(i))) {
            browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
        }
    }
}

void MainWindow::applyBlacklist() {
    const QStringList blacklist = m_state.blacklistTags();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(i)))
            browser->setBlacklist(blacklist);
    }
}

void MainWindow::applyLibraryPath(const QString& newPath) {
    const QString oldDir = Paths::libraryDir();
    Paths::setLibraryDirOverride(newPath);

    Database& db = Database::instance();
    db.close();

    if (!newPath.isEmpty() && QDir::cleanPath(oldDir) != QDir::cleanPath(newPath) &&
        QDir(oldDir).exists()) {
        QDir().mkpath(newPath);
        if (!QDir().rename(oldDir, newPath)) {
            const QFileInfoList files = QDir(oldDir).entryInfoList(QDir::Files);
            for (const QFileInfo& fi : files) {
                QFile::remove(newPath + QLatin1Char('/') + fi.fileName());
                QFile::rename(fi.absoluteFilePath(), newPath + QLatin1Char('/') + fi.fileName());
            }
        }
    }
    const QString legacyDb = Paths::appDataDir() + QStringLiteral("/Mosaic.duckdb");
    if (QFile::exists(legacyDb) && !QFile::exists(Paths::databaseFile()))
        QFile::rename(legacyDb, Paths::databaseFile());

    if (db.open()) {
        setStatus(QStringLiteral("Library folder set to %1")
                      .arg(newPath.isEmpty() ? QStringLiteral("the default location")
                                             : QDir::toNativeSeparators(newPath)));
    } else {
        setStatus(QStringLiteral("Library folder changed, but the catalog could not be"
                                 " reopened: %1")
                      .arg(db.lastError()));
    }
}

void MainWindow::showNewTabMenu() {
    QMenu menu(this);
    menu.addAction(QStringLiteral("Library (downloaded images)"), this,
                   [this]() { addSourceTab(m_sources.library()); });
    for (Source* source : m_sources.remoteSources()) {
        menu.addAction(source->displayName(), this, [this, source]() { addSourceTab(source); });
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("All sources (merged)…"), this, [this]() {
        const QList<Source*> available = m_sources.remoteSources();
        if (available.isEmpty())
            return;
        MultiSourceDialog dialog(available, this);
        if (dialog.exec() == QDialog::Accepted)
            addMultiSourceTab(dialog.selectedSources());
    });

    QWidget* corner = m_tabs->cornerWidget(Qt::TopLeftCorner);
    menu.exec(corner ? corner->mapToGlobal(corner->rect().bottomLeft()) : QCursor::pos());
}

void MainWindow::addFavoritesTab() {
    m_favoritesTab = new FavoritesPage(m_sources, m_state, m_tabs);
    const int index = m_tabs->addTab(m_favoritesTab, QStringLiteral("Favorites"));
    m_tabs->tabBar()->moveTab(index, 0);
}

void MainWindow::applyFeedTabSetting() {
    // Page is created lazily on first open so a disabled feed never
    // spins up its database.
    m_feedButton->setVisible(m_state.feedTabEnabled());
}

void MainWindow::showFeedPage() {
    if (!m_feedPage) {
        m_feedPage = new FeedPage(m_sources, m_state, m_tabs);
        const int index = m_tabs->addTab(m_feedPage, QStringLiteral("For You"));
        m_tabs->tabBar()->setTabVisible(index, false);
        connect(m_feedPage, &FeedPage::tagSearchRequested, this,
                [this](const QString& tag, const QString& sourceId) {
                    Source* source = m_sources.source(sourceId);
                    if (!source) {
                        const QList<Source*> remotes = m_sources.remoteSources();
                        source = remotes.isEmpty() ? nullptr : remotes.first();
                    }
                    if (source)
                        addSourceTab(source, tag);
                });
    }
    m_tabs->setCurrentWidget(m_feedPage);
}

void MainWindow::showTabContextMenu(const QPoint& pos) {
    const int index = m_tabs->tabBar()->tabAt(pos);
    if (index < 0)
        return;
    auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(index));
    if (!browser)
        return;

    QMenu menu(this);
    menu.addAction(QStringLiteral("Add to Favorites…"), this, [this, index]() {
        auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(index));
        if (!browser)
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Add to Favorites"),
                                                   QStringLiteral("Favorite name:"),
                                                   QLineEdit::Normal, m_tabs->tabText(index), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        FavoriteEntry entry;
        entry.name = name.trimmed();
        entry.tags = browser->tags();
        entry.sourceId = browser->source()->id();
        const QList<Source*> members = browser->source()->members();
        if (!members.isEmpty()) {
            QStringList ids;
            for (Source* member : members) ids << member->id();
            entry.multiSourceIds = ids.join(QLatin1Char(','));
        }
        entry.lastViewed = QDateTime::currentMSecsSinceEpoch();
        m_state.addFavorite(entry);
        m_state.save();
        if (m_favoritesTab)
            m_favoritesTab->refresh();
        setStatus(QStringLiteral("Added \"%1\" to favorites").arg(entry.name));
    });
    menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
}

void MainWindow::reopenClosedTab() {
    if (m_closedTabs.isEmpty()) {
        setStatus(QStringLiteral("No closed tab to reopen"));
        return;
    }
    const TabState t = m_closedTabs.takeLast();
    if (!t.sourceId.isEmpty()) {
        if (t.sourceId == QLatin1String("multi")) {
            QList<Source*> members;
            for (const QString& id : t.multiSourceIds.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                if (Source* member = m_sources.source(id.trimmed()))
                    members.append(member);
            }
            if (!members.isEmpty()) {
                SourceBrowser* browser = addMultiSourceTab(members, t.title);
                if (!t.tags.isEmpty()) {
                    browser->setTags(t.tags);
                    browser->searchFromPage(t.page);
                    browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
                }
            }
            return;
        }
        if (Source* source = m_sources.source(t.sourceId)) {
            SourceBrowser* browser = addSourceTab(source, {}, t.title);
            if (!t.tags.isEmpty()) {
                browser->setTags(t.tags);
                browser->searchFromPage(t.page);
                browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
            }
        }
    }
}

void MainWindow::restoreState() {
    addFavoritesTab();

    if (!m_state.load()) {
        addSourceTab(m_sources.library());
        applyFeedTabSetting();
        return;
    }

    if (AppState::isGeometryOnScreen(m_state.windowGeometry())) {
        setGeometry(m_state.windowGeometry());
    }
    if (m_state.windowMaximized()) {
        showMaximized();
    }

    if (m_favoritesTab)
        m_favoritesTab->refresh();

    const auto tabs = m_state.tabs();
    for (const auto& t : tabs) {
        if (t.sourceId.isEmpty())
            continue;  // legacy folder tabs
        if (t.sourceId == QLatin1String("multi")) {
            QList<Source*> members;
            for (const QString& id : t.multiSourceIds.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                if (Source* member = m_sources.source(id.trimmed()))
                    members.append(member);
            }
            if (members.isEmpty())
                continue;
            SourceBrowser* browser = addMultiSourceTab(members, t.title);
            if (!t.tags.isEmpty()) {
                browser->setTags(t.tags);
                browser->searchFromPage(t.page);
            }
            continue;
        }
        if (Source* source = m_sources.source(t.sourceId)) {
            SourceBrowser* browser = addSourceTab(source, {}, t.title);
            if (!t.tags.isEmpty()) {
                browser->setTags(t.tags);
                browser->searchFromPage(t.page);
                browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
            }
        }
    }
    if (m_tabs->count() <= 1) {
        addSourceTab(m_sources.library());
    }

    const int active = qBound(0, m_state.activeTab() + 1, m_tabs->count() - 1);
    m_tabs->setCurrentIndex(active);

    applyGridSettings();
    applyBlacklist();
    m_sources.applyGelbooruCredentials(m_state.siteCredentials());
    applyFeedTabSetting();
}

void MainWindow::saveState() {
    m_state.setWindowMaximized(isMaximized());
    if (!isMaximized()) {
        m_state.setWindowGeometry(geometry());
    }

    QList<TabState> tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(i));
        if (!browser)
            continue;
        TabState tab;
        tab.title = m_tabs->tabText(i);
        tab.sourceId = browser->source()->id();
        const QList<Source*> members = browser->source()->members();
        if (!members.isEmpty()) {
            QStringList ids;
            for (Source* member : members) ids << member->id();
            tab.multiSourceIds = ids.join(QLatin1Char(','));
        }
        tab.tags = browser->tags();
        tab.page = qMax(1, browser->page());
        tabs.append(tab);
    }
    m_state.setTabs(tabs);
    int active = 0;
    for (int i = 0; i < m_tabs->currentIndex(); ++i) {
        if (qobject_cast<SourceBrowser*>(m_tabs->widget(i)))
            ++active;
    }
    const bool currentIsSessionTab =
        qobject_cast<SourceBrowser*>(m_tabs->currentWidget()) != nullptr;
    m_state.setActiveTab(currentIsSessionTab ? qMax(0, active) : 0);

    if (!m_state.save()) {
        setStatus(QStringLiteral("Could not save UI state: %1").arg(m_state.lastError()));
    }

    if (auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(m_tabs->currentIndex()))) {
        browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
    }
}

SourceBrowser* MainWindow::addSourceTab(Source* source, const QString& tags, const QString& title) {
    auto* browser = new SourceBrowser(source, m_tabs);
    browser->setBlacklist(m_state.blacklistTags());
    browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
    connect(browser, &SourceBrowser::statusChanged, this, [this](const QString& text) {
        if (sender() == m_tabs->currentWidget())
            setStatus(text);
    });
    connect(browser, &SourceBrowser::tagSearchRequested, this,
            [this](const QString& tag, const QString& sourceId) {
                Source* source = m_sources.source(sourceId);
                if (!source)
                    return;
                addSourceTab(source, tag);
            });
    if (!tags.isEmpty()) {
        browser->setTags(tags);
        browser->search();
    }
    const QString tabTitle = title.isEmpty() ? source->displayName() : title;
    const int index = m_tabs->addTab(browser, tabTitle);
    m_tabs->setCurrentIndex(index);
    return browser;
}

SourceBrowser* MainWindow::addMultiSourceTab(const QList<Source*>& members, const QString& title) {
    // Manager owns the MultiSource so it outlives the tab and any
    // worker thread still running a search.
    Source* multi = m_sources.createMultiSource(members);
    if (!multi)
        return nullptr;
    SourceBrowser* browser = addSourceTab(multi, {}, title);
    if (title.isEmpty()) {
        QStringList names;
        for (Source* member : members) names << member->displayName();
        m_tabs->setTabText(m_tabs->indexOf(browser), names.join(QStringLiteral(" + ")));
    }
    return browser;
}

void MainWindow::onTabChanged(int index) {
    Q_UNUSED(index);

    cancelTabNameEdit();
    if (auto* browser = qobject_cast<SourceBrowser*>(m_tabs->currentWidget())) {
        setStatus(QStringLiteral("Source: %1").arg(browser->source()->displayName()));
    } else {
        setStatus({});
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Viewers persist their geometry into state.json on close.
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(i)))
            browser->closeViewerWindow();
    }
    // Reload first on purpose: viewer windows save their geometry through
    // their own AppState, and saveState() would otherwise overwrite it.
    m_state.load();

    saveState();
    event->accept();

    // Ask long-running workers (scan, tag refresh, imports, duplicate
    // search, HTTP waits) to stop at their next safe point.
    Shutdown::request();

    // Close the feed DB before waiting on the pool: its workers hold the
    // DB lock and duckdb_close must not race them. Also CHECKPOINTs the WAL.
    if (m_feedPage)
        m_feedPage->shutdown();

    // Cancelled workers finish their current item (one file import or one
    // HTTP request, which aborts within ~100ms) and drain. The hard exit is
    // a last resort for a task stuck in a call that cannot be interrupted.
    if (!ThreadPools::waitForDone(10000)) {
        qWarning().noquote() << "Shutdown: worker threads still busy after 10s, forcing exit";
        std::thread([]() { Database::instance().close(); }).detach();
        QThread::msleep(250);
        std::_Exit(0);
    }
}

void MainWindow::renameSourceTab(int index) {
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }

    auto* browser = qobject_cast<SourceBrowser*>(m_tabs->widget(index));
    if (!browser) {
        return;
    }

    startTabNameEdit(index);
}

void MainWindow::startTabNameEdit(int index) {
    if (!m_tabNameEditor || !m_tabs) {
        return;
    }
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }

    // Track the tab by widget, not index: tabs can be closed or moved
    // while the editor is open.
    m_editingTabWidget = m_tabs->widget(index);
    m_originalTabTitle = m_tabs->tabText(index);

    m_tabNameEditor->setGeometry(m_tabs->tabBar()->tabRect(index));
    m_tabNameEditor->setText(m_originalTabTitle);
    m_tabNameEditor->show();
    m_tabNameEditor->raise();
    m_tabNameEditor->selectAll();
    m_tabNameEditor->setFocus(Qt::OtherFocusReason);
}

void MainWindow::finishTabNameEdit() {
    if (!m_tabNameEditor || !m_tabNameEditor->isVisible()) {
        return;
    }

    // The tab may have been closed while editing.
    const int index = (m_editingTabWidget && m_tabs) ? m_tabs->indexOf(m_editingTabWidget) : -1;
    const QString newTitle = m_tabNameEditor->text().trimmed();
    if (index >= 0) {
        m_tabs->setTabText(index, newTitle.isEmpty() ? m_originalTabTitle : newTitle);
    }

    m_tabNameEditor->hide();
    m_editingTabWidget = nullptr;
    m_originalTabTitle.clear();
}

void MainWindow::cancelTabNameEdit() {
    if (m_tabNameEditor && m_tabNameEditor->isVisible() && m_editingTabWidget && m_tabs) {
        const int index = m_tabs->indexOf(m_editingTabWidget);
        if (index >= 0)
            m_tabs->setTabText(index, m_originalTabTitle);
    }
    if (m_tabNameEditor)
        m_tabNameEditor->hide();
    m_editingTabWidget = nullptr;
    m_originalTabTitle.clear();
}

void MainWindow::onTabMoved(int from, int to) {
    Q_UNUSED(from);
    Q_UNUSED(to);
    // Keep the pinned Favorites tab left-most no matter how the user drags.
    const int fav = m_favoritesTab ? m_tabs->indexOf(m_favoritesTab) : -1;
    if (fav > 0)
        m_tabs->tabBar()->moveTab(fav, 0);
    if (m_tabNameEditor && m_tabNameEditor->isVisible() && m_editingTabWidget && m_tabs) {
        const int index = m_tabs->indexOf(m_editingTabWidget);
        if (index >= 0)
            m_tabNameEditor->setGeometry(m_tabs->tabBar()->tabRect(index));
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tabNameEditor) {
        if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                cancelTabNameEdit();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                finishTabNameEdit();
                return true;
            }
        }
        if (event->type() == QEvent::FocusOut) {
            finishTabNameEdit();
            return false;
        }
    } else if (watched == m_tabs->tabBar()) {
        if (m_tabNameEditor && m_tabNameEditor->isVisible()) {
            if ((event->type() == QEvent::MouseButtonPress ||
                 event->type() == QEvent::MouseButtonRelease ||
                 event->type() == QEvent::MouseButtonDblClick ||
                 event->type() == QEvent::MouseMove || event->type() == QEvent::Wheel)) {
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setStatus(const QString& text) { m_status->setText(text); }

void MainWindow::startLibraryScan() {
    if (!m_db.isOpen())
        return;

    m_db.connection().execute(QStringLiteral(
        "DELETE FROM tags WHERE id NOT IN (SELECT DISTINCT tag_id FROM image_tags);"));

    setStatus(QStringLiteral("Scanning library folder for new files…"));
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self]() {
        LibrarySource library;
        QString error;
        const int imported = library.scanLibrary(
            [self](int done, int total) {
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, done, total]() {
                        if (self)
                            self->setStatus(
                                QStringLiteral("Scanning library… %1 / %2").arg(done).arg(total));
                    },
                    Qt::QueuedConnection);
            },
            &error);
        int refreshed = 0;
        if (error.isEmpty()) {
            refreshed = library.refreshUntagged(
                [self](int done, int total) {
                    if (!self)
                        return;
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self, done, total]() {
                            if (self)
                                self->setStatus(QStringLiteral("Refreshing tags… %1 / %2")
                                                    .arg(done)
                                                    .arg(total));
                        },
                        Qt::QueuedConnection);
                },
                &error);
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, imported, refreshed, error]() {
                if (!self)
                    return;
                if (!error.isEmpty()) {
                    self->setStatus(QStringLiteral("Library scan failed: %1").arg(error));
                    return;
                }
                self->setStatus(
                    imported > 0 || refreshed > 0
                        ? QStringLiteral("Library scan: imported %1 new file(s), refreshed tags on "
                                         "%2 image(s)")
                              .arg(imported)
                              .arg(refreshed)
                        : QStringLiteral("Library scan: nothing new"));
                if (imported > 0 || refreshed > 0) {
                    for (int i = 0; i < self->m_tabs->count(); ++i) {
                        auto* browser = qobject_cast<SourceBrowser*>(self->m_tabs->widget(i));
                        if (browser && browser->source() && browser->source()->isLocal() &&
                            !browser->tags().isEmpty()) {
                            browser->search();
                        }
                    }
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    switch (event->key()) {
        case Qt::Key_Q:
            if (ctrl) {
                // closeEvent saves state, stops workers and closes the DB;
                // the event loop then ends normally (no exit() mid-teardown).
                close();
            }
            break;
        case Qt::Key_T:
            if (ctrl && shift) {
                reopenClosedTab();
            }
            break;
        case Qt::Key_F11:
            if (this->isMaximized())
                this->showNormal();
            else
                this->showMaximized();
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

}  // namespace Mosaic
