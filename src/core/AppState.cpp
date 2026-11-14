#include "AppState.h"

#include "Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScreen>
#include <QDebug>

namespace Mosaic {

bool AppState::isGeometryOnScreen(const QRect& geometry) {
    if (geometry.width() <= 0 || geometry.height() <= 0)
        return false;
    const QRect titleBar(geometry.x(), geometry.y(), geometry.width(), 40);
    for (const QScreen* screen : QGuiApplication::screens()) {
        const QRect visible = screen->availableGeometry().intersected(titleBar);
        if (visible.width() >= 100 && visible.height() >= 20)
            return true;
    }
    return false;
}

namespace {
constexpr int kStateVersion = 1;

/**
 * The state file's root object, shared by load() and the static readers.
 * Returns false (with a reason) when missing, unreadable or corrupt.
 */
bool readStateRoot(QJsonObject* root, QString* error) {
    QFile file(Paths::stateFile());
    if (!file.exists()) {
        if (error)
            *error = QStringLiteral("No state file yet");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = parseError.errorString();
        return false;
    }
    *root = doc.object();
    return true;
}

AppState::SiteCredentials parseCredentials(const QJsonObject& root) {
    AppState::SiteCredentials credentials;
    const QJsonObject creds = root.value(QStringLiteral("siteCredentials")).toObject();
    for (auto it = creds.begin(); it != creds.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        const QString userId = c.value(QStringLiteral("user_id")).toString().trimmed();
        const QString apiKey = c.value(QStringLiteral("api_key")).toString().trimmed();
        if (!userId.isEmpty() && !apiKey.isEmpty())
            credentials.insert(it.key(), qMakePair(userId, apiKey));
    }
    return credentials;
}

}  // namespace

AppState::AppState() = default;

bool AppState::load() {
    QJsonObject root;
    if (!readStateRoot(&root, &m_lastError)) {
        if (QFile::exists(Paths::stateFile()))
            qWarning().noquote() << "AppState: cannot read state file:" << m_lastError;
        else
            qDebug().noquote() << "AppState: no state file yet (first run)";
        return false;
    }

    const QJsonObject win = root.value(QStringLiteral("window")).toObject();
    const QRect geo(win.value(QStringLiteral("x")).toInt(), win.value(QStringLiteral("y")).toInt(),
                    win.value(QStringLiteral("width")).toInt(1100),
                    win.value(QStringLiteral("height")).toInt(700));
    if (geo.width() > 0 && geo.height() > 0) {
        m_windowGeometry = geo;
    }
    m_windowMaximized = win.value(QStringLiteral("maximized")).toBool();

    const QJsonObject viewer = root.value(QStringLiteral("viewer")).toObject();
    const QRect viewerGeo(viewer.value(QStringLiteral("x")).toInt(),
                          viewer.value(QStringLiteral("y")).toInt(),
                          viewer.value(QStringLiteral("width")).toInt(0),
                          viewer.value(QStringLiteral("height")).toInt(0));
    if (viewerGeo.width() > 0 && viewerGeo.height() > 0)
        m_viewerGeometry = viewerGeo;
    m_viewerLoupeZoom = qBound(0.0, viewer.value(QStringLiteral("loupeZoom")).toDouble(2.0), 16.0);
    m_viewerLoupeDiameter =
        qBound(200, viewer.value(QStringLiteral("loupeDiameter")).toInt(250), 640);

    m_splitterSizes.clear();
    for (const auto& v : root.value(QStringLiteral("splitter")).toArray()) {
        m_splitterSizes << v.toInt();
    }

    m_tabs.clear();
    for (const auto& v : root.value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject t = v.toObject();
        TabState tab;
        tab.title = t.value(QStringLiteral("title")).toString();
        tab.sourceId = t.value(QStringLiteral("source")).toString();
        tab.multiSourceIds = t.value(QStringLiteral("multiSources")).toString();
        tab.tags = t.value(QStringLiteral("tags")).toString();
        tab.page = qMax(1, t.value(QStringLiteral("page")).toInt(1));
        m_tabs.append(tab);
    }
    m_activeTab = root.value(QStringLiteral("activeTab")).toInt(0);

    m_favorites.clear();
    for (const auto& v : root.value(QStringLiteral("favorites")).toArray()) {
        const QJsonObject f = v.toObject();
        FavoriteEntry entry;
        entry.name = f.value(QStringLiteral("name")).toString();
        entry.tags = f.value(QStringLiteral("tags")).toString();
        entry.sourceId = f.value(QStringLiteral("source")).toString();
        entry.multiSourceIds = f.value(QStringLiteral("multiSources")).toString();
        entry.lastViewed = f.value(QStringLiteral("lastViewed")).toVariant().toLongLong();
        if (!entry.name.isEmpty())
            m_favorites.append(entry);
    }

    const QJsonObject grid = root.value(QStringLiteral("grid")).toObject();
    m_thumbMinSize = qBound(48, grid.value(QStringLiteral("thumbMinSize")).toInt(120), 400);
    m_thumbMaxSize =
        qBound(m_thumbMinSize, grid.value(QStringLiteral("thumbMaxSize")).toInt(220), 600);
    m_thumbSpacing = qBound(0, grid.value(QStringLiteral("thumbSpacing")).toInt(1), 24);

    m_feedTabEnabled = root.value(QStringLiteral("feedTab")).toBool(true);
    m_feedAllowAnimated = root.value(QStringLiteral("feedAnimated")).toBool(false);
    m_feedVolume = qBound(0, root.value(QStringLiteral("feedVolume")).toInt(0), 100);

    // Hidden knob, edited by hand in state.json.
    if (root.contains(QStringLiteral("feedSources"))) {
        QStringList ids;
        for (const auto& v : root.value(QStringLiteral("feedSources")).toArray()) {
            const QString id = v.toString().trimmed();
            if (!id.isEmpty())
                ids << id;
        }
        if (!ids.isEmpty())
            m_feedSourceIds = ids;
    }

    m_libraryPath = root.value(QStringLiteral("libraryPath")).toString();

    const QJsonObject web = root.value(QStringLiteral("webServer")).toObject();
    m_webServerEnabled = web.value(QStringLiteral("enabled")).toBool(false);
    m_webServerPort = qBound(1024, web.value(QStringLiteral("port")).toInt(8787), 65535);
    m_webServerToken = web.value(QStringLiteral("token")).toString();

    m_blacklistTags.clear();
    for (const auto& v : root.value(QStringLiteral("blacklistTags")).toArray()) {
        const QString tag = v.toString().trimmed();
        if (!tag.isEmpty())
            m_blacklistTags.insert(tag);
    }

    m_siteCredentials = parseCredentials(root);

    m_lastError.clear();
    qDebug().noquote() << "AppState: loaded state with" << m_tabs.size() << "tab(s),"
                       << m_favorites.size() << "favorite(s)";
    return true;
}

QString AppState::readLibraryPath() {
    QJsonObject root;
    if (!readStateRoot(&root, nullptr))
        return {};
    return root.value(QStringLiteral("libraryPath")).toString();
}

AppState::SiteCredentials AppState::readSiteCredentials() {
    QJsonObject root;
    if (!readStateRoot(&root, nullptr))
        return {};
    return parseCredentials(root);
}

QStringList AppState::blacklistTags() const {
    QStringList list(m_blacklistTags.begin(), m_blacklistTags.end());
    list.sort();
    return list;
}

void AppState::setBlacklistTags(const QStringList& tags) {
    m_blacklistTags.clear();
    for (const QString& tag : tags) {
        const QString t = tag.trimmed();
        if (!t.isEmpty())
            m_blacklistTags.insert(t);
    }
}

bool AppState::save() const {
    Paths::ensureCreated();

    QJsonObject win;
    win[QStringLiteral("x")] = m_windowGeometry.x();
    win[QStringLiteral("y")] = m_windowGeometry.y();
    win[QStringLiteral("width")] = m_windowGeometry.width();
    win[QStringLiteral("height")] = m_windowGeometry.height();
    win[QStringLiteral("maximized")] = m_windowMaximized;

    QJsonObject viewer;
    viewer[QStringLiteral("x")] = m_viewerGeometry.x();
    viewer[QStringLiteral("y")] = m_viewerGeometry.y();
    viewer[QStringLiteral("width")] = m_viewerGeometry.width();
    viewer[QStringLiteral("height")] = m_viewerGeometry.height();
    viewer[QStringLiteral("loupeZoom")] = m_viewerLoupeZoom;
    viewer[QStringLiteral("loupeDiameter")] = m_viewerLoupeDiameter;

    QJsonArray splitter;
    for (const int s : m_splitterSizes) splitter.append(s);

    QJsonArray tabs;
    for (const auto& t : m_tabs) {
        QJsonObject o;
        o[QStringLiteral("title")] = t.title;
        if (!t.sourceId.isEmpty())
            o[QStringLiteral("source")] = t.sourceId;
        if (!t.multiSourceIds.isEmpty())
            o[QStringLiteral("multiSources")] = t.multiSourceIds;
        if (!t.tags.isEmpty())
            o[QStringLiteral("tags")] = t.tags;
        if (t.page > 1)
            o[QStringLiteral("page")] = t.page;
        tabs.append(o);
    }

    QJsonObject grid;
    grid[QStringLiteral("thumbMinSize")] = m_thumbMinSize;
    grid[QStringLiteral("thumbMaxSize")] = m_thumbMaxSize;
    grid[QStringLiteral("thumbSpacing")] = m_thumbSpacing;
    QJsonObject root;
    root[QStringLiteral("version")] = kStateVersion;
    root[QStringLiteral("savedAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("window")] = win;
    if (m_viewerGeometry.width() > 0 && m_viewerGeometry.height() > 0)
        root[QStringLiteral("viewer")] = viewer;
    root[QStringLiteral("splitter")] = splitter;
    QJsonArray favorites;
    for (const auto& f : m_favorites) {
        QJsonObject o;
        o[QStringLiteral("name")] = f.name;
        o[QStringLiteral("tags")] = f.tags;
        o[QStringLiteral("source")] = f.sourceId;
        if (!f.multiSourceIds.isEmpty())
            o[QStringLiteral("multiSources")] = f.multiSourceIds;
        o[QStringLiteral("lastViewed")] = f.lastViewed;
        favorites.append(o);
    }

    QJsonArray blacklist;
    for (const QString& tag : blacklistTags()) blacklist.append(tag);

    QJsonObject creds;
    for (auto it = m_siteCredentials.begin(); it != m_siteCredentials.end(); ++it) {
        QJsonObject c;
        c[QStringLiteral("user_id")] = it.value().first;
        c[QStringLiteral("api_key")] = it.value().second;
        creds[it.key()] = c;
    }

    root[QStringLiteral("activeTab")] = m_activeTab;
    root[QStringLiteral("tabs")] = tabs;
    root[QStringLiteral("favorites")] = favorites;
    root[QStringLiteral("grid")] = grid;
    root[QStringLiteral("feedTab")] = m_feedTabEnabled;
    root[QStringLiteral("feedAnimated")] = m_feedAllowAnimated;
    root[QStringLiteral("feedVolume")] = m_feedVolume;
    QJsonArray feedSources;
    for (const QString& id : m_feedSourceIds) feedSources.append(id);
    root[QStringLiteral("feedSources")] = feedSources;
    root[QStringLiteral("blacklistTags")] = blacklist;
    root[QStringLiteral("siteCredentials")] = creds;
    QJsonObject web;
    web[QStringLiteral("enabled")] = m_webServerEnabled;
    web[QStringLiteral("port")] = m_webServerPort;
    if (!m_webServerToken.isEmpty())
        web[QStringLiteral("token")] = m_webServerToken;
    root[QStringLiteral("webServer")] = web;
    if (!m_libraryPath.isEmpty())
        root[QStringLiteral("libraryPath")] = m_libraryPath;

    // QSaveFile renames a temp file into place, so a crash can't leave a
    // corrupt state.json behind.
    QSaveFile file(Paths::stateFile());
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = file.errorString();
        qWarning().noquote() << "AppState: cannot save state:" << m_lastError;
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        m_lastError = file.errorString();
        qWarning().noquote() << "AppState: state commit failed:" << m_lastError;
        return false;
    }

    m_lastError.clear();
    return true;
}

void AppState::addFavorite(const FavoriteEntry& entry) {
    for (auto& f : m_favorites) {
        if (f.name == entry.name) {
            f = entry;
            return;
        }
    }
    m_favorites.append(entry);
}

void AppState::touchFavorite(const QString& name) {
    for (auto& f : m_favorites) {
        if (f.name == name) {
            f.lastViewed = QDateTime::currentMSecsSinceEpoch();
            return;
        }
    }
}

void AppState::removeFavorite(const QString& name) {
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites[i].name == name) {
            m_favorites.removeAt(i);
            return;
        }
    }
}

}  // namespace Mosaic
