#pragma once

#include <QString>

#include <functional>

class QFileInfo;

namespace Mosaic {

// Bulk import of media files into the library: legacy folder imports
// (md5.ext + sidecars) and scans for uncataloged strays. Metadata per file
// resolves from: remote lookup by md5, .json sidecar, .tags sidecar, or a
// "usergroup:notags" marker.
class LibraryImporter {
   public:
    enum class Outcome { Imported, Skipped, Failed };

    LibraryImporter() = default;

    int importFolder(const QString& folder, int* skipped = nullptr,
                     const std::function<void(int, int)>& progress = nullptr,
                     QString* error = nullptr, bool markFavorite = false,
                     const QString& userTag = QString());

    // Scans the library folder for uncataloged media; strays are renamed to
    // their content md5. Synchronous - call from a worker thread.
    int scanLibrary(const std::function<void(int, int)>& progress = nullptr,
                    QString* error = nullptr);

   private:
    // Shared by importFolder and scanLibrary; with removeOriginal the
    // source file is deleted once its md5-named copy exists.
    Outcome importOneFile(const QFileInfo& fi, bool markFavorite, const QString& userTag,
                          bool removeOriginal);
};

}  // namespace Mosaic
