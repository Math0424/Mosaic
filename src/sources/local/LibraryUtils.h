#pragma once

#include <QString>

namespace Mosaic {
namespace LibraryUtils {

bool isValidMd5(const QString& s);

// Sets the file's timestamps to the given epoch seconds.
bool setFileTimestamps(const QString& path, qint64 epochSeconds);

// Appends to import_failures.log in the library folder (best effort).
void logImportFailure(const QString& message);

}  // namespace LibraryUtils
}  // namespace Mosaic
