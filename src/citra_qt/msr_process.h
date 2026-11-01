// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Helpers for the command-line programs the launcher runs: curl and tar for downloads and the
// update check, Randovania for the randomizer.

#pragma once

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

namespace MSR::Process {

/// Windows has had curl and tar (bsdtar, which unpacks zip files) since Windows 10 1803: the
/// System32 ones, not whichever an MSYS or Git install puts first in the PATH. Empty when there
/// is none.
inline QString SystemTool(const QString& name) {
#ifdef _WIN32
    const QString path = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")) +
                         QStringLiteral("\\System32\\") + name + QStringLiteral(".exe");
    return QFile::exists(path) ? path : QStandardPaths::findExecutable(name);
#else
    return QStandardPaths::findExecutable(name);
#endif
}

/// No console window for a console program the launcher starts (Windows).
inline void HideConsole(QProcess& process) {
#ifdef _WIN32
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        // CREATE_NO_WINDOW, which CREATE_NEW_CONSOLE would override.
        args->flags = (args->flags & ~0x00000010u) | 0x08000000u;
    });
#else
    (void)process;
#endif
}

} // namespace MSR::Process
