// msr-native's launcher: what the program shows when it is started on its own (no options). It is the settings menu
// (src/msr/overlay.cpp, the same as in play) with Play, the game file and the native code on its Game tab:
//   - the game file: the player's decrypted Samus Returns (USA) dump, typed in or chosen with the system's file dialog,
//     kept in user/msr/game-path.txt;
//   - the native code: the game's code recompiled from that dump (tools/msr/recomp/build_module.py, shipped in recomp/),
//     built once into user/msr/recomp/. The build tools (Python on Windows, or on Linux when the system has none; Zig;
//     capstone) are downloaded on the first build with curl, pinned by SHA-256, into user/msr/recomp-kit/, as the
//     emulator-based build's launcher does (its msr_recomp.cpp);
//   - Play: this program again, with --play and the display settings (settings.ini [native]), in the same folder; the
//     launcher hides until it ends and starts it again when its menu's Restart asks (exit code 75);
//   - updates: a newer release on GitHub, offered when the launcher starts and put in place of this installation's own
//     files (the updates section below);
//   - the direct start: once the native code is built, msr-native-direct.exe beside the program (msr-native-direct.sh
//     beside the AppImage) starts the game without the launcher (the direct start section below).
// The user folder: beside the program (Windows, a portable folder; Linux when a user/ folder is there, also beside the
// AppImage), else the system's (%LOCALAPPDATA%\msr-native, ~/.local/share/msr-native); MSR_NATIVE_HOME overrides it.
// The Android app (native/android): the user folder is the app's on the shared storage (Android/data/<app>/files/user),
// which a PC reaches over USB. Nothing is built there: the PC's launcher builds the Android native code (Build for
// Android: build_module.py --target android-arm64, into user/msr/recomp-android/), which the player copies into the
// same folder of the app's, with the game file (found in the user folder). Play starts the app again as the game
// (android.c); there are no file dialogs, folders to open, updates or direct start there.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <SDL_syswm.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include "android.h"
#endif
#include "dump.h"
#include "ini.h"
#include "mods.h"
#include "msr.h"
#include "msr/overlay.h"

#ifndef MSR_NATIVE_VERSION
#define MSR_NATIVE_VERSION "dev"
#endif
#ifndef MSR_UPDATE_REPO
#define MSR_UPDATE_REPO "gminustwo/msr-native"
#endif

extern "C" {
int video_init(int scale);
int video_frame(void);
void video_shutdown(void);
void video_show(int on);
SDL_Window* video_window(void);
int video_option(const char* name, const char* value);
void msr_overlay_start(int fullscreen, int resolution, int vsync, int volume);
void msr_overlay_launcher(int on);
void msr_overlay_reload(void);
void msr_mods_init(const char* user, const char* settings, const char* exe);
}

namespace {

namespace fs = std::filesystem;
using Snapshot = MSR::LauncherSnapshot;
using Code = Snapshot::Code;

#ifdef _WIN32
constexpr const char* Library = "msr_aot.dll";
#else
constexpr const char* Library = "libmsr_aot.so";
#endif

// ---- text and paths ----
std::string Utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
#endif
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    s.erase(0, i);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);   // pasted with quotes
    return s;
}
std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool WriteText(const fs::path& p, const std::string& text) {
    std::error_code e;
    fs::create_directories(p.parent_path(), e);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return static_cast<bool>(f);
}
// "key": "value" or "key": number in a flat JSON file
std::string JsonValue(const std::string& text, const char* key) {
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return {};
    auto v = text.find(':', at);
    if (v == std::string::npos) return {};
    v = text.find_first_not_of(" \t\r\n", v + 1);
    if (v == std::string::npos) return {};
    if (text[v] == '"') {
        const auto end = text.find('"', v + 1);
        return end == std::string::npos ? std::string{} : text.substr(v + 1, end - v - 1);
    }
    const auto end = text.find_first_of(",}\r\n", v);
    return Trim(text.substr(v, end == std::string::npos ? std::string::npos : end - v));
}
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

fs::path ExePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n));
#else
    std::error_code e;
    return fs::read_symlink("/proc/self/exe", e);
#endif
}

bool Writable(const fs::path& dir) {
    std::error_code e;
    fs::create_directories(dir, e);
    const fs::path probe = dir / ".msr-native-write-test";
    { std::ofstream f(probe); if (!f) return false; }
    fs::remove(probe, e);
    return true;
}

// The folder that holds user/ (and that the game runs in)
fs::path DataRoot() {
    if (const char* home = std::getenv("MSR_NATIVE_HOME"); home && *home) return FromUtf8(home);
#ifdef __ANDROID__
    return FromUtf8(msr_android_root());   // Android/data/<app>/files: reachable from a PC over USB
#endif
    const fs::path exe_dir = ExePath().parent_path();
#ifdef _WIN32
    if (Writable(exe_dir / "user")) return exe_dir;   // portable, as the zip unpacks
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return n ? fs::path(std::wstring(buf, n)) / "msr-native" : exe_dir;
#else
    std::error_code e;
    if (const char* image = std::getenv("APPIMAGE"); image && *image) {
        const fs::path beside = FromUtf8(image).parent_path();
        if (fs::is_directory(beside / "user", e)) return beside;   // portable: a user/ folder beside the AppImage
    } else if (fs::is_directory(exe_dir / "user", e)) {
        return exe_dir;
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return FromUtf8(xdg) / "msr-native";
    const char* h = std::getenv("HOME");
    return FromUtf8(h ? h : ".") / ".local" / "share" / "msr-native";
#endif
}

// The recompiler shipped with the program: recomp/ beside it (Windows), ../share/msr-native/recomp (the AppImage), the
// source tree above a build folder (development), or MSR_RECOMP_SCRIPTS
fs::path Scripts() {
    const fs::path rel = fs::path("tools") / "msr" / "recomp" / "build_module.py";
    const fs::path exe_dir = ExePath().parent_path();
    std::vector<fs::path> roots;
    if (const char* dev = std::getenv("MSR_RECOMP_SCRIPTS"); dev && *dev) roots.push_back(FromUtf8(dev));
    roots.push_back(exe_dir / "recomp");
    roots.push_back(exe_dir / ".." / "share" / "msr-native" / "recomp");
    roots.push_back(exe_dir / "..");
    std::error_code e;
    for (const auto& r : roots)
        if (fs::is_regular_file(r / rel, e)) return fs::weakly_canonical(r / rel, e);
    return {};
}

// ---- SHA-256 (FIPS 180-4), for the downloads ----
std::string Sha256(const fs::path& path) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    const auto block = [&](const unsigned char* p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = std::uint32_t{p[4 * i]} << 24 | std::uint32_t{p[4 * i + 1]} << 16 | std::uint32_t{p[4 * i + 2]} << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<unsigned char> buf(1 << 20);
    std::uint64_t total = 0;
    unsigned char tail[128];
    std::size_t pending = 0;
    while (file) {
        file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        std::size_t n = static_cast<std::size_t>(file.gcount()), i = 0;
        total += n;
        if (pending) {
            const std::size_t take = std::min(n, 64 - pending);
            std::memcpy(tail + pending, buf.data(), take);
            pending += take; i = take;
            if (pending == 64) { block(tail); pending = 0; }
        }
        for (; i + 64 <= n; i += 64) block(buf.data() + i);
        if (i < n) { std::memcpy(tail + pending, buf.data() + i, n - i); pending += n - i; }
    }
    tail[pending++] = 0x80;
    if (pending > 56) { std::memset(tail + pending, 0, 64 - pending); block(tail); pending = 0; }
    std::memset(tail + pending, 0, 56 - pending);
    for (int i = 0; i < 8; i++) tail[56 + i] = static_cast<unsigned char>((total * 8) >> (56 - 8 * i));
    block(tail);
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

// ---- child processes: the tools (no console, output read line by line or to a file) and the game ----
struct Child {
#ifdef _WIN32
    HANDLE process = nullptr, out = nullptr;
#else
    pid_t pid = -1;
    int out = -1;
#endif
};

#ifdef _WIN32
std::wstring Quote(const std::string& arg) {   // CommandLineToArgvW's rules
    std::wstring w = Wide(arg), q = L"\"";
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::size_t slashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
#endif

// capture: stdout and stderr to a pipe (read with ReadLines); else to `log` (appended), or nowhere
bool Spawn(const std::vector<std::string>& argv, const fs::path& cwd, bool capture, const fs::path& log, bool tool,
           Child& child) {
#ifdef _WIN32
    std::wstring cmd;
    for (const auto& a : argv) { if (!cmd.empty()) cmd += L' '; cmd += Quote(a); }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (capture) {
        if (!CreatePipe(&read, &write, &sa, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    } else if (!log.empty()) {
        write = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write == INVALID_HANDLE_VALUE) write = nullptr;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = write ? write : nul;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, tool ? CREATE_NO_WINDOW : 0, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (write) CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { if (read) CloseHandle(read); return false; }
    CloseHandle(pi.hThread);
    child.process = pi.hProcess;
    child.out = read;
    return true;
#else
    int fds[2] = {-1, -1};
    if (capture && pipe(fds)) return false;
    const pid_t pid = fork();
    if (pid < 0) { if (capture) { close(fds[0]); close(fds[1]); } return false; }
    if (pid == 0) {
        int out = -1;
        if (capture) out = fds[1];
        else if (!log.empty()) out = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (out < 0) out = open("/dev/null", O_WRONLY);
        const int in = open("/dev/null", O_RDONLY);
        dup2(in, 0); dup2(out, 1); dup2(out, 2);
        if (capture) close(fds[0]);
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(127);
        if (tool) unsetenv("LD_LIBRARY_PATH");   // the AppImage's libraries are not the tools'
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    if (capture) close(fds[1]);
    child.pid = pid;
    child.out = capture ? fds[0] : -1;
    return true;
#endif
}

// the captured output, a line at a time, until the process closes it
void ReadLines(Child& c, const std::function<void(const std::string&)>& line) {
    std::string pending;
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!c.out || !ReadFile(c.out, buf, sizeof buf, &n, nullptr) || !n) break;
#else
        const ssize_t n = c.out >= 0 ? read(c.out, buf, sizeof buf) : -1;
        if (n <= 0) break;
#endif
        pending.append(buf, static_cast<std::size_t>(n));
        std::size_t end;
        while ((end = pending.find('\n')) != std::string::npos) {
            line(Trim(pending.substr(0, end)));
            pending.erase(0, end + 1);
        }
    }
    if (!pending.empty()) line(Trim(pending));
}

// -1 while running; the exit code once it ended. The process stays (its handle, its zombie) until Close, so Kill
// never reaches another process.
int Poll(Child& c, bool wait) {
#ifdef _WIN32
    if (!c.process) return -2;
    if (WaitForSingleObject(c.process, wait ? INFINITE : 0) != WAIT_OBJECT_0) return -1;
    DWORD code = 1;
    GetExitCodeProcess(c.process, &code);
    return static_cast<int>(code);
#else
    if (c.pid < 0) return -2;
    siginfo_t info{};
    if (waitid(P_PID, static_cast<id_t>(c.pid), &info, WEXITED | WNOWAIT | (wait ? 0 : WNOHANG)) != 0) return -2;
    if (info.si_pid == 0) return -1;
    return info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
#endif
}

void Close(Child& c) {
#ifdef _WIN32
    if (c.process) CloseHandle(c.process);
    if (c.out) CloseHandle(c.out);
    c.process = c.out = nullptr;
#else
    if (c.pid > 0) waitpid(c.pid, nullptr, 0);
    if (c.out >= 0) close(c.out);
    c.pid = -1;
    c.out = -1;
#endif
}

void Kill(Child& c) {
#ifdef _WIN32
    if (c.process) TerminateProcess(c.process, 1);
#else
    if (c.pid > 0) kill(c.pid, SIGTERM);
#endif
}

// ---- the build tools: each from its own project, pinned by SHA-256 (as msr_recomp.cpp) ----
struct Download {
    const char* what;
    const char* url;
    const char* sha256;
    std::uint64_t size;   // for the progress; 0 when unknown
    const char* file;
};
constexpr int KitVersion = 2;   // 2: Zig keeps the Android app's C headers (build_module.py ZIG_HEADERS_ANDROID)
#ifdef _WIN32
constexpr Download Downloads[] = {
    {"Python 3.12", "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", 11133606, "python.zip"},
    {"Zig 0.15.2", "https://ziglang.org/download/0.15.2/zig-x86_64-windows-0.15.2.zip",
     "3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c", 92614574, "zig.zip"},
    {"capstone 5.0.9",
     "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl",
     "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0", 1273459, "capstone.whl"},
};
constexpr const char* ZigArchive = "zig.zip";
#else
constexpr Download Downloads[] = {
    // only when the system has no Python 3.8 or newer (SteamOS and most desktops have one)
    {"Python 3.12",
     "https://github.com/astral-sh/python-build-standalone/releases/download/20261009/cpython-3.12.15%2B20261009-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz",
     "ffa8f85f1b56e88b687f08d743d817015524b86a9365fd921243132b80f1a36d", 0, "python.tar.gz"},
    {"Zig 0.15.2", "https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz",
     "02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239", 53733924, "zig.tar.xz"},
    {"capstone 5.0.9",
     "https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
     "273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e", 1485316, "capstone.whl"},
};
constexpr const char* ZigArchive = "zig.tar.xz";
#endif

std::string SystemTool(const char* name) {
#ifdef _WIN32
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);   // curl.exe and tar.exe come with Windows 10 and 11
    const fs::path p = fs::path(std::wstring(dir, n)) / (std::string(name) + ".exe");
    std::error_code e;
    return fs::is_regular_file(p, e) ? Utf8(p) : std::string{};
#else
    return name;   // found in PATH by execvp
#endif
}

// ---- the launcher's state ----
struct State {
    std::mutex lock;
    fs::path root, user, settings;
    std::string game, game_note;
    Code code = Code::Missing;
    std::string code_note;
    float progress = -1.f;
    bool building = false, cancel = false;
    std::thread worker;
    Child tool;           // the build's current process
    bool playing = false;
    Child game_process;
    bool quit = false;
    bool dirty = true;    // the overlay's snapshot needs updating
    // a newer release (the updates section below)
    Snapshot::Update update = Snapshot::Update::None;
    std::string update_version, update_note, update_page, update_url, update_sha256;
    std::uint64_t update_size = 0;
    float update_progress = -1.f;
    bool update_installable = false;
    std::thread update_checker, update_worker;
    Child update_tool;       // the update's current process (curl, tar): stopped when the launcher closes
    fs::path restart_into;   // after the window closes: this program's new version (an update)
    std::string direct;      // the direct start's program, written beside this one (its file name), or empty
    bool android_build = false;   // the build under way is the Android app's (Build for Android)
    Code code_before = Code::Missing;   // the native code's state before it, shown again after it
    std::string android_note;     // the Android build's result: where it is, or what failed
} S;

void WriteDirect();   // the direct start section

// The native code: recomp/ here, recomp-android/ in the Android app (the PC's Build for Android makes that folder,
// which the player copies over with its module.json)
#ifdef __ANDROID__
constexpr const char* CodeFolder = "recomp-android";
#else
constexpr const char* CodeFolder = "recomp";
#endif
fs::path KitFolder() { return S.user / "msr" / "recomp-kit"; }
fs::path LibraryPath() { return S.user / "msr" / CodeFolder / Library; }

void Log(const std::string& text) {
    std::ofstream f(S.user / "msr" / "recomp-build.log", std::ios::binary | std::ios::app);
    f << text;
}

void Status(const std::string& note, float progress = -1.f) {
    std::scoped_lock lock{S.lock};
    S.code_note = note;
    S.progress = progress;
    S.dirty = true;
}

// The game file's problems (none: "")
std::string CheckGame(const std::string& path) {
    if (path.empty()) return {};
    std::error_code e;
    const fs::path p = FromUtf8(path);
    if (!fs::is_regular_file(p, e)) return "There is no file there.";
    if (fs::file_size(p, e) < (16u << 20)) return "This file is too small to be the game.";
    DumpFile* d = dump_open(path.c_str());   // a compressed one (.zcci) must be whole: its frames' table is at the end
    if (!d) return "This file cannot be read (a compressed game file that is damaged or incomplete?).";
    dump_close(d);
    return {};
}

// A compressed game file (.zcci, .zcxi: Azahar's Z3DS) for the recompiler, which reads the image itself and has no
// zstd: the start of the image, decompressed, up to the end of its ExeFS (the headers, the exheader and the game's
// code: a few MB), written to `out`. "" when done.
std::string WriteImageStart(DumpFile* d, const fs::path& out) {
    constexpr std::uint64_t Media = 0x200;
    unsigned char h[0x200];
    std::uint64_t ncch = 0;
    const auto le32 = [](const unsigned char* p) { return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24; };
    if (dump_read(d, 0, h, sizeof h)) return "The compressed game file cannot be read.";
    if (!std::memcmp(h + 0x100, "NCSD", 4)) {
        ncch = le32(h + 0x120) * Media;
        if (dump_read(d, ncch, h, sizeof h)) return "The compressed game file cannot be read.";
    }
    if (std::memcmp(h + 0x100, "NCCH", 4)) return "The compressed file is not a 3DS game.";
    const std::uint64_t end = ncch + std::max<std::uint64_t>(0x200 + 0x800, std::uint64_t{le32(h + 0x1A0)} * Media + std::uint64_t{le32(h + 0x1A4)} * Media);
    if (end > dump_size(d) || end > (256u << 20)) return "The compressed game file is damaged.";
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(1 << 20);
    for (std::uint64_t at = 0; at < end && f; at += buf.size()) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), end - at));
        if (dump_read(d, at, buf.data(), n)) return "The compressed game file cannot be read.";
        f.write(buf.data(), static_cast<std::streamsize>(n));
    }
    return f ? std::string{} : "Could not write " + Utf8(out) + ".";
}

// The native code's state, from module.json beside it
void CheckCode() {
    std::error_code e;
    Code code = Code::Missing;
    std::string note;
    if (fs::is_regular_file(LibraryPath(), e)) {
        const std::string json = ReadText(S.user / "msr" / CodeFolder / "module.json");
        const std::string runtime = JsonValue(json, "runtime_sha256"), built = JsonValue(json, "built");
        code = runtime == MSR_AOT_RUNTIME_SHA256 ? Code::Ready : Code::Outdated;
        if (!built.empty()) note = "Built " + built + (code == Code::Ready ? "." : " for another version of msr-native.");
    }
#ifndef __ANDROID__
    if (code == Code::Ready) WriteDirect();   // the game can start: the program that starts it without the launcher
#endif
    std::scoped_lock lock{S.lock};
    if (S.building) return;
    if (S.code != Code::Failed || code == Code::Ready) {
        S.code = code;
        S.code_note = note;
    }
    S.dirty = true;
}

void SetGame(const std::string& path) {
    const std::string p = Trim(path);
    WriteText(S.user / "msr" / "game-path.txt", p + "\n");
    std::scoped_lock lock{S.lock};
    S.game = p;
    S.game_note = CheckGame(p);
    S.dirty = true;
}

// ---- the build (a worker thread) ----
bool RunTool(const std::vector<std::string>& argv, const std::function<void(const std::string&)>& line = {},
             const fs::path& watch = {}, std::uint64_t watch_total = 0, const std::string& what = {}) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
    Log("\n> " + cmd + "\n");
    Child c;
    const bool captured = !watch.empty() ? false : true;
    if (!Spawn(argv, KitFolder(), captured, captured ? fs::path{} : S.user / "msr" / "recomp-build.log", true, c)) {
        Log("could not start\n");
        return false;
    }
    {
        std::scoped_lock lock{S.lock};
        S.tool = c;
    }
    int code;
    if (captured) {
        ReadLines(c, [&](const std::string& text) {
            if (text.rfind("@progress", 0) != 0) Log(text + "\n");
            if (line) line(text);
        });
        code = Poll(c, true);
    } else {   // a download: its size as the progress
        while ((code = Poll(c, false)) == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::error_code e;
            const std::uint64_t size = fs::exists(watch, e) ? fs::file_size(watch, e) : 0;
            if (watch_total) Status("Downloading " + what + ": " + std::to_string(size >> 20) + " of " +
                                    std::to_string(watch_total >> 20) + " MB", static_cast<float>(size) / static_cast<float>(watch_total));
            else Status("Downloading " + what + ": " + std::to_string(size >> 20) + " MB");
        }
    }
    {
        std::scoped_lock lock{S.lock};
        S.tool = Child{};
    }
    Close(c);
    Log("(exit " + std::to_string(code) + ")\n");
    return code == 0;
}

bool Cancelled() {
    std::scoped_lock lock{S.lock};
    return S.cancel;
}

std::string SystemPython() {
#ifdef _WIN32
    return {};
#else
    for (const char* name : {"python3", "python"}) {
        Child c;
        if (!Spawn({name, "-I", "-c", "import sys, lzma, zipfile, tarfile, concurrent.futures; sys.exit(0 if sys.version_info >= (3, 8) else 1)"},
                   {}, false, {}, true, c)) continue;
        const int code = Poll(c, true);
        Close(c);
        if (code == 0) return name;
    }
    return {};
#endif
}

bool KitReady(std::string& python) {
    const std::string kit = ReadText(KitFolder() / "kit.json");
    python = JsonValue(kit, "python");
    std::error_code e;
#ifdef _WIN32
    const fs::path zig = KitFolder() / "zig" / "zig.exe";
    if (!python.empty() && !fs::is_regular_file(FromUtf8(python), e)) return false;
#else
    const fs::path zig = KitFolder() / "zig" / "zig";
#endif
    return JsonValue(kit, "kit") == std::to_string(KitVersion) && !python.empty() && fs::is_regular_file(zig, e) &&
           fs::is_directory(KitFolder() / "site" / "capstone", e);
}

std::string Fail(const std::string& why) {
    Log("\n=== failed: " + why + "\n");
    return why;
}

// downloads the kit if needed, then runs build_module.py; "" when it succeeded, else what failed. `android`: the
// Android app's native code (--target android-arm64, into user/msr/recomp-android/), the PC's own left as it is.
std::string Build(const std::string& game, bool android) {
    const fs::path scripts = Scripts();
    if (scripts.empty()) return Fail("The recompiler is missing from this installation (the recomp folder).");
    std::error_code e;
    fs::create_directories(KitFolder(), e);
    Log("\n=== msr-native " MSR_NATIVE_VERSION ": the native code from " + game + "\n");
    std::string python;
    if (!KitReady(python)) {
        const fs::path downloads = KitFolder() / "downloads";
        fs::create_directories(downloads, e);
        python = SystemPython();
        for (const Download& d : Downloads) {
            const bool is_python = std::strncmp(d.file, "python", 6) == 0;
            if (is_python && !python.empty()) continue;   // the system's will do (Linux)
            const fs::path file = downloads / d.file;
            if (!(fs::exists(file, e) && Sha256(file) == d.sha256)) {
                const std::string curl = SystemTool("curl");
                if (curl.empty()) return Fail("Downloading needs curl, which this system does not have.");
                fs::remove(file, e);
                Status(std::string("Downloading ") + d.what, d.size ? 0.f : -1.f);
                if (!RunTool({curl, "-sSfL", "--retry", "2", "--connect-timeout", "30", "-o", Utf8(file), d.url}, {},
                             file, d.size, d.what)) {
                    if (Cancelled()) return "Cancelled.";
                    return Fail(std::string("The ") + d.what + " download failed. Check the internet connection.");
                }
                const std::string actual = Sha256(file);
                if (actual != d.sha256) {
                    fs::remove(file, e);
                    return Fail(std::string("The ") + d.what + " download is not the expected file (SHA-256 " + actual.substr(0, 16) + "...).");
                }
            }
            if (Cancelled()) return "Cancelled.";
            if (is_python) {
                const fs::path target = KitFolder() / "python";
                fs::remove_all(target, e);
                fs::create_directories(target, e);
                const std::string tar = SystemTool("tar");
                if (tar.empty()) return Fail("Unpacking needs tar, which this system does not have.");
                Status("Unpacking Python");
                std::vector<std::string> args{tar, "-xf", Utf8(file), "-C", Utf8(target)};
#ifndef _WIN32
                args.push_back("--strip-components=1");
#endif
                if (!RunTool(args)) return Fail("Python could not be unpacked.");
#ifdef _WIN32
                python = Utf8(target / "python.exe");
#else
                python = Utf8(target / "bin" / "python3");
#endif
            }
        }
        Status("Unpacking the build tools");
        if (!RunTool({python, "-I", "-B", Utf8(scripts), "--setup-kit", Utf8(KitFolder()), "--zig-archive",
                      Utf8(downloads / ZigArchive), "--wheel", Utf8(downloads / "capstone.whl")}))
            return Fail("The build tools could not be unpacked.");
        WriteText(KitFolder() / "kit.json", "{\n \"kit\": " + std::to_string(KitVersion) + ",\n \"python\": \"" + JsonEscape(python) + "\"\n}\n");
        fs::remove_all(downloads, e);
    }
    if (Cancelled()) return "Cancelled.";
    Status("Starting the recompiler");
    std::string dump = game;   // a compressed game file: the start of its image, decompressed (WriteImageStart)
    const fs::path image_start = S.user / "msr" / "recomp-image-start.cci";
    fs::remove(image_start, e);
    if (DumpFile* d = dump_open(game.c_str())) {
        std::string error;
        if (dump_compressed(d)) {
            Status("Reading the compressed game file");
            error = WriteImageStart(d, image_start);
            dump = Utf8(image_start);
        }
        dump_close(d);
        if (!error.empty()) return Fail(error);
    }
    struct Remove {
        fs::path p;
        ~Remove() { std::error_code ignored; fs::remove(p, ignored); }
    } remove_after{image_start};
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    if (const char* j = std::getenv("MSR_BUILD_JOBS"); j && std::atoi(j) > 0) jobs = static_cast<unsigned>(std::atoi(j));
    std::vector<std::string> args{python, "-I", "-B", Utf8(scripts), "--dump", dump, "--user", Utf8(S.user), "--kit",
                                  Utf8(KitFolder()), "--jobs", std::to_string(jobs), "--port-version", MSR_NATIVE_VERSION};
    if (android) {
        args.push_back("--target");
        args.push_back("android-arm64");
    }
    for (const auto& entry : fs::directory_iterator(S.user / "msr", e)) {   // code an earlier run had to interpret
        const std::string name = Utf8(entry.path().filename());
        if (name.rfind("recomp-jit-entries-core", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0) {
            args.push_back("--extra");
            args.push_back(Utf8(entry.path()));
        }
    }
    std::string error;
    const bool ok = RunTool(args, [&](const std::string& line) {
        int a, b;
        char text[512];
        if (std::sscanf(line.c_str(), "@step %d %d %511[^\n]", &a, &b, text) == 3) {
            Status("Step " + std::to_string(a) + " of " + std::to_string(b) + ": " + text);
        } else if (std::sscanf(line.c_str(), "@progress %d %d", &a, &b) == 2 && b > 0) {
            Status("Step 4 of 5: Compiling, " + std::to_string(a) + " of " + std::to_string(b) + " files",
                   static_cast<float>(a) / static_cast<float>(b));
        } else if (line.rfind("@error ", 0) == 0) {
            error = line.substr(7);
        }
    });
    if (Cancelled()) return "Cancelled.";
    if (!ok) return Fail(error.empty() ? "The recompiler stopped with an error: see user/msr/recomp-build.log." : error);
    Log("\n=== done\n");
    return {};
}

void StartBuild(bool android) {
#ifdef __ANDROID__
    return;   // built on a PC (Build for Android there)
#endif
    std::string game;
    {
        std::scoped_lock lock{S.lock};
        if (S.building || S.game.empty() || !S.game_note.empty() || S.update == Snapshot::Update::Working) return;
        S.building = true;
        S.cancel = false;
        S.android_build = android;
        S.code_before = S.code;
        S.code = Code::Building;
        S.code_note = android ? "Starting the Android build" : "Starting";
        S.progress = -1.f;
        S.dirty = true;
        game = S.game;
    }
    if (S.worker.joinable()) S.worker.join();
    S.worker = std::thread([game, android] {
        const auto started = std::chrono::steady_clock::now();
        const std::string error = Build(game, android);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        const std::string took = std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s";
        {
            std::scoped_lock lock{S.lock};
            S.building = false;
            S.progress = -1.f;
            if (android) {   // the PC's native code as it was; the Android build's result below the button
                S.code = S.code_before;
                S.code_note.clear();
                S.android_note = error.empty()
                    ? "Built in " + took + ": copy the folder user/msr/recomp-android into the Android app's user/msr " +
                          "folder (over USB: Android/data/io.github.gminustwo.msrnative/files/user/msr), with your game " +
                          "file in its user folder."
                    : "The Android build failed: " + error;
            } else {
                S.code = error.empty() ? Code::Ready : Code::Failed;
                S.code_note = error.empty() ? "Built in " + took + "." : error;
            }
            S.dirty = true;
        }
        if (error.empty() || android) CheckCode();
    });
}

void CancelBuild() {
    std::scoped_lock lock{S.lock};
    if (!S.building) return;
    S.cancel = true;
    Kill(S.tool);
}

// ---- the file dialog ----
bool CanBrowse() {
#ifdef _WIN32
    return true;
#elif defined(__ANDROID__)
    return false;   // the game file is found in the user folder (FindGame)
#else
    return std::system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") == 0;
#endif
}

std::string Browse(const std::string& current) {
#ifdef _WIN32
    wchar_t file[4096] = L"";
    const std::wstring start = Wide(current);
    if (!start.empty() && start.size() < 4095) std::wcscpy(file, start.c_str());
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (video_window() && SDL_GetWindowWMInfo(video_window(), &info)) of.hwndOwner = info.info.win.window;
    of.lpstrFilter = L"3DS game dumps (*.3ds, *.cci, *.cxi, *.zcci, *.zcxi)\0*.3ds;*.cci;*.cxi;*.zcci;*.zcxi\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 4096;
    of.lpstrTitle = L"Metroid: Samus Returns (USA), decrypted";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of) ? Narrow(file) : std::string{};
#else
    (void)current;
    const char* cmd = std::system("command -v zenity >/dev/null 2>&1") == 0
        ? "env -u LD_LIBRARY_PATH zenity --file-selection --title='Metroid: Samus Returns (USA), decrypted' "
          "--file-filter='3DS game dumps | *.3ds *.cci *.cxi *.zcci *.zcxi' --file-filter='All files | *' 2>/dev/null"
        : "env -u LD_LIBRARY_PATH kdialog --getopenfilename . '*.3ds *.cci *.cxi *.zcci *.zcxi' 2>/dev/null";
    FILE* p = popen(cmd, "r");
    if (!p) return {};
    char buf[4096];
    std::string out;
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    return Trim(out);
#endif
}

void OpenFolder(const fs::path& dir) {
    std::error_code e;
    fs::create_directories(dir, e);
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__ANDROID__)
    (void)dir;   // the overlay shows the folder instead
#else
    Child c;
    if (Spawn({"xdg-open", Utf8(dir)}, {}, false, {}, true, c)) std::thread([c]() mutable { Poll(c, true); Close(c); }).detach();
#endif
}

// ---- updates: a newer release, downloaded and put in place of this installation's own files ----
// The releases of MSR_UPDATE_REPO on GitHub (native/CMakeLists.txt; public releases), each with the packages
// tools/package makes: msr-native-<version>-windows-x64.zip (the program, its libraries and recomp/, in one folder) and
// msr-native-<version>-x86_64.AppImage. The launcher looks when it starts and offers the newest release above its own
// version that has this system's package (pre-releases too). Update downloads it, checks its SHA-256 against the
// release's digest, and then
//   - Windows: unpacks it (tar) into user/msr/update/; each file it has goes where the installation has it: the old one
//     renamed to <name>.updating-old (a running program and its libraries can be renamed, not overwritten), the new one
//     copied in; recomp/'s files the package no longer has are retired the same way; any failure puts the old files
//     back. The new program starts and deletes the old files.
//   - Linux: writes the new AppImage beside the running one and renames it over it (the same path: shortcuts, Steam's
//     too, keep working), then runs it in this process's place.
// The user folder is never touched: saves, settings, mods, the game file and the native code built from it, which
// stays valid unless the new version changes aot_runtime.h (CheckCode then asks for a rebuild). MSR_UPDATE_URL: another
// address for the release list (any curl URL, file:// for tests; "off": no check). The log: user/msr/update.log.
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* Get(const char* key) const {
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
    std::string Str(const char* key) const { const Json* v = Get(key); return v && v->type == String ? v->s : std::string{}; }
    bool Flag(const char* key) const { const Json* v = Get(key); return v && v->type == Bool && v->b; }
    double Num(const char* key) const { const Json* v = Get(key); return v && v->type == Number ? v->n : 0.; }
};
struct JsonReader {
    const std::string& t;
    std::size_t i = 0;
    bool ok = true;
    void Space() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }
    bool Word(const char* w) {
        const std::size_t n = std::strlen(w);
        if (t.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    std::string Text() {   // at the opening quote
        std::string out;
        for (i++; i < t.size() && t[i] != '"'; ) {
            const char c = t[i++];
            if (c != '\\' || i >= t.size()) { out += c; continue; }
            const char e = t[i++];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else if (e == 'b') out += '\b';
            else if (e == 'f') out += '\f';
            else if (e == 'u') {   // UTF-8 (a surrogate pair's halves each as they are: rare in release lists)
                unsigned cp = 0;
                for (int k = 0; k < 4 && i < t.size(); k++) {
                    const char h = t[i++];
                    cp = cp * 16 + static_cast<unsigned>(h >= '0' && h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) { out += static_cast<char>(0xC0 | cp >> 6); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                else { out += static_cast<char>(0xE0 | cp >> 12); out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            } else out += e;   // \" \\ \/
        }
        if (i >= t.size()) ok = false;
        else i++;
        return out;
    }
    Json Value(int depth = 0) {
        Json v;
        Space();
        if (i >= t.size() || depth > 64) { ok = false; return v; }
        const char c = t[i];
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            v.type = object ? Json::Object : Json::Array;
            i++;
            Space();
            if (i < t.size() && t[i] == (object ? '}' : ']')) { i++; return v; }
            while (ok) {
                Space();
                if (object) {
                    if (i >= t.size() || t[i] != '"') { ok = false; break; }
                    std::string key = Text();
                    Space();
                    if (i >= t.size() || t[i] != ':') { ok = false; break; }
                    i++;
                    v.members.emplace_back(std::move(key), Value(depth + 1));
                } else {
                    v.items.push_back(Value(depth + 1));
                }
                Space();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == (object ? '}' : ']')) { i++; break; }
                ok = false;
            }
        } else if (c == '"') {
            v.type = Json::String;
            v.s = Text();
        } else if (Word("true")) {
            v.type = Json::Bool;
            v.b = true;
        } else if (Word("false")) {
            v.type = Json::Bool;
        } else if (!Word("null")) {
            char* end = nullptr;
            v.type = Json::Number;
            v.n = std::strtod(t.c_str() + i, &end);
            const std::size_t used = static_cast<std::size_t>(end - (t.c_str() + i));
            if (!used) ok = false;
            i += used;
        }
        return v;
    }
};

// "v1.2.3", "1.2.3-test" -> {1, 2, 3}; empty when it is no version
std::vector<int> VersionOf(std::string s) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    std::vector<int> parts;
    for (std::size_t i = 0; i < s.size() && s[i] >= '0' && s[i] <= '9';) {
        int n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + (s[i++] - '0');
        parts.push_back(n);
        if (i < s.size() && s[i] == '.') i++;
        else break;
    }
    return parts;
}
bool Newer(const std::vector<int>& a, const std::vector<int>& b) {
    for (std::size_t k = 0; k < std::max(a.size(), b.size()); k++) {
        const int x = k < a.size() ? a[k] : 0, y = k < b.size() ? b[k] : 0;
        if (x != y) return x > y;
    }
    return false;
}

fs::path UpdateFolder() { return S.user / "msr" / "update"; }
void UpdateLog(const std::string& text) {
    std::error_code e;
    fs::create_directories(UpdateFolder().parent_path(), e);
    std::ofstream f(S.user / "msr" / "update.log", std::ios::binary | std::ios::app);
    f << text << "\n";
    std::fprintf(stderr, "launcher: update: %s\n", text.c_str());
}
void UpdateStatus(Snapshot::Update state, const std::string& note, float progress = -1.f) {
    std::scoped_lock lock{S.lock};
    S.update = state;
    S.update_note = note;
    S.update_progress = progress;
    S.dirty = true;
}
std::string Megabytes(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    return buf;
}

// a tool for the update (curl, tar), waited for; with `file` and `total`, the file's growth as the progress
bool UpdateTool(const std::vector<std::string>& argv, const fs::path& file = {}, std::uint64_t total = 0) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + a;
    UpdateLog("> " + cmd);
    Child c;
    if (!Spawn(argv, {}, false, S.user / "msr" / "update.log", true, c)) { UpdateLog("could not start"); return false; }
    {
        std::scoped_lock lock{S.lock};
        S.update_tool = c;
    }
    int code;
    while ((code = Poll(c, false)) == -1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (file.empty() || !total) continue;
        std::error_code e;
        const std::uint64_t size = fs::exists(file, e) ? fs::file_size(file, e) : 0;
        UpdateStatus(Snapshot::Update::Working, "Downloading: " + Megabytes(size) + " of " + Megabytes(total),
                     static_cast<float>(std::min(size, total)) / static_cast<float>(total));
    }
    {
        std::scoped_lock lock{S.lock};
        S.update_tool = Child{};
    }
    Close(c);
    if (code) UpdateLog("(exit " + std::to_string(code) + ")");
    return code == 0;
}

// The package this system takes, and whether this installation can be updated in place (a release's: its recomp/
// beside the program, or an AppImage, in a folder it can write to; not a development build)
const char* PackageSuffix() {
#ifdef _WIN32
    return "-windows-x64.zip";
#else
    return "-x86_64.AppImage";
#endif
}
bool Installable() {
    std::error_code e;
#ifdef _WIN32
    const fs::path dir = ExePath().parent_path();
    return fs::is_regular_file(dir / "recomp" / "tools" / "msr" / "recomp" / "build_module.py", e) && Writable(dir);
#else
    const char* image = std::getenv("APPIMAGE");
    return image && *image && fs::is_regular_file(FromUtf8(image), e) && Writable(FromUtf8(image).parent_path());
#endif
}

// the release list (a worker thread, when the launcher starts): the newest release above this version, if any
void CheckForUpdate() {
    std::string url = "https://api.github.com/repos/" MSR_UPDATE_REPO "/releases?per_page=20";
    if (const char* u = std::getenv("MSR_UPDATE_URL"); u && *u) url = u;
    if (url == "off") return;
    const std::string curl = SystemTool("curl");
    if (curl.empty()) return;
    std::error_code e;
    fs::create_directories(UpdateFolder(), e);
    const fs::path list = UpdateFolder() / "releases.json";
    fs::remove(list, e);
    if (!UpdateTool({curl, "-sSfL", "--max-time", "20", "--connect-timeout", "10", "-H", "Accept: application/vnd.github+json",
                     "-o", Utf8(list), url})) {
        UpdateLog("no release list from " + url);
        return;
    }
    const std::string text = ReadText(list);
    JsonReader reader{text};
    const Json releases = reader.Value();
    if (!reader.ok || releases.type != Json::Array) { UpdateLog("the release list could not be read"); return; }
    const std::vector<int> current = VersionOf(MSR_NATIVE_VERSION);
    std::vector<int> best_version = current;
    const Json *best = nullptr, *best_asset = nullptr;
    for (const Json& r : releases.items) {
        if (r.type != Json::Object || r.Flag("draft")) continue;
        const std::vector<int> v = VersionOf(r.Str("tag_name"));
        if (v.empty() || !Newer(v, best_version)) continue;
        const Json* assets = r.Get("assets");
        if (!assets) continue;
        for (const Json& a : assets->items) {
            const std::string name = a.Str("name"), suffix = PackageSuffix();
            if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                !a.Str("browser_download_url").empty()) {
                best = &r;
                best_asset = &a;
                best_version = v;
                break;
            }
        }
    }
    if (!best) { UpdateLog("msr-native " MSR_NATIVE_VERSION " is the newest"); return; }
    std::string version = best->Str("tag_name");
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.erase(0, 1);
    std::string sha256 = best_asset->Str("digest");
    if (sha256.rfind("sha256:", 0) == 0) sha256.erase(0, 7);
    else sha256.clear();
    const bool installable = Installable();
    UpdateLog("offered: msr-native " + version + " (" + best_asset->Str("name") + ")" + (installable ? "" : ", not here: the page"));
    std::scoped_lock lock{S.lock};
    S.update = Snapshot::Update::Available;
    S.update_version = version;
    S.update_page = best->Str("html_url");
    S.update_url = best_asset->Str("browser_download_url");
    S.update_sha256 = sha256;
    S.update_size = static_cast<std::uint64_t>(best_asset->Num("size"));
    S.update_installable = installable;
    S.update_note = installable ? "A " + Megabytes(S.update_size) + " download. Your saves, settings, mods and the native code stay."
                                : std::string("Download it from its page.");
    S.dirty = true;
}

// the leftovers of an update: the old files (once the old program has ended) and the unpacked package
void CleanUpdate() {
    std::error_code e;
    fs::remove_all(UpdateFolder() / "unpacked", e);
    for (const auto& entry : fs::directory_iterator(UpdateFolder(), e))
        if (entry.path().extension() == ".zip") fs::remove(entry.path(), e);
#ifdef _WIN32
    const fs::path install = ExePath().parent_path();
    for (int attempt = 0; attempt < 40; attempt++) {   // 20 s: the old program may still be ending
        std::vector<fs::path> old;
        for (const auto& entry : fs::directory_iterator(install, e))
            if (Utf8(entry.path().filename()).find(".updating-old") != std::string::npos) old.push_back(entry.path());
        if (fs::is_directory(install / "recomp", e))
            for (const auto& entry : fs::recursive_directory_iterator(install / "recomp", e))
                if (Utf8(entry.path().filename()).find(".updating-old") != std::string::npos) old.push_back(entry.path());
        bool left = false;
        for (const auto& p : old) {
            fs::remove(p, e);
            left |= static_cast<bool>(e);
        }
        if (!left) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
}

#ifdef _WIN32
// <path>.updating-old, or <path>.updating-old2... when an earlier one cannot be removed yet
fs::path OldName(const fs::path& p) {
    std::error_code e;
    for (int n = 1;; n++) {
        fs::path old = p;
        old += n == 1 ? std::string(".updating-old") : ".updating-old" + std::to_string(n);
        fs::remove(old, e);
        if (!fs::exists(old, e)) return old;
    }
}

// the unpacked package's files in place of this installation's (see above); "" when done
std::string InstallWindows(const fs::path& zip) {
    std::error_code e;
    const fs::path install = ExePath().parent_path(), unpacked = UpdateFolder() / "unpacked";
    fs::remove_all(unpacked, e);
    fs::create_directories(unpacked, e);
    const std::string tar = SystemTool("tar");
    if (tar.empty()) return "Unpacking needs tar, which this system does not have.";
    UpdateStatus(Snapshot::Update::Working, "Unpacking");
    if (!UpdateTool({tar, "-xf", Utf8(zip), "-C", Utf8(unpacked)})) return "The download could not be unpacked.";
    fs::path root;   // the package's folder: the one with msr-native.exe
    if (fs::is_regular_file(unpacked / "msr-native.exe", e)) root = unpacked;
    else
        for (const auto& entry : fs::directory_iterator(unpacked, e))
            if (fs::is_regular_file(entry.path() / "msr-native.exe", e)) root = entry.path();
    if (root.empty() || !fs::is_regular_file(root / "recomp" / "tools" / "msr" / "recomp" / "build_module.py", e))
        return "The download is not an msr-native package.";
    std::vector<fs::path> files;   // relative to the package
    for (const auto& entry : fs::recursive_directory_iterator(root, e))
        if (entry.is_regular_file(e)) files.push_back(fs::relative(entry.path(), root, e));
    UpdateStatus(Snapshot::Update::Working, "Putting the new files in place");
    struct Placed { fs::path dest, old; };
    std::vector<Placed> placed;
    const auto undo = [&] {
        std::error_code u;
        for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
            fs::remove(it->dest, u);
            if (!it->old.empty()) fs::rename(it->old, it->dest, u);
        }
    };
    for (const auto& rel : files) {
        const fs::path dest = install / rel;
        fs::path old;
        if (fs::exists(dest, e)) {
            old = OldName(dest);
            fs::rename(dest, old, e);
            if (e) { undo(); return "Could not replace " + Utf8(rel) + " (" + e.message() + ")."; }
        }
        fs::create_directories(dest.parent_path(), e);
        fs::copy_file(root / rel, dest, e);
        placed.push_back({dest, old});
        if (e) { undo(); return "Could not write " + Utf8(rel) + " (" + e.message() + ")."; }
    }
    // the recompiler's files the new package no longer has: retired with the old ones (deleted by the new program)
    std::set<fs::path> fresh;
    for (const auto& rel : files) fresh.insert(install / rel);
    std::vector<fs::path> stale;
    for (const auto& entry : fs::recursive_directory_iterator(install / "recomp", e))
        if (entry.is_regular_file(e) && !fresh.count(entry.path()) &&
            Utf8(entry.path().filename()).find(".updating-old") == std::string::npos)
            stale.push_back(entry.path());
    for (const auto& p : stale) fs::rename(p, OldName(p), e);
    UpdateLog("installed " + std::to_string(files.size()) + " files into " + Utf8(install));
    S.restart_into = install / "msr-native.exe";
    return {};
}
#else
// the new AppImage over the running one (see above); "" when done
std::string InstallLinux(const fs::path& download) {
    std::error_code e;
    const fs::path image = FromUtf8(std::getenv("APPIMAGE"));
    if (chmod(download.c_str(), 0755)) return "The new AppImage could not be made executable.";
    fs::rename(download, image, e);
    if (e) return "Could not replace the AppImage (" + e.message() + ").";
    UpdateLog("installed " + Utf8(image));
    S.restart_into = image;
    return {};
}
#endif

// Update (a worker thread): download, check, put in place; the launcher then closes and the new version starts
void StartUpdate() {
    std::string url, sha256;
    std::uint64_t size;
    {
        std::scoped_lock lock{S.lock};
        if (!S.update_installable || S.update == Snapshot::Update::Working || S.update == Snapshot::Update::None ||
            S.building || S.playing)
            return;
        S.update = Snapshot::Update::Working;
        S.update_note = "Starting the download";
        S.update_progress = 0.f;
        S.dirty = true;
        url = S.update_url;
        sha256 = S.update_sha256;
        size = S.update_size;
    }
    if (S.update_worker.joinable()) S.update_worker.join();
    S.update_worker = std::thread([url, sha256, size] {
        const std::string error = [&]() -> std::string {
            const std::string curl = SystemTool("curl");
            if (curl.empty()) return "Downloading needs curl, which this system does not have.";
            std::error_code e;
            fs::create_directories(UpdateFolder(), e);
#ifdef _WIN32
            const fs::path file = UpdateFolder() / "msr-native-update.zip";
#else
            fs::path file = FromUtf8(std::getenv("APPIMAGE"));
            file += ".download";
#endif
            fs::remove(file, e);
            if (!UpdateTool({curl, "-sSfL", "--retry", "2", "--connect-timeout", "30", "-o", Utf8(file), url}, file, size)) {
                fs::remove(file, e);
                return "The download failed. Check the internet connection.";
            }
            if (!sha256.empty()) {
                UpdateStatus(Snapshot::Update::Working, "Checking the download");
                const std::string actual = Sha256(file);
                if (actual != sha256) {
                    fs::remove(file, e);
                    UpdateLog("SHA-256 " + actual + ", expected " + sha256);
                    return "The download is not the release's file (its SHA-256 differs).";
                }
            }
#ifdef _WIN32
            return InstallWindows(file);
#else
            return InstallLinux(file);
#endif
        }();
        if (!error.empty()) {
            UpdateLog("failed: " + error);
            UpdateStatus(Snapshot::Update::Failed, error);
            return;
        }
        UpdateStatus(Snapshot::Update::Working, "Starting the new version");
        std::scoped_lock lock{S.lock};
        S.quit = true;   // msr_launcher_main closes the window and starts it
    });
}

void OpenPage(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    Child c;
    if (Spawn({"xdg-open", url}, {}, false, {}, true, c)) std::thread([c]() mutable { Poll(c, true); Close(c); }).detach();
#endif
}

// ---- the direct start: the game without the launcher ----
// Once the native code is built (CheckCode finds it ready), the launcher writes a program beside itself that starts the
// game directly: msr-native-direct.exe on Windows (runtime/win/direct.c, inside this program as resource 200), or
// msr-native-direct.sh beside the AppImage on Linux. Either runs this program with --direct (msr_launcher_direct): Play
// without the launcher's window, Restart included; while the game cannot start (no game file, the native code missing
// or built by another version), the launcher opens instead. A shortcut, Steam or a frontend can start it. Written
// again when this version's differs (after an update; the old one, perhaps running, renamed aside as an update's old
// files are), and left out when the program's folder cannot be written to.
fs::path DirectPath() {
#ifdef _WIN32
    return ExePath().parent_path() / "msr-native-direct.exe";
#else
    const char* image = std::getenv("APPIMAGE");
    return (image && *image ? FromUtf8(image) : ExePath()).parent_path() / "msr-native-direct.sh";
#endif
}

std::string DirectContent() {
#ifdef _WIN32
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(200), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    return bytes ? std::string(static_cast<const char*>(bytes), SizeofResource(nullptr, found)) : std::string{};
#else
    // the AppImage (or the program) by its name, from the script's own folder: the folder can move
    const char* image = std::getenv("APPIMAGE");
    std::string quoted = "'";
    for (char c : Utf8((image && *image ? FromUtf8(image) : ExePath()).filename())) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    return "#!/bin/sh\n"
           "# msr-native without its launcher (the launcher wrote this once the native code was built)\n"
           "exec \"$(dirname \"$(readlink -f \"$0\")\")\"/" + quoted + " --direct\n";
#endif
}

void WriteDirect() {
    static std::mutex writing;   // the build's worker and the window's thread
    std::scoped_lock one{writing};
    const std::string content = DirectContent();
    if (content.empty()) return;
    const fs::path path = DirectPath();
    std::error_code e;
    if (ReadText(path) != content) {   // missing, or another version's
        fs::path temp = path;
        temp += ".new";
        if (!WriteText(temp, content)) {
            fs::remove(temp, e);
            std::fprintf(stderr, "launcher: could not write %s\n", Utf8(path).c_str());
            return;
        }
        fs::path old;
#ifdef _WIN32
        if (fs::exists(path, e)) {   // it may be running: renamed aside (CleanUpdate deletes it)
            old = OldName(path);
            fs::rename(path, old, e);
            if (e) old.clear();
        }
#else
        chmod(temp.c_str(), 0755);
#endif
        fs::rename(temp, path, e);
        if (e) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            if (!old.empty()) fs::rename(old, path, ignored);
            std::fprintf(stderr, "launcher: could not write %s (%s)\n", Utf8(path).c_str(), e.message().c_str());
            return;
        }
        std::fprintf(stderr, "launcher: wrote %s\n", Utf8(path).c_str());
    }
    std::scoped_lock lock{S.lock};
    S.direct = Utf8(path.filename());
    S.dirty = true;
}

// ---- Play: this program again, with the display settings ----
bool StartGame() {
    MsrIni::File file(Utf8(S.settings));
    const bool fullscreen = file.Bool("native", "fullscreen", false), vsync = file.Bool("native", "vsync", true);
    const int resolution = static_cast<int>(file.Number("native", "resolution", 4));
    const int volume = static_cast<int>(file.Number("native", "volume", 100));
    std::vector<std::string> args{Utf8(ExePath()), "--play", "--layout=single", "--scale=3",
                                  "--resolution=" + std::to_string(std::clamp(resolution, 0, 10)),   // 0: match the window
                                  "--filter=fsr", "--volume=" + std::to_string(std::clamp(volume, 0, 100))};
    if (fullscreen) args.push_back("--fullscreen=borderless");
    if (!vsync) args.push_back("--vsync=off");
#ifdef __ANDROID__
    {   // the app again, as the game (main.c logs to user/msr-native.log there); this process ends
        std::fprintf(stderr, "launcher: starting the game\n");
        std::vector<const char*> argv;
        for (std::size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].c_str());
        msr_android_relaunch(static_cast<int>(argv.size()), argv.data());
    }
#endif
    const fs::path log = S.root / "user" / "msr-native.log";
    std::error_code e;
    fs::remove(log, e);   // each run's own log (the game writes it through this handle)
    Child c;
    if (!Spawn(args, S.root, false, log, false, c)) {
        std::fprintf(stderr, "launcher: the game could not start (%s)\n", Utf8(ExePath()).c_str());
        return false;
    }
    std::fprintf(stderr, "launcher: started the game\n");
    std::scoped_lock lock{S.lock};
    S.game_process = c;
    S.playing = true;
    return true;
}

void Publish() {
    Snapshot s;
    {
        std::scoped_lock lock{S.lock};
        if (!S.dirty) return;
        S.dirty = false;
        s.active = true;
        s.version = "msr-native " MSR_NATIVE_VERSION;
        s.game = S.game;
        s.game_note = S.game_note;
        s.code = S.code;
        s.code_note = S.code_note;
        s.progress = S.progress;
        s.can_play = !S.game.empty() && S.game_note.empty() && S.code == Code::Ready && !S.building &&
                     S.update != Snapshot::Update::Working;
        s.folder = Utf8(S.user);
        s.update = S.update;
        s.update_version = S.update_version;
        s.update_note = S.update_note;
        s.update_progress = S.update_progress;
        s.update_installable = S.update_installable;
        s.direct = S.direct;
#ifdef __ANDROID__
        s.can_build = false;   // built on a PC (its Build for Android)
#else
        s.android_build = true;
        s.android_note = S.android_note;
#endif
    }
    static const bool browse = CanBrowse();
    s.can_browse = browse;
    MSR::GetOverlay().SetLauncher(s);
}

} // namespace

// The overlay's launcher actions (overlay_host.cpp)
extern "C" void msr_launcher_action(int kind, const char* text) {
    using Kind = MSR::OverlayAction::Kind;
    std::fprintf(stderr, "launcher: action %d\n", kind);
    switch (static_cast<Kind>(kind)) {
    case Kind::LauncherPlay: {
        bool ready;
        {
            std::scoped_lock lock{S.lock};
            ready = !S.game.empty() && S.game_note.empty() && S.code == Code::Ready && !S.building && !S.playing &&
                    S.update != Snapshot::Update::Working;
        }
        if (ready) {
            msr_settings_save();
            if (StartGame()) video_show(0);
        }
        break;
    }
    case Kind::LauncherBuild: StartBuild(false); break;
    case Kind::LauncherBuildAndroid: StartBuild(true); break;
    case Kind::LauncherCancel: CancelBuild(); break;
    case Kind::LauncherBrowse: {
        std::string current;
        {
            std::scoped_lock lock{S.lock};
            current = S.game;
        }
        const std::string chosen = Browse(current);
        if (!chosen.empty()) SetGame(chosen);
        break;
    }
    case Kind::LauncherGame: SetGame(text ? text : ""); break;
    case Kind::LauncherFolder: OpenFolder(S.user); break;
    case Kind::LauncherUpdate: StartUpdate(); break;
    case Kind::LauncherUpdatePage: {
        std::string page;
        {
            std::scoped_lock lock{S.lock};
            page = S.update_page;
        }
        OpenPage(page);
        break;
    }
    case Kind::Quit: {
        std::scoped_lock lock{S.lock};
        S.quit = true;
        break;
    }
    default: break;
    }
}

namespace {

// The user folder, the working folder (the game's paths are user/...) and the log: the launcher's and the direct start's
void Setup() {
    S.root = DataRoot();
    std::error_code e;
    fs::create_directories(S.root / "user" / "msr", e);
    fs::current_path(S.root, e);   // the game runs here too: its paths are user/...
    S.user = S.root / "user";
    S.settings = S.user / "msr" / "settings.ini";
    {   // the launcher's own log (a program without a console has no stderr)
        const fs::path log = S.user / "launcher.log";
#ifdef _WIN32
        _wfreopen(log.c_str(), L"w", stderr);
#else
        std::freopen(log.c_str(), "w", stderr);
#endif
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    std::fprintf(stderr, "launcher: msr-native %s, user folder %s\n", MSR_NATIVE_VERSION, Utf8(S.user).c_str());
#ifdef _WIN32
    _putenv("PYTHONUNBUFFERED=1"); _putenv("PYTHONUTF8=1"); _putenv("PYTHONDONTWRITEBYTECODE=1");
    _putenv("PYTHONHOME="); _putenv("PYTHONPATH=");
#else
    setenv("PYTHONUNBUFFERED", "1", 1); setenv("PYTHONUTF8", "1", 1); setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    unsetenv("PYTHONHOME"); unsetenv("PYTHONPATH");
#endif
}

#ifdef __ANDROID__
// A game file copied into the user folder (or its game/ folder): the first that can be the game, else the first found
std::string FindGame() {
    std::string first;
    for (const fs::path& dir : {S.user, S.user / "game", S.root}) {
        std::error_code e;
        for (const auto& entry : fs::directory_iterator(dir, e)) {
            std::string ext = Utf8(entry.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (ext != ".3ds" && ext != ".cci" && ext != ".cxi" && ext != ".zcci" && ext != ".zcxi") continue;
            const std::string path = Utf8(entry.path());
            if (CheckGame(path).empty()) return path;
            if (first.empty()) first = path;
        }
    }
    return first;
}
#endif

void ReadGame() {
    std::string game = Trim(ReadText(S.user / "msr" / "game-path.txt"));
#ifdef __ANDROID__
    std::error_code e;
    if (game.empty() || !fs::is_regular_file(FromUtf8(game), e)) {   // copied in over USB: found there
        game = FindGame();
        if (!game.empty()) WriteText(S.user / "msr" / "game-path.txt", game + "\n");
    }
#endif
    std::scoped_lock lock{S.lock};
    S.game = game;
    S.game_note = CheckGame(S.game);
#ifdef __ANDROID__
    if (S.game.empty()) S.game_note = "Copy your game file (a decrypted .3ds, .cci, .cxi or .zcci) into the user folder below.";
#endif
}

// The launcher's window until it is closed. 0 when closed normally.
int Launcher() {
    const std::string settings = Utf8(S.settings);
    msr_settings_load(settings.c_str());
    {
        char* base = SDL_GetBasePath();
        msr_mods_init(Utf8(S.user).c_str(), settings.c_str(), base ? base : "");
        if (base) SDL_free(base);
    }
    MsrIni::File file(settings);
    msr_overlay_launcher(1);
    msr_overlay_start(0, static_cast<int>(file.Number("native", "resolution", 4)), file.Bool("native", "vsync", true),
                      static_cast<int>(file.Number("native", "volume", 100)));
    ReadGame();
    CheckCode();
    video_option("layout", "single");
    if (video_init(3)) {
        std::fprintf(stderr, "launcher: no window (Vulkan 1.3 is needed)\n");
        return 1;
    }
    Publish();
#ifndef __ANDROID__
    S.update_checker = std::thread([] { CleanUpdate(); CheckForUpdate(); });   // a newer release (the updates section)
#endif
    for (;;) {
        bool playing;
        {
            std::scoped_lock lock{S.lock};
            if (S.quit) break;
            playing = S.playing;
        }
        if (playing) {   // the game runs: wait for it, the window hidden
            SDL_PumpEvents();
            const int code = Poll(S.game_process, false);
            if (code == -1) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            std::fprintf(stderr, "launcher: the game ended (%d)\n", code);
            {
                std::scoped_lock lock{S.lock};
                Close(S.game_process);
                S.playing = false;
            }
            if (code == MSR_EXIT_RESTART && StartGame()) continue;   // its menu's Restart
            msr_overlay_reload();   // what its menu changed
            CheckCode();
            video_show(1);
        }
        Publish();
        if (!video_frame()) break;   // the window was closed
        std::this_thread::sleep_for(std::chrono::milliseconds(4));   // the presentation paces it; without V-Sync too
    }
    CancelBuild();
    if (S.worker.joinable()) S.worker.join();
    {   // a check or download still running: stopped (an update being put in place finishes first)
        std::scoped_lock lock{S.lock};
        Kill(S.update_tool);
    }
    if (S.update_checker.joinable()) S.update_checker.join();
    if (S.update_worker.joinable()) S.update_worker.join();
    msr_settings_save();
    video_shutdown();
    if (!S.restart_into.empty()) {   // an update: its program, in this one's place
        std::fprintf(stderr, "launcher: starting %s\n", Utf8(S.restart_into).c_str());
#ifdef _WIN32
        Child c;
        if (Spawn({Utf8(S.restart_into)}, S.restart_into.parent_path(), false, {}, false, c)) Close(c);
#else
        unsetenv("LD_LIBRARY_PATH");   // this AppImage's libraries; the new one sets its own
        const std::string path = Utf8(S.restart_into);
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "launcher: could not start the new version\n");
#endif
    }
    return 0;
}

} // namespace

// main.c, when the program is started on its own: the launcher until it is closed. 0 when closed normally.
extern "C" int msr_launcher_main(void) {
    Setup();
    return Launcher();
}

// main.c, --direct (msr-native-direct.exe, the direct start section): Play at once, without the launcher's window, and
// again when the game's Restart asks; the game's exit code. The launcher when the game cannot start yet.
extern "C" int msr_launcher_direct(void) {
    Setup();
    std::fprintf(stderr, "launcher: the direct start (--direct)\n");
    ReadGame();
    CheckCode();
    bool ready;
    {
        std::scoped_lock lock{S.lock};
        ready = !S.game.empty() && S.game_note.empty() && S.code == Code::Ready;
    }
    if (!ready) {
        std::fprintf(stderr, "launcher: the game cannot start yet (the game file or the native code): the launcher\n");
        return Launcher();
    }
    for (;;) {
        if (!StartGame()) return 1;
        const int code = Poll(S.game_process, true);
        std::fprintf(stderr, "launcher: the game ended (%d)\n", code);
        {
            std::scoped_lock lock{S.lock};
            Close(S.game_process);
            S.playing = false;
        }
        if (code != MSR_EXIT_RESTART) return code;
    }
}
