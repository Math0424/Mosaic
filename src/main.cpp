#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QTextStream>

#include "BuildNumber.h"
#include "app/InstanceGuard.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/Verification.h"
#include "core/AppState.h"
#include "core/Database.h"
#include "core/FileLogger.h"
#include "core/Paths.h"

namespace {

// Startup self-tests (GL context, codecs, DuckDB) cost a noticeable delay,
// so a normal launch runs them only until a build has passed once. The
// marker holds the last passing build; a failure removes it so the next
// launch checks again. --selftest always runs them.
QString selfTestMarkerPath() {
    return Mosaic::Paths::appDataDir() + QStringLiteral("/selftest.ok");
}

QByteArray currentBuildId() {
    return QCoreApplication::applicationVersion().toUtf8() + '+' +
           QByteArray::number(Mosaic_BUILD_NUMBER);
}

bool selfTestPassedForThisBuild() {
    QFile marker(selfTestMarkerPath());
    return marker.open(QIODevice::ReadOnly) && marker.readAll().trimmed() == currentBuildId();
}

void recordSelfTestResult(bool passed) {
    if (!passed) {
        QFile::remove(selfTestMarkerPath());
        return;
    }
    Mosaic::Paths::ensureCreated();
    QFile marker(selfTestMarkerPath());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(currentBuildId());
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Math"));
    QCoreApplication::setApplicationName(QStringLiteral("Mosaic"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MOSAIC_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.svg")));

    Mosaic::FileLogger::install();

    qInfo().noquote() << "=== Mosaic" << QCoreApplication::applicationVersion() << "starting ===";

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Mosaic - bulk image viewing and downloading platform"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption selfTestOpt(
        QStringList{QStringLiteral("s"), QStringLiteral("selftest")},
        QStringLiteral("Run stack verification headless and exit (code 0 = all pass)."));
    parser.addOption(selfTestOpt);
    parser.process(app);

    QTextStream out(stdout);

    if (parser.isSet(selfTestOpt)) {
        const auto results = Mosaic::Verification::runAll();
        out << Mosaic::Verification::formatReport(results) << '\n';
        out.flush();
        return Mosaic::Verification::allPassed(results) ? 0 : 1;
    }

    if (selfTestPassedForThisBuild()) {
        qDebug().noquote() << "Self-tests skipped (passed before for this build)";
    } else if (const auto results = Mosaic::Verification::runAll();
               !Mosaic::Verification::allPassed(results)) {
        recordSelfTestResult(false);
        QTextStream err(stderr);
        for (const auto& r : results) {
            if (!r.ok) {
                qWarning().noquote() << "Self-test failed:" << r.name << "--" << r.detail;
                err << "[FAIL] " << r.name << ": " << r.detail << '\n';
            }
        }
        err.flush();
    } else {
        recordSelfTestResult(true);
        qDebug().noquote() << "Self-tests passed";
    }

    const QString libraryPath = Mosaic::AppState::readLibraryPath();
    if (!libraryPath.isEmpty()) {
        qInfo().noquote() << "Library path override:" << libraryPath;
        Mosaic::Paths::setLibraryDirOverride(libraryPath);
    }
    Mosaic::Paths::ensureCreated();

    // One-time migration from old builds that kept Mosaic.duckdb next to state.json.
    const QString legacyDb = Mosaic::Paths::appDataDir() + QStringLiteral("/Mosaic.duckdb");
    if (QFile::exists(legacyDb) && !QFile::exists(Mosaic::Paths::databaseFile())) {
        qInfo().noquote() << "Migrating legacy database to library folder";
        QFile::rename(legacyDb, Mosaic::Paths::databaseFile());
    }

    Mosaic::Database& db = Mosaic::Database::instance();
    if (!db.open()) {
        const QStringList others = Mosaic::InstanceGuard::otherInstances();
        QString message = QStringLiteral(
            "The library database is busy and could not be opened.\n"
            "Is another instance of Mosaic running?");
        if (!others.isEmpty())
            message += QStringLiteral("\n\nOther running instance(s):\n%1")
                           .arg(others.join(QStringLiteral("\n")));
        else
            message += QStringLiteral("\n\n%1").arg(db.lastError());

        QTextStream err(stderr);
        err << "ERROR: " << message << '\n';
        err.flush();
        qCritical().noquote() << "Database open failed at startup:" << db.lastError()
                              << "-- other instances:" << others.join(QStringLiteral(", "));
        QMessageBox::critical(nullptr, QStringLiteral("Mosaic - database busy"), message);
        return 1;
    }

    const QStringList otherInstances = Mosaic::InstanceGuard::otherInstances();
    if (!otherInstances.isEmpty()) {
        const QString message = QStringLiteral(
                                    "Another Mosaic instance is already running:\n%1\n\n"
                                    "Only one instance can use the library at a time.")
                                    .arg(otherInstances.join(QStringLiteral("\n")));
        qCritical().noquote() << "Refusing to start: other Mosaic instance(s):"
                              << otherInstances.join(QStringLiteral(", "));
        QTextStream err(stderr);
        err << "ERROR: " << message << '\n';
        err.flush();
        QMessageBox::critical(nullptr, QStringLiteral("Mosaic - already running"), message);
        return 1;
    }

    Mosaic::Theme::apply();

    Mosaic::MainWindow window(db);
    window.show();
    const int code = app.exec();
    qInfo().noquote() << "=== Mosaic exited with code" << code << "===";
    return code;
}
