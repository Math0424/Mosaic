#include "FileLogger.h"

#include "Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

namespace Mosaic {

namespace {

// On boot the log is rotated to Mosaic.log.1 (replacing the previous one)
// once it is older than this or larger than kRotateBytes.
constexpr int kRotateDays = 2;
constexpr qint64 kRotateBytes = 10LL * 1024 * 1024;

QMutex s_mutex;
bool s_initialized = false;
QFile* s_file = nullptr;  // kept open; reopening per message was slow

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

/** Date of the log's first line; invalid when unreadable or undated. */
QDate firstLineDate(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QDate::fromString(QString::fromUtf8(file.readLine(64)).left(10),
                             QStringLiteral("yyyy-MM-dd"));
}

/**
 * Renames instead of rewriting in place: a crash mid-way can't lose the
 * log, and the previous run stays readable in Mosaic.log.1.
 */
void rotateIfStale() {
    const QString path = logFilePath();
    const QFileInfo info(path);
    if (!info.exists())
        return;
    const QDate first = firstLineDate(path);
    const bool old = first.isValid() && first < QDate::currentDate().addDays(-kRotateDays);
    if (!old && info.size() <= kRotateBytes)
        return;
    const QString previous = path + QStringLiteral(".1");
    QFile::remove(previous);
    QFile::rename(path, previous);
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    fprintf(stderr, "%s\n", qPrintable(msg));
    if (type == QtFatalMsg)
        fflush(stderr);

    const QString prefix =
        QStringLiteral("%1 [%2] ")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")),
                 QLatin1String(typeName(type)));
    const QString body = context.category && *context.category
                             ? QStringLiteral("%1: %2").arg(context.category, msg)
                             : msg;

    // Append-only and flushed per message, safe for live viewers. Every
    // line of a multi-line message carries the prefix.
    QMutexLocker lock(&s_mutex);
    if (s_file && s_file->isOpen()) {
        QByteArray out;
        for (const QStringView line : QStringView(body).split(QLatin1Char('\n')))
            out += (prefix + line.toString() + QLatin1Char('\n')).toUtf8();
        s_file->write(out);
        s_file->flush();
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
    rotateIfStale();
    s_file = new QFile(logFilePath());  // process lifetime
    if (!s_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        fprintf(stderr, "FileLogger: cannot open %s\n", qPrintable(logFilePath()));

    qInstallMessageHandler(handler);
}

QString FileLogger::filePath() { return logFilePath(); }

}  // namespace Mosaic
