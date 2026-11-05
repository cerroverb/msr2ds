/* msr-native-direct.exe: the game without the launcher. The launcher writes it beside msr-native.exe once the native
 * code is built (launcher.cpp, the direct start: this program is in msr-native.exe as resource 200). It starts
 * msr-native.exe --direct from its own folder and waits for it, so a shortcut, Steam or a frontend sees the game run
 * until it ends; msr-native.exe --direct plays (the launcher's Play, its Restart too) or, while the game cannot start
 * yet, opens the launcher. Windows' libraries only (linked statically), so a copy left by an older version still works
 * with a newer msr-native.exe. */
#include <windows.h>

#define LONGEST 32768   /* a path's longest, in UTF-16 units */

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
    (void)instance; (void)previous; (void)command_line; (void)show;
    static wchar_t folder[LONGEST], program[LONGEST + 16], command[LONGEST + 32], text[LONGEST + 160];
    DWORD n = GetModuleFileNameW(NULL, folder, LONGEST);
    if (n == 0 || n >= LONGEST) return 1;
    while (n > 0 && folder[n - 1] != L'\\' && folder[n - 1] != L'/') n--;   /* the folder, with its separator */
    folder[n] = 0;
    lstrcpyW(program, folder);
    lstrcatW(program, L"msr-native.exe");
    lstrcpyW(command, L"\"");   /* a path has no quotes */
    lstrcatW(command, program);
    lstrcatW(command, L"\" --direct");
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(program, command, NULL, NULL, FALSE, 0, NULL, n ? folder : NULL, &si, &pi)) {
        lstrcpyW(text, L"msr-native.exe could not be started:\n");
        lstrcatW(text, program);
        lstrcatW(text, L"\n\nThis program starts the game with msr-native.exe from its own folder.");
        MessageBoxW(NULL, text, L"msr-native", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
