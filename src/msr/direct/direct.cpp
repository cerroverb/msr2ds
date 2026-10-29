// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// SamusReturns-Direct.exe: starts SamusReturns.exe from its own folder with --play, which boots
// the game with the saved settings instead of showing the launcher (the launcher still shows
// while no game dump is set up). It waits for the game and returns its exit code, so Steam and
// other front ends see the game running for as long as it does, and stopping it stops the game.

#include <string>
#include <windows.h>

namespace {

/// What follows the program's own name on its command line: passed on to the game.
const wchar_t* Arguments() {
    const wchar_t* line = GetCommandLineW();
    if (*line == L'"') {
        line = wcschr(line + 1, L'"');
        line = line ? line + 1 : L"";
    } else {
        while (*line != L'\0' && *line != L' ' && *line != L'\t') {
            ++line;
        }
    }
    while (*line == L' ' || *line == L'\t') {
        ++line;
    }
    return line;
}

std::wstring OwnFolder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    path.erase(path.find_last_of(L'\\') + 1);
    return path;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring folder = OwnFolder();
    const std::wstring game = folder + L"SamusReturns.exe";
    std::wstring command = L"\"" + game + L"\" --play";
    if (const wchar_t* arguments = Arguments(); *arguments != L'\0') {
        command += L' ';
        command += arguments;
    }

    // The game runs in a job that closes with this program, so ending it (Steam's Stop) ends the
    // game too. What the game starts itself (a folder window) leaves the job.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, folder.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        const std::wstring message =
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? L"SamusReturns.exe was not found. Keep SamusReturns-Direct.exe in the game's "
                  L"folder, next to SamusReturns.exe."
                : L"SamusReturns.exe could not be started (error " + std::to_wstring(error) +
                      L").";
        MessageBoxW(nullptr, message.c_str(), L"Metroid: Samus Returns", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (job) {
        AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
