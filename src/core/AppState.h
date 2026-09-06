#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>

namespace Mosaic {

struct TabState {
    QString title;
    QString sourceId;        // "library", "e621", ...
    QString multiSourceIds;  // comma-separated member ids
    QString tags;
    int page = 1;
};

// A saved favorite: quick access to a tag group on a source (state file only).
struct FavoriteEntry {
    QString name;
    QString tags;
    QString sourceId;
    QString multiSourceIds;
    qint64 lastViewed = 0;  // ms since epoch, sort key
};

// Persistent UI state, stored as JSON in AppData (see Paths::stateFile()).
class AppState {
   public:
    AppState();

    // Loads state from disk; returns false when no valid file exists.
    bool load();

    // Writes state atomically to disk. Returns false on failure.
    bool save() const;

    // Window geometry
    QRect windowGeometry() const { return m_windowGeometry; }
    void setWindowGeometry(const QRect& r) { m_windowGeometry = r; }

    bool windowMaximized() const { return m_windowMaximized; }
    void setWindowMaximized(bool m) { m_windowMaximized = m; }

    // Viewer window
    QRect viewerGeometry() const { return m_viewerGeometry; }
    void setViewerGeometry(const QRect& r) { m_viewerGeometry = r; }

    double viewerLoupeZoom() const { return m_viewerLoupeZoom; }
    void setViewerLoupeZoom(double z) { m_viewerLoupeZoom = qBound(0.0, z, 16.0); }

    int viewerLoupeDiameter() const { return m_viewerLoupeDiameter; }
    void setViewerLoupeDiameter(int d) { m_viewerLoupeDiameter = qBound(200, d, 640); }

    // Main splitter
    QList<int> splitterSizes() const { return m_splitterSizes; }
    void setSplitterSizes(const QList<int>& sizes) { m_splitterSizes = sizes; }

    // Tabs
    QList<TabState> tabs() const { return m_tabs; }
    void setTabs(const QList<TabState>& tabs) { m_tabs = tabs; }

    int activeTab() const { return m_activeTab; }
    void setActiveTab(int index) { m_activeTab = index; }

    // Favorites
    QList<FavoriteEntry> favorites() const { return m_favorites; }
    void setFavorites(const QList<FavoriteEntry>& favorites) { m_favorites = favorites; }

    // Replaces by name if it exists, otherwise appends.
    void addFavorite(const FavoriteEntry& entry);

    void touchFavorite(const QString& name);

    void removeFavorite(const QString& name);

    // Thumbnail grid
    int thumbMaxSize() const { return m_thumbMaxSize; }
    void setThumbMaxSize(int v) { m_thumbMaxSize = v; }

    int thumbSpacing() const { return m_thumbSpacing; }
    void setThumbSpacing(int v) { m_thumbSpacing = v; }

    // Feed
    bool feedTabEnabled() const { return m_feedTabEnabled; }
    void setFeedTabEnabled(bool v) { m_feedTabEnabled = v; }

    // Feed candidate source ids (hidden knob, edit state.json by hand).
    QStringList feedSourceIds() const { return m_feedSourceIds; }
    void setFeedSourceIds(const QStringList& ids) { m_feedSourceIds = ids; }

    // Tag blacklist: tags always excluded from remote searches.
    QStringList blacklistTags() const;
    void setBlacklistTags(const QStringList& tags);

    // Library folder (empty = default; the DuckDB catalog lives inside it).
    QString libraryPath() const { return m_libraryPath; }
    void setLibraryPath(const QString& path) { m_libraryPath = path; }

    // Reads only the persisted library path (usable before load()).
    static QString readLibraryPath();

    // Site credentials: sourceId -> {user_id, api_key}.
    using SiteCredentials = QHash<QString, QPair<QString, QString>>;
    SiteCredentials siteCredentials() const { return m_siteCredentials; }
    void setSiteCredentials(const SiteCredentials& creds) { m_siteCredentials = creds; }

    // Reads only the persisted site credentials (usable before load()).
    static SiteCredentials readSiteCredentials();

    const QString& lastError() const { return m_lastError; }

   private:
    QRect m_windowGeometry;
    bool m_windowMaximized = false;
    QRect m_viewerGeometry;
    double m_viewerLoupeZoom = 2.0;
    int m_viewerLoupeDiameter = 250;
    QList<int> m_splitterSizes;
    QList<TabState> m_tabs;
    QList<FavoriteEntry> m_favorites;
    int m_activeTab = 0;
    int m_thumbMinSize = 120;
    int m_thumbMaxSize = 220;
    int m_thumbSpacing = 1;
    bool m_feedTabEnabled = true;
    QStringList m_feedSourceIds = {QStringLiteral("e621"), QStringLiteral("r34")};
    QSet<QString> m_blacklistTags;
    QString m_libraryPath;
    SiteCredentials m_siteCredentials;
    mutable QString m_lastError;
};

}  // namespace Mosaic
