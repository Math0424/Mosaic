#include "LibraryUtils.h"

#include "../../core/Paths.h"

#include <QDateTime>
#include <QFile>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <utime.h>
#endif

namespace Mosaic {
namespace LibraryUtils {

bool isValidMd5(const QString& s) {
    if (s.size() != 32)
        return false;
    for (QChar c : s) {
        const QChar lc = c.toLower();
        if (!((lc >= QLatin1Char('0') && lc <= QLatin1Char('9')) ||
              (lc >= QLatin1Char('a') && lc <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

bool setFileTimestamps(const QString& path, qint64 epochSeconds) {
#ifdef Q_OS_WIN
    // Windows FILETIME is 100-nanosecond intervals since 1601-01-01
    const unsigned long long WINDOWS_EPOCH_DIFF = 11644473600ULL;
    unsigned long long intervals =
        (static_cast<unsigned long long>(epochSeconds) + WINDOWS_EPOCH_DIFF) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(intervals & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>((intervals >> 32) & 0xFFFFFFFFULL);
    const std::wstring wpath = reinterpret_cast<const wchar_t*>(path.utf16());
    HANDLE h = CreateFileW(wpath.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const BOOL ok = SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
    return ok == TRUE;
#else
    struct utimbuf times;
    times.actime = epochSeconds;
    times.modtime = epochSeconds;
    return utime(path.toLocal8Bit().constData(), &times) == 0;
#endif
}

void logImportFailure(const QString& message) {
    const QString logPath = Paths::libraryDir() + QLatin1String("/import_failures.log");
    QFile log(logPath);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char(' ') +
                         message + QLatin1Char('\n');
    log.write(line.toUtf8());
    log.close();
}

}  // namespace LibraryUtils
}  // namespace Mosaic
