#pragma once

#include <QString>

namespace Mosaic {

// Appends every log message to stderr and to Mosaic.log in AppData.
// The file is append-only during a run; on boot, lines older than two
// days are pruned once.
class FileLogger {
   public:
    static void install();

    static QString filePath();
};

}  // namespace Mosaic
