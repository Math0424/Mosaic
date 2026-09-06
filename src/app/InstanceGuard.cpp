#include "InstanceGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Mosaic {
namespace InstanceGuard {

QStringList otherInstances() {
    QStringList result;
#ifdef Q_OS_WIN
    const QString selfExe = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == selfPid)
                continue;
            const QString name = QString::fromWCharArray(entry.szExeFile);
            if (name.compare(selfExe, Qt::CaseInsensitive) != 0)
                continue;

            HANDLE process =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                result << name;
                continue;
            }
            wchar_t buffer[4096];
            DWORD size = 4096;
            if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                result << QDir::toNativeSeparators(QString::fromWCharArray(buffer, size));
            } else {
                result << name;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
#endif
    return result;
}

}  // namespace InstanceGuard
}  // namespace Mosaic
