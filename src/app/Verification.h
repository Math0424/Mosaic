#pragma once

#include <QString>
#include <QStringList>

namespace Mosaic {

struct CheckResult {
    QString name;
    bool ok = false;
    QString detail;
};

// Stack verification: Qt 6, GPU/RHI, OpenCV, FFmpeg, DuckDB.
class Verification {
   public:
    static QList<CheckResult> runAll();
    static QString formatReport(const QList<CheckResult>& results);
    static bool allPassed(const QList<CheckResult>& results);

   private:
    static CheckResult checkQt();
    static CheckResult checkRhi();
    static CheckResult checkOpenCV();
    static CheckResult checkFFmpeg();
    static CheckResult checkDuckDB();
};

}  // namespace Mosaic
