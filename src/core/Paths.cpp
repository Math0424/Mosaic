#include "Paths.h"

#include <QDir>
#include <QStandardPaths>

namespace Mosaic {

namespace {

QString makeDir(const QString& path) {
    QDir().mkpath(path);
    return QDir::toNativeSeparators(path);
}

QString g_libraryDirOverride;

}  // namespace

QString Paths::appDataDir() {
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation);
    return dirs.isEmpty() ? QDir::homePath() + QStringLiteral("/.mosaic") : dirs.first();
}

QString Paths::databaseFile() { return libraryDir() + QStringLiteral("/mosaic.duckdb"); }

QString Paths::libraryDir() {
    if (!g_libraryDirOverride.isEmpty())
        return g_libraryDirOverride;
    return appDataDir() + QStringLiteral("/library");
}

void Paths::setLibraryDirOverride(const QString& dir) { g_libraryDirOverride = dir; }

QString Paths::stateFile() { return appDataDir() + QStringLiteral("/state.json"); }

QString Paths::tempDir() { return QDir::tempPath() + QStringLiteral("/mosaic"); }

void Paths::ensureCreated() {
    makeDir(appDataDir());
    makeDir(libraryDir());
    makeDir(tempDir());
}

}  // namespace Mosaic
