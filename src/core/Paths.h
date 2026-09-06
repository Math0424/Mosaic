#pragma once

#include <QString>

namespace Mosaic {

// Well-known runtime directories (AppData, temp, library).
// All created on demand by ensureCreated().
class Paths {
   public:
    static QString appDataDir();

    static QString databaseFile();

    // Flat image library folder. Defaults to the app data dir; a custom
    // location (persisted in state.json) is set via setLibraryDirOverride().
    // Files are copied here named by MD5; the DuckDB catalog lives here too.
    static QString libraryDir();

    static void setLibraryDirOverride(const QString& dir);

    static QString stateFile();

    static QString tempDir();

    static void ensureCreated();
};

}  // namespace Mosaic
