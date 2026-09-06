#include "FileLogger.h"

#include "Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStringList>
#include <QTextStream>

namespace Mosaic {

namespace {

// Lines older than this are dropped on boot.
constexpr int kPruneDays = 2;

QMutex s_mutex;
bool s_initialized = false;

QString logFilePath() { return Paths::appDataDir() + QStringLiteral("/Mosaic.log"); }

const char* typeName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg:
            return "DEBUG";
        case QtInfoMsg:
            return "INFO";
        case QtWarningMsg:
            return "WARN";
        case QtCriticalMsg:
            return "CRIT";
        case QtFatalMsg:
            return "FATAL";
    }
    return "?";
}

void pruneOldLines() {
    QFile file(logFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();

    const QDate cutoff = QDate::currentDate().addDays(-kPruneDays);
    QStringList kept;
    kept.reserve(lines.size());
    for (const QString& line : lines) {
        const QDate lineDate = QDate::fromString(line.left(10), QStringLiteral("yyyy-MM-dd"));
        if (!lineDate.isValid() || lineDate >= cutoff)  // undated lines are kept
            kept.append(line);
    }
    while (!kept.isEmpty() && kept.last().isEmpty()) kept.removeLast();
    if (kept.size() == lines.size())
        return;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    QTextStream out(&file);
    for (const QString& line : kept) out << line << '\n';
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    fprintf(stderr, "%s\n", qPrintable(msg));
    if (type == QtFatalMsg)
        fflush(stderr);

    const QString line =
        QStringLiteral("%1 [%2] %3")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")),
                 QLatin1String(typeName(type)),
                 context.category && *context.category
                     ? QStringLiteral("%1: %2").arg(context.category, msg)
                     : msg);

    // Append-only, safe for live viewers.
    QMutexLocker lock(&s_mutex);
    QFile file(logFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << line << '\n';
    }

    if (type == QtFatalMsg)
        abort();
}

}  // namespace

void FileLogger::install() {
    QMutexLocker lock(&s_mutex);
    if (s_initialized)
        return;
    s_initialized = true;

    Paths::ensureCreated();
    pruneOldLines();

    qInstallMessageHandler(handler);
}

QString FileLogger::filePath() { return logFilePath(); }

}  // namespace Mosaic
