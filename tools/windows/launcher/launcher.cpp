/* "Tears of the Kingdom.exe": launcher + setup for a package folder (see build_windows.py).
   - The window always opens (Dear ImGui over Direct3D 11). First run: one setup screen (NSP/XCI + update + prod.keys +
     data folder, one "Set up" button: import, verify, prepare shaders; resumable; done = game files + shaders prepared).
     Then: Play (big PLAY button), Game files (import / adopt, run setup again, delete), Settings (settings.ini + .bat),
     Saves (import / export / open folder), Controls.
   - PLAY starts bin\totk-win.exe with stderr in logs\totk.log, waits, reports problems.
   - Long jobs (import, shaders, save import/export) run support\totk_setup.py with the bundled Python (support\python).
   Options: --play (start the game without the window: scripts, benchmarks)  --setup (Settings page)  --tab <0-4>. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <functional>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef std::string S;
typedef std::wstring WS;
static const wchar_t *TITLE = L"Tears of the Kingdom";

/* ---------- strings / paths ---------- */
static S U(const WS &w) { if (w.empty()) return S(); int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), 0, 0, 0, 0); S s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, 0, 0); return s; }
static WS W(const S &s) { if (s.empty()) return WS(); int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0); WS w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w; }
static S fmt(const char *f, ...) { char b[2048]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap); return b; }
static bool exists(const WS &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool isdir(const WS &p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void fail(const S &msg) { MessageBoxW(NULL, W(msg).c_str(), TITLE, MB_OK | MB_ICONERROR | MB_SETFOREGROUND); }
static bool read_text(const WS &path, S &out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(h, NULL), got = 0; out.assign(n, 0);
    bool ok = n == 0 || ReadFile(h, &out[0], n, &got, NULL); CloseHandle(h); out.resize(got);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB) out.erase(0, 3);
    return ok;
}
static bool write_text(const WS &path, const S &text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w; bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, NULL); CloseHandle(h); return ok;
}
static void open_path(const WS &p) { ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL, SW_SHOWNORMAL); }
static S parent_of(const S &p) { size_t i = p.find_last_of("\\/"); return i == S::npos ? S() : p.substr(0, i); }

/* ---------- package state ---------- */
static WS here;                       /* package folder with trailing backslash */
static S dispver, vdir;               /* "1.4.3" "v143" from bin\game-version.txt */
static std::map<S, S> kv;             /* settings.bat: every  set "NAME=VALUE"  line (values as written, %~dp0 unexpanded) */

static S expand(const S &in) {        /* %~dp0 -> package folder */
    S out, h = U(here);
    for (size_t i = 0; i < in.size();) { if (!in.compare(i, 5, "%~dp0")) { out += h; i += 5; } else out += in[i++]; }
    return out;
}
static S get(const char *k, const char *def = "") { auto it = kv.find(k); return it == kv.end() ? S(def) : it->second; }
static void put(const char *k, const S &v) { if (v.empty()) kv.erase(k); else kv[k] = v; }

static void parse_settings(const S &text, std::map<S, S> &m) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == S::npos) e = text.size();
        S line = text.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t a = line.find_first_not_of(" \t"); if (a == S::npos) continue;
        line = line.substr(a);
        if (_strnicmp(line.c_str(), "set \"", 5)) continue;
        size_t eq = line.find('=', 5), end = line.rfind('"');
        if (eq == S::npos || end == S::npos || end < eq) continue;
        m[line.substr(5, eq - 5)] = line.substr(eq + 1, end - eq - 1);
    }
}
/* settings.ini (runtime/win/settings_ini.h): NAME=VALUE lines shared with the game, whose in-game menu writes it; its
   values win over settings.bat, which is still written for the .bat launcher. */
static void parse_ini(const S &text, std::map<S, S> &m) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == S::npos) e = text.size();
        S line = text.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t a = line.find_first_not_of(" \t"); if (a == S::npos) continue;
        line = line.substr(a);
        if (line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('='); if (eq == S::npos || !eq) continue;
        S k = line.substr(0, eq); while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k.empty() || k.find_first_of(" \t") != S::npos) continue;
        S v = line.substr(eq + 1); size_t b = v.find_first_not_of(" \t"); v = b == S::npos ? S() : v.substr(b);
        if (v.empty()) m.erase(k); else m[k] = v;
    }
}
static bool load_settings() {
    S t; if (!read_text(here + L"settings.bat", t)) return false;
    kv.clear(); parse_settings(t, kv);
    S ini; if (read_text(here + L"settings.ini", ini)) parse_ini(ini, kv);
    return true;
}

static void save_settings() {
    static const char *known[] = { "TOTK_DATA", "TOTK_WINDOW_SIZE", "TOTK_FULLSCREEN", "TOTK_VK_VSYNC_OFF", "TOTK_FPS60", "TOTK_UNCAPPED_HZ",
        "TOTK_RENDER_RES", "TOTK_AUDIO_OFF", "TOTK_VOLUME", "TOTK_LANG", "TOTK_MOUSE", "TOTK_RUMBLE", "TOTK_GYRO", "TOTK_FACE_BUTTONS", "TOTK_VK_DEVICE", "TOTK_XLATE" };
    S o = "@echo off\r\nrem Settings for \"Tears of the Kingdom " + dispver + "\". Written by the launcher's Settings page;\r\n"
          "rem you may also edit it by hand: lines that start with rem are off, a missing line means the game's default.\r\n";
    for (const char *k : known) { auto it = kv.find(k); if (it != kv.end() && !it->second.empty()) o += S("set \"") + k + "=" + it->second + "\"\r\n"; }
    bool first = true;
    for (auto &p : kv) {
        bool k = false; for (const char *n : known) if (p.first == n) k = true;
        if (k) continue;
        if (first) { o += "rem Other variables kept as they were:\r\n"; first = false; }
        o += "set \"" + p.first + "=" + p.second + "\"\r\n";
    }
    write_text(here + L"settings.bat", o);
    S ini = "; Tears of the Kingdom " + dispver + " settings, shared by the launcher and the in-game menu (F1 / Esc).\r\n"
            "; NAME=VALUE; a variable set in the environment overrides its line.\r\n";
    for (auto &p : kv) if (!p.second.empty()) ini += p.first + "=" + p.second + "\r\n";
    write_text(here + L"settings.ini", ini);
}

/* ---------- checks ---------- */
struct Checks { bool data = false, dotnet = false, vulkan = false, python = false; S dataMsg, dotnetMsg, vulkanMsg, pythonMsg; std::vector<S> gpus; } chk;
static WS data_dir() { return W(expand(get("TOTK_DATA"))); }
static WS romfs_dir() { return data_dir() + L"\\extracted\\" + W(vdir) + L"\\romfs"; }
static WS exefs_dir() { return data_dir() + L"\\extracted\\" + W(vdir) + L"\\exefs"; }

static void check_data() {
    WS probe = romfs_dir() + L"\\romfs.bin";
    chk.data = exists(probe) && exists(exefs_dir() + L"\\main");
    chk.dataMsg = chk.data ? "Game files for " + dispver + " found" : "No game files for " + dispver + " yet (looked for " + U(probe) + ")";
}
static void check_dotnet() {
    chk.dotnet = false;
    S x = get("TOTK_XLATE");
    if (x.empty()) {   /* the bundled NativeAOT translator (bin\xlate): no .NET runtime needed */
        if (exists(here + L"bin\\xlate\\totk_xlate.exe")) { chk.dotnet = true; chk.dotnetMsg = "Shader translator: bundled (no .NET needed)"; return; }
        x = "dotnet.exe";
    }
    if (_stricmp(x.c_str(), "dotnet.exe") && _stricmp(x.c_str(), "dotnet")) { chk.dotnet = true; chk.dotnetMsg = "custom shader translator command"; return; }
    wchar_t exe[1024];
    if (!SearchPathW(NULL, L"dotnet.exe", NULL, 1024, exe, NULL)) {
        wchar_t pf[1024]; if (!GetEnvironmentVariableW(L"ProgramFiles", pf, 1024)) wcscpy(pf, L"C:\\Program Files");
        swprintf(exe, 1024, L"%ls\\dotnet\\dotnet.exe", pf);
        if (!exists(exe)) { chk.dotnetMsg = ".NET 10 runtime not installed (the game needs it to translate shaders)"; return; }
    }
    WS dir = exe; dir.resize(dir.rfind(L'\\'));
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\shared\\Microsoft.NETCore.App\\10.*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); chk.dotnet = true; chk.dotnetMsg = ".NET 10 runtime found"; }
    else chk.dotnetMsg = ".NET 10 runtime not installed (found dotnet.exe, but no Microsoft.NETCore.App 10.x)";
}
/* Vulkan 1.3 device check with hand-declared minimal types (no SDK needed); also lists device names for the GPU setting */
typedef int (__stdcall *PFN_ci)(const void *, const void *, void **);
typedef void (__stdcall *PFN_di)(void *, const void *);
typedef int (__stdcall *PFN_ep)(void *, unsigned *, void **);
typedef void (__stdcall *PFN_gp)(void *, void *);
typedef void *(__stdcall *PFN_gipa)(void *, const char *);
static void check_vulkan() {
    const unsigned v13 = (1u << 22) | (3u << 12);
    chk.vulkan = false; chk.gpus.clear();
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) { chk.vulkanMsg = "No Vulkan driver (vulkan-1.dll missing): install the latest NVIDIA/AMD/Intel graphics driver"; return; }
    PFN_gipa gipa = (PFN_gipa)GetProcAddress(vk, "vkGetInstanceProcAddr");
    PFN_ci ci = gipa ? (PFN_ci)gipa(NULL, "vkCreateInstance") : NULL;
    if (!ci) { chk.vulkanMsg = "The Vulkan loader is broken: reinstall your graphics driver"; return; }
    struct { int sType; const void *pNext; const char *an; unsigned av; const char *en; unsigned ev; unsigned api; } app = { 0, NULL, "totk-launcher", 1, NULL, 0, v13 };
    struct { int sType; const void *pNext; unsigned flags; const void *app; unsigned nl; const char *const *l; unsigned ne; const char *const *e; } info = { 1, NULL, 0, &app, 0, NULL, 0, NULL };
    void *inst = NULL; int r = ci(&info, NULL, &inst);
    if (r != 0 || !inst) { chk.vulkanMsg = fmt("Vulkan 1.3 could not be initialised (error %d): update your graphics driver", r); return; }
    PFN_ep ep = (PFN_ep)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_gp gp = (PFN_gp)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_di di = (PFN_di)gipa(inst, "vkDestroyInstance");
    void *devs[16]; unsigned n = 16, best = 0; S bestName;
    if (ep && gp && ep(inst, &n, devs) >= 0)
        for (unsigned i = 0; i < n; i++) {
            static char props[2048]; gp(devs[i], props);
            unsigned v = *(unsigned *)props; S name = props + 20;
            chk.gpus.push_back(name + (v >= v13 ? "" : " (no Vulkan 1.3)"));
            if (v > best) { best = v; bestName = name; }
        }
    else n = 0;
    if (di) di(inst, NULL);
    if (best < v13) chk.vulkanMsg = n ? fmt("No GPU with Vulkan 1.3 (best: %u.%u): update your graphics driver", best >> 22, (best >> 12) & 0x3ff) : "No Vulkan device found: update your graphics driver";
    else { chk.vulkan = true; chk.vulkanMsg = "Vulkan 1.3: " + bestName; }
}
static WS find_python() {
    WS p = here + L"support\\python\\python.exe";
    if (exists(p)) return p;
    wchar_t exe[1024];
    if (SearchPathW(NULL, L"python.exe", NULL, 1024, exe, NULL) && !wcsstr(exe, L"WindowsApps")) return exe;
    return WS();
}
static void check_python() {
    WS p = find_python(); chk.python = !p.empty() && exists(here + L"support\\totk_setup.py");
    chk.pythonMsg = !chk.python ? "Bundled Python (support\\python) is missing, and no Python was found on PATH: import needs it"
                  : p.find(L"support\\python") != WS::npos ? "Bundled Python" : "System Python (needs: pip install cryptography lz4)";
}

/* ---------- launching the game ---------- */
static void setv(const char *k, const S &v) { SetEnvironmentVariableW(W(k).c_str(), W(v).c_str()); }
/* returns "" when the game ran and exited with 0, else a message */
static S launch_game(const WS &logName = L"totk.log") {
    WS exe = here + L"bin\\totk-win.exe";
    if (!exists(exe)) return "bin\\totk-win.exe is missing from\n" + U(here);
    check_data();
    if (!chk.data) return "The game data for Tears of the Kingdom " + dispver + " was not found:\n" + U(romfs_dir()) + "\\romfs.bin\n\nStart \"Tears of the Kingdom.exe\" and run the setup.";
    check_dotnet(); check_vulkan();
    if (!chk.dotnet) {
        if (MessageBoxW(NULL, L"The .NET 10 runtime is not installed. The game needs it to translate shaders.\n\nInstall \".NET Runtime 10\" (x64) from https://dotnet.microsoft.com/download/dotnet/10.0 and start the game again.\n\nOpen the download page now?",
                        TITLE, MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND) == IDYES) open_path(L"https://dotnet.microsoft.com/download/dotnet/10.0");
        return "!";
    }
    if (!chk.vulkan) return chk.vulkanMsg;
    for (auto &p : kv) setv(p.first.c_str(), expand(p.second));
    S xlate = expand(get("TOTK_XLATE"));
    S h = U(here);
    setv("TOTK_ROMFS", U(romfs_dir()));
    setv("TOTK_ROMFS_INDEX", h + "bin\\runtime-index.bin");
    setv("TOTK_PROFILE_DIR", h + "user-data");
    setv("TOTK_SHADER_CACHE", h + "shader-cache");
    if (xlate.empty() && exists(here + L"bin\\xlate\\totk_xlate.exe")) setv("TOTK_XLATE_CMD", "\"" + h + "bin\\xlate\\totk_xlate.exe\"");
    else setv("TOTK_XLATE_CMD", "\"" + (xlate.empty() ? S("dotnet.exe") : xlate) + "\" \"" + h + "bin\\translator\\totk_xlate.dll\"");
    /* the runtime treats "variable set" as on for these; the settings file stores explicit 0 / 1 */
    if (get("TOTK_AUDIO_OFF") != "1") SetEnvironmentVariableW(L"TOTK_AUDIO_OFF", NULL);
    if (get("TOTK_VK_VSYNC_OFF") != "1") SetEnvironmentVariableW(L"TOTK_VK_VSYNC_OFF", NULL);
    if (get("TOTK_FULLSCREEN") != "1") SetEnvironmentVariableW(L"TOTK_FULLSCREEN", NULL);
    WS logs = here + L"logs"; CreateDirectoryW(logs.c_str(), NULL);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    WS logp = logs + L"\\" + logName;
    HANDLE log = CreateFileW(logp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = nul; si.hStdOutput = nul; si.hStdError = log;
    WS line = L"\"" + exe + L"\" \"" + exefs_dir() + L"\" boot";
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), &line[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, here.c_str(), &si, &pi)) return fmt("Could not start bin\\totk-win.exe (error %lu).", GetLastError());
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(log); CloseHandle(nul);
    return rc ? fmt("The game exited with code %lu.\n\nSee the log:\n", rc) + U(logp) : S();
}

/* ---------- jobs (helper process on a worker thread) ---------- */
static std::mutex mu;
static std::vector<S> logLines;
static std::atomic<bool> running(false);
static std::atomic<float> progress(0);
static float progBase = 0, progSpan = 1;   /* a helper's "@@progress x" fills [progBase, progBase + progSpan] (setup = import + shaders) */
static S jobStatus, jobError, jobDone, resVersion, resVdir, resData;
static HANDLE jobProc = NULL;
static std::atomic<bool> scrollLog(false);

static void logline(const S &s) { std::lock_guard<std::mutex> l(mu); logLines.push_back(s); scrollLog = true; }

/* run support\totk_setup.py with args; fills res* / jobError; returns the exit code */
static int run_helper(const WS &args) {
    WS py = find_python();
    if (py.empty()) { jobError = "Python not found (support\\python is missing)"; return 1; }
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8"); SetEnvironmentVariableW(L"PYTHONUTF8", L"1"); SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1"); SetEnvironmentVariableW(L"PYTHONWARNINGS", L"ignore");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    WS cmd = L"\"" + py + L"\" \"" + here + L"support\\totk_setup.py\" " + args;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(py.c_str(), &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, (here + L"support").c_str(), &si, &pi)) { CloseHandle(rd); CloseHandle(wr); jobError = "Could not start Python"; return 1; }
    CloseHandle(wr); CloseHandle(pi.hThread); jobProc = pi.hProcess;
    S acc; char buf[4096]; DWORD n;
    auto handle = [&](const S &l) {
        if (l.compare(0, 11, "@@progress ") == 0) progress = progBase + progSpan * (float)atof(l.c_str() + 11);
        else if (l.compare(0, 10, "@@version ") == 0) { size_t sp = l.find(' ', 10); resVdir = l.substr(10, sp - 10); resVersion = l.substr(sp + 1); logline("Detected game version " + resVersion); }
        else if (l.compare(0, 7, "@@data ") == 0) resData = l.substr(7);
        else if (l.compare(0, 8, "@@error ") == 0) { jobError = l.substr(8); logline("ERROR: " + jobError); }
        else if (l.compare(0, 7, "@@done ") == 0) { jobDone = l.substr(7); logline(jobDone); }
        else if (!l.empty()) { logline(l); jobStatus = l; }
    };
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        acc.append(buf, n);
        size_t p;
        while ((p = acc.find('\n')) != S::npos) { S l = acc.substr(0, p); if (!l.empty() && l.back() == '\r') l.pop_back(); acc.erase(0, p + 1); handle(l); }
    }
    if (!acc.empty()) handle(acc);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc); CloseHandle(pi.hProcess); jobProc = NULL;
    if (rc && jobError.empty()) { jobError = fmt("The helper stopped with exit code %lu (see the log)", rc); logline("ERROR: " + jobError); }
    return (int)rc;
}
static WS q(const S &s) { return L"\"" + W(s) + L"\""; }
static void start_job(std::function<void()> fn) {
    if (running) return;
    running = true; progress = 0; progBase = 0; progSpan = 1; jobError.clear(); jobDone.clear(); jobStatus.clear(); resVersion.clear(); resVdir.clear(); resData.clear();
    std::thread([fn] { fn(); progress = jobError.empty() ? 1.0f : progress.load(); running = false; }).detach();
}

/* ---------- sibling packages (other game versions) ---------- */
struct Sibling { S name, display; WS dir; };
static std::vector<Sibling> siblings;
static void scan_siblings() {
    siblings.clear();
    WS me = here; me.pop_back();
    WS parent = me.substr(0, me.rfind(L'\\') + 1);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((parent + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        WS d = parent + fd.cFileName + L"\\"; S t;
        if (d == here || !exists(d + L"Tears of the Kingdom.exe") || !read_text(d + L"bin\\game-version.txt", t)) continue;
        char a[64] = {0}, b[64] = {0}; if (sscanf(t.c_str(), "%63s %63s", a, b) != 2) continue;
        siblings.push_back({ U(fd.cFileName), a, d });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void set_data_in(const WS &pkg, const S &data) {
    S t; if (!read_text(pkg + L"settings.bat", t)) return;
    std::map<S, S> m; parse_settings(t, m);
    /* rewrite just the TOTK_DATA line (keep the rest of the file untouched) */
    S out; size_t pos = 0; bool done = false;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos); if (e == S::npos) e = t.size();
        S line = t.substr(pos, e + 1 - pos); pos = e + 1;
        if (!_strnicmp(line.c_str(), "set \"TOTK_DATA=", 15)) { out += "set \"TOTK_DATA=" + data + "\"\r\n"; done = true; } else out += line;
    }
    if (!done) out += "set \"TOTK_DATA=" + data + "\"\r\n";
    write_text(pkg + L"settings.bat", out);
    S ini; if (!read_text(pkg + L"settings.ini", ini)) return;   /* its TOTK_DATA line would win: rewrite that too */
    S iout; pos = 0;
    while (pos < ini.size()) {
        size_t e = ini.find('\n', pos); if (e == S::npos) e = ini.size();
        S line = ini.substr(pos, e + 1 - pos); pos = e + 1;
        if (_strnicmp(line.c_str(), "TOTK_DATA=", 10)) iout += line;
    }
    if (!iout.empty() && iout.back() != '\n') iout += "\r\n";
    write_text(pkg + L"settings.ini", iout + "TOTK_DATA=" + data + "\r\n");
}

/* ---------- file dialogs ---------- */
static bool pick(bool folder, bool save, const wchar_t *title, const wchar_t *fname, const wchar_t *spec, const wchar_t *defName, S &out) {
    bool ok = false; IFileDialog *d = NULL;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, NULL, CLSCTX_ALL, save ? IID_IFileSaveDialog : IID_IFileOpenDialog, (void **)&d);
    if (FAILED(hr)) return false;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    d->SetTitle(title);
    if (!folder && spec) { COMDLG_FILTERSPEC f[2] = { { fname, spec }, { L"All files", L"*.*" } }; d->SetFileTypes(2, f); }
    if (defName) d->SetFileName(defName);
    if (SUCCEEDED(d->Show(GetActiveWindow()))) {
        IShellItem *it; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U(p); CoTaskMemFree(p); ok = true; } it->Release(); }
    }
    d->Release(); return ok;
}

/* ---------- UI state ---------- */
static int tab = 0;
static char baseP[1024], updP[1024], keysP[1024], outP[1024], exefsP[1024], romfsP[1024], customW[8] = "1920", customH[8] = "1080";
static S banner;                      /* shown on the Play page (game exit message) */
static bool bannerErr = false;
static S profileNote;                 /* after an import: a message about the version profile */
static WS profileOpen;                /* sibling package to open */
static bool wantPlay = false, dirty = false;

static void setbuf(char *b, size_t n, const S &s) { snprintf(b, n, "%s", s.c_str()); }
static void guess_keys() {
    const char *cands[] = { "%USERPROFILE%\\.switch\\prod.keys", "%APPDATA%\\Ryujinx\\system\\prod.keys", "%APPDATA%\\yuzu\\keys\\prod.keys", "%APPDATA%\\eden\\keys\\prod.keys", "%APPDATA%\\suyu\\keys\\prod.keys" };
    for (auto c : cands) { wchar_t b[1024]; if (ExpandEnvironmentStringsW(W(c).c_str(), b, 1024) && exists(b)) { setbuf(keysP, sizeof keysP, U(b)); return; } }
}
static WS profile_dir() { return here + L"user-data"; }

/* "Prepare shaders" (support\prepare_shaders.py through totk_setup.py): the shared shader list translated from the game
   files into <package>\shader-cache before play. Done once per package; resumable. */
static bool shaders_prepared(const WS &pkg) { return exists(pkg + L"shader-cache\\prepared.json"); }
static void job_shaders(WS pkg, float base = 0) {   /* base: where the progress bar stands (0.6 after an import in the same job) */
    logline("Preparing shaders from your game files (one time; takes a few minutes, uses all CPU cores) ...");
    jobStatus = "Preparing shaders";
    progBase = base; progSpan = 1 - base;
    run_helper(L"shaders --pkg " + q(U(pkg)));
}
static void job_import(S base, S upd, S keys, S out) {
    WS common = L"--base " + q(base) + (upd.empty() ? L"" : L" --update " + q(upd)) + L" --keys " + q(keys) + L" --output " + q(out);
    logline("Checking the game version ...");
    progBase = 0; progSpan = 0.6f;   /* import 0-60 %, shaders 60-100 % */
    if (run_helper(L"detect " + common)) return;
    jobStatus = "Version " + resVersion;
    S ver = resVdir, disp = resVersion;
    WS target = here;
    if (ver != vdir) {
        const Sibling *sib = nullptr; for (auto &s : siblings) if (s.display == disp) sib = &s;
        if (!sib) { jobError = "These files are Tears of the Kingdom " + disp + ", but this package runs only " + dispver + " and no \"" + disp + "\" package was found next to it. Supported: 1.0.0, 1.1.1, 1.2.0, 1.4.3 (one package each, build_windows.py makes them)."; logline("ERROR: " + jobError); return; }
        logline("Version " + disp + " belongs to the package \"" + sib->name + "\"; the files go there.");
        target = sib->dir; profileOpen = sib->dir; profileNote = "Imported " + disp + " game files. They belong to the \"" + sib->name + "\" package: open it to play.";
    }
    progress = progBase + progSpan * 0.12f;
    S okOut = out;
    if (run_helper(L"import " + common)) return;
    if (target == here) { put("TOTK_DATA", okOut); profileNote = "Imported game version " + disp + "."; save_settings(); } else set_data_in(target, okOut);
    check_data();
    job_shaders(target, 0.6f);
}
static void job_adopt(S ex, S ro, S out) {
    WS common = L"--exefs " + q(ex) + L" --romfs " + q(ro) + L" --output " + q(out);
    progBase = 0; progSpan = 0.6f;
    if (run_helper(L"adopt --probe " + common)) return;
    S ver = resVdir, disp = resVersion; WS target = here;
    if (ver != vdir) {
        const Sibling *sib = nullptr; for (auto &s : siblings) if (s.display == disp) sib = &s;
        if (!sib) { jobError = "These files are Tears of the Kingdom " + disp + ", but this package runs only " + dispver + " and no \"" + disp + "\" package was found next to it."; logline("ERROR: " + jobError); return; }
        target = sib->dir; profileOpen = sib->dir; profileNote = "Game files " + disp + " belong to the \"" + sib->name + "\" package: open it to play.";
    }
    if (run_helper(L"adopt " + common)) return;
    if (target == here) { put("TOTK_DATA", resData); save_settings(); profileNote = "Using game version " + disp + "."; } else set_data_in(target, resData);
    check_data();
    job_shaders(target, 0.6f);
}

/* ---------- widgets ---------- */
static bool path_row(const char *label, char *buf, size_t n, bool folder, const wchar_t *fname = nullptr, const wchar_t *spec = nullptr) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    float bw = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 + 4;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##p", buf, n);
    ImGui::SameLine();
    bool changed = false;
    if (ImGui::Button("Browse...")) { S r; if (pick(folder, false, W(label).c_str(), fname, spec, nullptr, r)) { setbuf(buf, n, r); changed = true; } }
    ImGui::PopID(); return changed;
}
static void status_line(bool ok, const S &msg) {
    ImGui::TextColored(ok ? ImVec4(0.35f, 0.85f, 0.45f, 1) : ImVec4(0.95f, 0.45f, 0.35f, 1), ok ? "[ OK ]" : "[ !! ]"); ImGui::SameLine(); ImGui::TextWrapped("%s", msg.c_str());
}
static void log_panel(float h) {
    ImGui::BeginChild("log", ImVec2(0, h), true);
    { std::lock_guard<std::mutex> l(mu);
      for (auto &s : logLines) { bool e = s.compare(0, 5, "ERROR") == 0; if (e) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.5f, 0.4f, 1)); ImGui::TextWrapped("%s", s.c_str()); if (e) ImGui::PopStyleColor(); } }
    if (scrollLog.exchange(false)) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
static void job_bar() {
    if (running) { ImGui::ProgressBar(progress, ImVec2(-1, 0)); ImGui::TextDisabled("%s", jobStatus.substr(0, 140).c_str()); }
    else if (!jobError.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", jobError.c_str());
    else if (!jobDone.empty()) ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1), "%s", jobDone.c_str());
}
static bool mod(bool c) { if (c) dirty = true; return c; }

/* Setup is done when the game files are there and the shaders are prepared (both kept on disk), so the one-button setup
   runs once; it is shown again only when the game files are deleted or "Run setup again" is chosen (Game files). */
static bool setupMode = false;
static bool setup_done() { return chk.data && shaders_prepared(here); }
static void big_button_colors() { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.30f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.36f, 1)); }
static void page_setup() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.back());
    ImGui::Text("Set up Tears of the Kingdom");
    ImGui::PopFont();
    ImGui::TextDisabled("Native Windows port - game version %s. One time: about 20 GB of disk and 10-20 minutes.", dispver.c_str());
    ImGui::Spacing();
    bool haveData = chk.data;
    if (running) ImGui::BeginDisabled();
    if (haveData) {
        status_line(true, chk.dataMsg + "\n" + U(data_dir()));
        ImGui::TextWrapped("The game files are in place. What is left: preparing the shaders from them (it continues where it stopped).");
    } else {
        ImGui::TextWrapped("Pick your own dump of the game: the base game (1.0.0, .nsp or .xci), the update NSP for 1.1.1 / 1.2.0 / 1.4.3 if this package is one of those, and your prod.keys. Setup imports and verifies the files, then prepares the shaders.");
        ImGui::Spacing();
        path_row("Base game (.nsp / .xci)", baseP, sizeof baseP, false, L"Switch game", L"*.nsp;*.xci");
        path_row("Update (optional .nsp)", updP, sizeof updP, false, L"Update NSP", L"*.nsp");
        path_row("prod.keys", keysP, sizeof keysP, false, L"prod.keys", L"*.keys");
        path_row("Store the game data in", outP, sizeof outP, true);
    }
    if (running) ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Spacing();
    bool ready = !running && (haveData || (baseP[0] && keysP[0] && outP[0]));
    if (!ready) ImGui::BeginDisabled();
    big_button_colors();
    const char *label = running ? "Setting up..." : haveData ? "Continue setup" : "Set up";
    if (ImGui::Button(label, ImVec2(ImGui::GetFontSize() * 12, ImGui::GetFontSize() * 2.6f))) {
        { std::lock_guard<std::mutex> l(mu); logLines.clear(); } profileNote.clear(); profileOpen.clear();
        if (haveData) start_job([] { job_shaders(here); });
        else { S b = baseP, u = updP, k = keysP, o = outP; start_job([=] { job_import(b, u, k, o); }); }
    }
    ImGui::PopStyleColor(2);
    if (!ready) ImGui::EndDisabled();
    if (running) { ImGui::SameLine(); if (ImGui::Button("Stop") && jobProc) { TerminateProcess(jobProc, 1); logline("Stopped. Setup continues from here next time."); } }
    ImGui::Spacing();
    job_bar();
    if (!profileNote.empty()) { ImGui::TextWrapped("%s", profileNote.c_str()); if (!profileOpen.empty() && !running && ImGui::Button("Open that package")) { WS exe = profileOpen + L"Tears of the Kingdom.exe"; ShellExecuteW(NULL, L"open", exe.c_str(), NULL, profileOpen.c_str(), SW_SHOWNORMAL); PostQuitMessage(0); } }
    if (!running && setup_done()) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.45f, 1), "Setup is complete.");
        big_button_colors();
        if (ImGui::Button("Continue", ImVec2(ImGui::GetFontSize() * 8, 0))) { setupMode = false; tab = 0; }
        ImGui::PopStyleColor(2);
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Details")) log_panel(ImGui::GetFontSize() * 12);
}
static void page_play() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.back());
    ImGui::Text("Tears of the Kingdom");
    ImGui::PopFont();
    ImGui::TextDisabled("Native Windows port - game version %s", dispver.c_str());
    ImGui::Spacing();
    if (!banner.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, bannerErr ? ImVec4(0.95f, 0.5f, 0.4f, 1) : ImVec4(0.8f, 0.8f, 0.8f, 1)); ImGui::TextWrapped("%s", banner.c_str()); ImGui::PopStyleColor(); ImGui::Spacing(); }
    bool can = chk.data && chk.vulkan && chk.dotnet && !running;
    ImGui::Spacing();
    if (!can) ImGui::BeginDisabled();
    big_button_colors();
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.back());
    if (ImGui::Button("PLAY", ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 2.4f))) wantPlay = true;
    ImGui::PopFont();
    ImGui::PopStyleColor(2);
    if (!can) ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Spacing();
    bool fs = get("TOTK_FULLSCREEN") == "1";
    if (ImGui::Checkbox("Start in fullscreen##f", &fs)) { put("TOTK_FULLSCREEN", fs ? "1" : ""); dirty = true; }
    ImGui::Spacing();
    bool prep = shaders_prepared(here);
    if (!chk.vulkan || !chk.dotnet || !chk.data || !prep) {
        status_line(chk.data, chk.dataMsg);
        status_line(chk.vulkan, chk.vulkanMsg);
        status_line(chk.dotnet, chk.dotnetMsg);
        status_line(prep, prep ? "Shaders prepared" : "Shaders not prepared: Game files > Run setup again");
    } else if (ImGui::CollapsingHeader("System check")) {
        status_line(chk.data, chk.dataMsg); status_line(chk.vulkan, chk.vulkanMsg); status_line(chk.dotnet, chk.dotnetMsg); status_line(prep, "Shaders prepared");
    }
    ImGui::TextDisabled("In game: F11 or Alt+Enter toggles fullscreen.");
}

/* Deletes this package's extracted game files (<data>\extracted\<ver>) and the "prepared" mark, so setup runs again. */
static S delete_game_files() {
    WS dir = data_dir() + L"\\extracted\\" + W(vdir);
    if (!exists(dir + L"\\romfs\\romfs.bin")) return "No game files to delete in " + U(dir);
    WS from = dir; from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int rc = SHFileOperationW(&op);
    DeleteFileW((here + L"shader-cache\\prepared.json").c_str());
    check_data();
    return rc || op.fAnyOperationsAborted ? fmt("Deleting %s failed (error %d).", U(dir).c_str(), rc) : S();
}

static void page_files() {
    ImGui::Text("Game files"); ImGui::Separator();
    check_data();
    status_line(chk.data, chk.dataMsg + (chk.data ? "\n" + U(data_dir()) : ""));
    status_line(shaders_prepared(here), shaders_prepared(here) ? "Shaders prepared" : "Shaders not prepared");
    if (running) ImGui::BeginDisabled();
    if (ImGui::Button("Run setup again")) { setupMode = true; }
    if (chk.data) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.18f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.25f, 0.22f, 1));
        if (ImGui::Button("Delete game files...")) ImGui::OpenPopup("Delete game files?");
        ImGui::PopStyleColor(2);
    }
    if (running) ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Delete game files?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This deletes the extracted game files of %s (about 17 GB):", dispver.c_str());
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "%s\\extracted\\%s", U(data_dir()).c_str(), vdir.c_str());
        ImGui::TextWrapped("Your saves, settings and your NSP / keys are not touched. Setup has to run again before you can play.");
        if (ImGui::Button("Delete")) { S e = delete_game_files(); banner = e.empty() ? "Game files deleted." : e; bannerErr = !e.empty(); if (e.empty()) setupMode = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!banner.empty() && !setupMode) ImGui::TextWrapped("%s", banner.c_str());
    ImGui::Spacing();
    bool busy = running;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::CollapsingHeader("Import from NSP / XCI (recommended)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Pick your own dump: the base game (1.0.0) and, for 1.1.1 / 1.2.0 / 1.4.3, that version's update NSP, plus your prod.keys. The version is detected from the game's code and verified. Needs about 20 GB free.");
        path_row("Base game (.nsp / .xci)", baseP, sizeof baseP, false, L"Switch game", L"*.nsp;*.xci");
        path_row("Update (optional .nsp)", updP, sizeof updP, false, L"Update NSP", L"*.nsp");
        path_row("prod.keys", keysP, sizeof keysP, false, L"prod.keys", L"*.keys");
        path_row("Store the game data in", outP, sizeof outP, true);
        bool ready = baseP[0] && keysP[0] && outP[0];
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button("Import game files")) { S b = baseP, u = updP, k = keysP, o = outP; { std::lock_guard<std::mutex> l(mu); logLines.clear(); } profileNote.clear(); profileOpen.clear(); start_job([=] { job_import(b, u, k, o); }); }
        if (!ready) ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("%s", chk.python ? chk.pythonMsg.c_str() : chk.pythonMsg.c_str());
    }
    if (ImGui::CollapsingHeader("Use extracted folders (exefs + romfs)")) {
        ImGui::TextWrapped("For files already extracted by this launcher's import: the ExeFS folder (main, rtld, sdk, subsdk0, ...) and the RomFS folder that holds romfs.bin. A folder of loose RomFS files cannot be used.");
        path_row("ExeFS folder", exefsP, sizeof exefsP, true);
        path_row("RomFS folder (with romfs.bin)", romfsP, sizeof romfsP, true);
        bool ready = exefsP[0] && romfsP[0];
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button("Use these folders")) { S e = exefsP, r = romfsP, o = outP[0] ? outP : U(here) + "data"; { std::lock_guard<std::mutex> l(mu); logLines.clear(); } profileNote.clear(); profileOpen.clear(); start_job([=] { job_adopt(e, r, o); }); }
        if (!ready) ImGui::EndDisabled();
    }
    if (busy) ImGui::EndDisabled();
    ImGui::Spacing();
    job_bar();
    if (running) { if (ImGui::Button("Cancel") && jobProc) { TerminateProcess(jobProc, 1); logline("Cancelled."); } }
    if (!profileNote.empty()) { ImGui::TextWrapped("%s", profileNote.c_str()); if (!profileOpen.empty() && !running && ImGui::Button("Open that package")) { WS exe = profileOpen + L"Tears of the Kingdom.exe"; ShellExecuteW(NULL, L"open", exe.c_str(), L"--setup", profileOpen.c_str(), SW_SHOWNORMAL); PostQuitMessage(0); } }
    ImGui::Spacing();
    log_panel(ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 4);
    ImGui::TextDisabled("Packages for other versions next to this one:");
    for (auto &s : siblings) { ImGui::SameLine(); ImGui::TextDisabled("%s", s.display.c_str()); }
    if (siblings.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(none)"); }
    ImGui::TextDisabled("Supported: 1.0.0, 1.1.1, 1.2.0, 1.4.3. Other versions (such as 1.4.2) cannot run.");
}

struct Opt { const char *label, *val; };
static bool combo_opt(const char *label, const char *key, const Opt *o, int n) {
    S cur = get(key); int sel = 0; for (int i = 0; i < n; i++) if (cur == o[i].val) sel = i;
    bool ch = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo(label, o[sel].label)) { for (int i = 0; i < n; i++) if (ImGui::Selectable(o[i].label, i == sel)) { put(key, o[i].val); ch = true; } ImGui::EndCombo(); }
    return ch;
}
static bool flag(const char *label, const char *key, bool onWhenSet, const char *offVal = "") {   /* checkbox <-> key */
    bool v = onWhenSet ? get(key) == "1" : get(key) != "0";
    if (ImGui::Checkbox(label, &v)) { put(key, onWhenSet ? (v ? "1" : "") : (v ? "" : "0")); (void)offVal; return true; }
    return false;
}
static S dir_size(const WS &dir, int &files) {
    unsigned long long total = 0; files = 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue; total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; files++; } while (FindNextFileW(h, &fd)); FindClose(h); }
    return fmt("%.1f MiB", total / 1048576.0);
}
static void page_settings() {
    ImGui::Text("Settings"); ImGui::SameLine(); ImGui::TextDisabled("(saved to settings.bat; applied at the next start of the game)"); ImGui::Separator();
    ImGui::BeginChild("set", ImVec2(0, 0));
    ImGui::SeparatorText("Video");
    static const Opt sizes[] = { { "1280 x 720", "1280x720" }, { "1600 x 900", "1600x900" }, { "1920 x 1080", "1920x1080" }, { "2560 x 1440", "2560x1440" }, { "3840 x 2160", "3840x2160" } };
    { S cur = get("TOTK_WINDOW_SIZE", "1920x1080"); int sel = -1; for (int i = 0; i < 5; i++) if (cur == sizes[i].val) sel = i;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
      if (ImGui::BeginCombo("Window size", sel >= 0 ? sizes[sel].label : cur.c_str())) { for (int i = 0; i < 5; i++) if (ImGui::Selectable(sizes[i].label, i == sel)) { put("TOTK_WINDOW_SIZE", sizes[i].val); dirty = true; } ImGui::EndCombo(); }
      ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4); ImGui::InputText("##cw", customW, sizeof customW, ImGuiInputTextFlags_CharsDecimal);
      ImGui::SameLine(); ImGui::Text("x"); ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4); ImGui::InputText("##ch", customH, sizeof customH, ImGuiInputTextFlags_CharsDecimal);
      ImGui::SameLine(); if (ImGui::Button("Set custom")) { int w = atoi(customW), h = atoi(customH); if (w >= 320 && h >= 180 && w <= 8192 && h <= 8192) { put("TOTK_WINDOW_SIZE", fmt("%dx%d", w, h)); dirty = true; } } }
    { bool fs = get("TOTK_FULLSCREEN") == "1"; if (ImGui::Checkbox("Start in fullscreen", &fs)) { put("TOTK_FULLSCREEN", fs ? "1" : ""); dirty = true; } }
    { bool vs = get("TOTK_VK_VSYNC_OFF") != "1"; if (ImGui::Checkbox("VSync", &vs)) { put("TOTK_VK_VSYNC_OFF", vs ? "" : "1"); dirty = true; } }
    static const Opt rr[] = { { "Game default (720p)", "" }, { "1280 x 720", "1280x720" }, { "1600 x 900", "1600x900" }, { "1920 x 1080", "1920x1080" }, { "2560 x 1440", "2560x1440" } };
    if (combo_opt("Render resolution", "TOTK_RENDER_RES", rr, 5)) dirty = true;
    { std::vector<S> items = { "Automatic" }; for (auto &g : chk.gpus) items.push_back(g);
      int sel = 0; S cur = get("TOTK_VK_DEVICE"); if (!cur.empty()) sel = std::min((int)items.size() - 1, atoi(cur.c_str()) + 1);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
      if (ImGui::BeginCombo("Graphics card", items[sel].c_str())) { for (int i = 0; i < (int)items.size(); i++) if (ImGui::Selectable(items[i].c_str(), i == sel)) { put("TOTK_VK_DEVICE", i ? fmt("%d", i - 1) : ""); dirty = true; } ImGui::EndCombo(); } }
    ImGui::SeparatorText("Frame rate");
    { bool on = get("TOTK_FPS60") != "0"; if (ImGui::Checkbox("60 FPS patch (off = the game's original 30 FPS)", &on)) { put("TOTK_FPS60", on ? "" : "0"); dirty = true; }
      static const Opt cap[] = { { "Automatic (display refresh)", "" }, { "30", "30" }, { "60", "60" }, { "90", "90" }, { "120", "120" }, { "144", "144" }, { "165", "165" }, { "240", "240" } };
      if (!on) ImGui::BeginDisabled();
      if (combo_opt("Frame rate cap", "TOTK_UNCAPPED_HZ", cap, 8)) dirty = true;
      if (!on) ImGui::EndDisabled(); }
    ImGui::SeparatorText("Audio");
    { bool mute = get("TOTK_AUDIO_OFF") == "1"; if (ImGui::Checkbox("Mute (run without sound)", &mute)) { put("TOTK_AUDIO_OFF", mute ? "1" : "0"); dirty = true; }
      int vol = get("TOTK_VOLUME").empty() ? 100 : atoi(get("TOTK_VOLUME").c_str()); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
      if (ImGui::SliderInt("Volume", &vol, 0, 100, "%d%%")) { put("TOTK_VOLUME", vol >= 100 ? "" : fmt("%d", vol)); dirty = true; } }
    ImGui::SeparatorText("Language");
    static const Opt lang[] = { { "Automatic (system)", "" }, { "English (US)", "en-US" }, { "English (UK)", "en-GB" }, { "Japanese", "ja" }, { "French", "fr" }, { "French (Canada)", "fr-CA" }, { "German", "de" },
        { "Italian", "it" }, { "Spanish", "es" }, { "Spanish (Latin America)", "es-419" }, { "Dutch", "nl" }, { "Portuguese", "pt" }, { "Russian", "ru" }, { "Korean", "ko" }, { "Chinese (Simplified)", "zh-CN" }, { "Chinese (Traditional)", "zh-TW" } };
    if (combo_opt("Game language", "TOTK_LANG", lang, 16)) dirty = true;
    ImGui::SeparatorText("Input");
    { bool m = get("TOTK_MOUSE") == "1"; if (ImGui::Checkbox("Mouse as right stick (buttons = ZR / ZL / R3)", &m)) { put("TOTK_MOUSE", m ? "1" : ""); dirty = true; } }
    if (mod(flag("Controller rumble", "TOTK_RUMBLE", false))) {}
    if (mod(flag("Controller motion (gyro)", "TOTK_GYRO", false))) {}
    static const Opt fb[] = { { "Automatic", "" }, { "By label (A is A)", "label" }, { "By position (Nintendo layout)", "position" } };
    if (combo_opt("Face buttons", "TOTK_FACE_BUTTONS", fb, 3)) dirty = true;
    ImGui::SeparatorText("Shader cache and translator");
    { int files; S sz = dir_size(here + L"shader-cache", files); ImGui::Text("Shader cache: %s in %d files (always on: it is what makes later runs smooth)", sz.c_str(), files);
      if (ImGui::Button("Open shader cache")) open_path(here + L"shader-cache"); ImGui::SameLine();
      if (ImGui::Button("Clear shader cache")) ImGui::OpenPopup("clearsc");
      if (ImGui::BeginPopupModal("clearsc", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::Text("Delete all cached shaders? The next run translates them again (slow, stuttery).");
          if (ImGui::Button("Delete")) { WIN32_FIND_DATAW fd; WS d = here + L"shader-cache"; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((d + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); } ImGui::CloseCurrentPopup(); }
          ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup(); ImGui::EndPopup(); } }
    { static char x[512]; static bool init = false; if (!init) { setbuf(x, sizeof x, get("TOTK_XLATE", "dotnet.exe")); init = true; }
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
      if (ImGui::InputText("Translator host command", x, sizeof x)) { put("TOTK_XLATE", S(x) == "dotnet.exe" ? "" : x); dirty = true; } }
    ImGui::SeparatorText("Logs");
    if (ImGui::Button("Open game log")) { WS l = here + L"logs\\totk.log"; if (exists(l)) open_path(l); else open_path(here + L"logs"); } ImGui::SameLine();
    if (ImGui::Button("Open logs folder")) open_path(here + L"logs");
    ImGui::SameLine(); if (ImGui::Button("Reset all settings")) { S data = get("TOTK_DATA"); kv.clear(); put("TOTK_DATA", data); put("TOTK_AUDIO_OFF", "0"); dirty = true; }
    ImGui::EndChild();
}

static S save_root_path() {
    S raw; if (!read_text(profile_dir() + L"\\account.uid", raw) || raw.size() != 16) return S();
    unsigned long long lo = 0, hi = 0; memcpy(&lo, raw.data(), 8); memcpy(&hi, raw.data() + 8, 8);
    return U(profile_dir()) + fmt("\\save\\%016llx%016llx", hi, lo);
}
static void page_saves() {
    ImGui::Text("Saves"); ImGui::Separator();
    S root = save_root_path();
    ImGui::TextWrapped("The game reads and writes its save in:"); ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), "%s", root.empty() ? (U(profile_dir()) + "\\save\\<account id>  (created on the first start)").c_str() : root.c_str());
    int slots = 0;
    if (!root.empty()) { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((W(root) + L"\\slot_*").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do slots++; while (FindNextFileW(h, &fd)); FindClose(h); } }
    ImGui::Text("Save slots found: %d", slots);
    ImGui::Spacing();
    bool busy = running; if (busy) ImGui::BeginDisabled();
    ImGui::TextWrapped("Import a save from Ryujinx, yuzu / Eden / Suyu or a Checkpoint dump: choose the folder that contains slot_00 (or a .zip of it). The current save is backed up first (user-data\\save-backups).");
    auto prof = [] { return U(profile_dir()); };
    if (ImGui::Button("Import save folder...")) { S r; if (pick(true, false, L"Save folder (contains slot_00)", nullptr, nullptr, nullptr, r)) { { std::lock_guard<std::mutex> l(mu); logLines.clear(); } S p = prof(); start_job([=] { run_helper(L"save-import --src " + q(r) + L" --profile " + q(p)); }); } }
    ImGui::SameLine();
    if (ImGui::Button("Import save zip...")) { S r; if (pick(false, false, L"Save zip", L"Zip archive", L"*.zip", nullptr, r)) { { std::lock_guard<std::mutex> l(mu); logLines.clear(); } S p = prof(); start_job([=] { run_helper(L"save-import --src " + q(r) + L" --profile " + q(p)); }); } }
    ImGui::SameLine();
    if (ImGui::Button("Export save (zip)...")) { S r; if (pick(false, true, L"Export save", L"Zip archive", L"*.zip", L"TotK-save.zip", r)) { { std::lock_guard<std::mutex> l(mu); logLines.clear(); } S p = prof(); start_job([=] { run_helper(L"save-export --dest " + q(r) + L" --profile " + q(p)); }); } }
    if (busy) ImGui::EndDisabled();
    if (ImGui::Button("Open saves folder")) { S r = save_root_path(); WS d = r.empty() ? profile_dir() : W(r); CreateDirectoryW(d.c_str(), NULL); open_path(d); }
    ImGui::SameLine(); if (ImGui::Button("Open backups folder")) { WS d = profile_dir() + L"\\save-backups"; CreateDirectoryW(d.c_str(), NULL); open_path(d); }
    ImGui::Spacing(); job_bar(); ImGui::Spacing();
    log_panel(ImGui::GetContentRegionAvail().y - ImGui::GetFontSize() * 2.5f);
    ImGui::TextDisabled("Quit the game before importing. Saves made with another game version may not load in an older one.");
}

static void page_controls() {
    ImGui::Text("Controls"); ImGui::Separator();
    ImGui::SeparatorText("Keyboard (Mac layout, by key position)");
    ImGui::BulletText("W A S D: left stick       Arrow keys: right stick");
    ImGui::BulletText("J: A   K or Space: B   I: X   U: Y   Z / C: stick clicks   Q: L   E: R");
    ImGui::BulletText("Left Alt: ZL   Left Shift: ZR   Enter: +   Tab: -   F / T / H / G: D-pad");
    ImGui::BulletText("In-game menu: F1 or Esc (controller: Minus + Plus together)");
    ImGui::BulletText("1-5: shortcut keys   F11 or Alt+Enter: fullscreen");
    ImGui::SeparatorText("Controllers");
    ImGui::TextWrapped("Any SDL3-supported gamepad works (Xbox / XInput, DualSense, DualShock 4, Switch Pro): plug it in or pair it before or during the game. Rumble and motion can be turned off in Settings. Face-button mapping can follow the labels or the Nintendo positions.");
    ImGui::SeparatorText("Mouse");
    ImGui::TextWrapped("Optional (Settings > Input): the mouse moves the right stick; left / right / middle button = ZR / ZL / R3.");
    ImGui::SeparatorText("More");
    ImGui::TextWrapped("Keys and controller buttons can be rebound in the game: F1 > Controls > Remap buttons (saved to controls.txt, see README-windows.md). This window opens every time you start \"Tears of the Kingdom.exe\".");
    if (ImGui::Button("Open package folder")) open_path(here);
}

/* ---------- Win32 + D3D11 ---------- */
static ID3D11Device *dev; static ID3D11DeviceContext *ctx; static IDXGISwapChain *sc; static ID3D11RenderTargetView *rtv;
static void make_rtv() { ID3D11Texture2D *b; sc->GetBuffer(0, IID_PPV_ARGS(&b)); dev->CreateRenderTargetView(b, NULL, &rtv); b->Release(); }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE: if (dev && w != SIZE_MINIMIZED) { if (rtv) { rtv->Release(); rtv = NULL; } sc->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); make_rtv(); } return 0;
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_CLOSE: if (running && MessageBoxW(h, L"A job is still running. Quit anyway?", TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0; if (running && jobProc) TerminateProcess(jobProc, 1); break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void style(float s) {
    ImGuiStyle &st = ImGui::GetStyle(); ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0; st.FrameRounding = 5; st.ChildRounding = 6; st.GrabRounding = 5; st.PopupRounding = 6; st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(9, 8); st.ScrollbarRounding = 6;
    ImVec4 *c = st.Colors; const ImVec4 acc(0.30f, 0.52f, 0.95f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.095f, 0.11f, 1); c[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.08f, 0.09f, 1); c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.15f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1); c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.22f, 0.26f, 1); c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.26f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.28f, 1); c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.31f, 0.40f, 1); c[ImGuiCol_ButtonActive] = acc;
    c[ImGuiCol_Header] = ImVec4(0.17f, 0.20f, 0.28f, 1); c[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.28f, 0.40f, 1); c[ImGuiCol_HeaderActive] = acc;
    c[ImGuiCol_CheckMark] = acc; c[ImGuiCol_SliderGrab] = acc; c[ImGuiCol_PlotHistogram] = acc; c[ImGuiCol_Separator] = ImVec4(0.2f, 0.22f, 0.27f, 1);
    st.ScaleAllSizes(s);
}

static void gui(int startTab) {
    tab = startTab;
    HINSTANCE hi = GetModuleHandleW(NULL);
    HDC sdc = GetDC(NULL); UINT dpi = GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(NULL, sdc); float scale = dpi / 96.0f;
    WNDCLASSEXW wc = { sizeof wc, CS_CLASSDC, wndproc, 0, 0, hi, LoadIconW(hi, MAKEINTRESOURCEW(1)), LoadCursorW(NULL, IDC_ARROW), NULL, NULL, L"TotkLauncher", NULL };
    RegisterClassExW(&wc);
    int ww = (int)(1000 * scale), wh = (int)(660 * scale);
    RECT r = { 0, 0, ww, wh }; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Tears of the Kingdom", WS_OVERLAPPEDWINDOW, (sx - (r.right - r.left)) / 2, (sy - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, lv, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, lv, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { fail("Could not create the setup window (Direct3D 11 is unavailable)."); return; }
    make_rtv();
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); io.IniFilename = NULL;
    style(scale);
    ImFont *f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * scale, NULL, io.Fonts->GetGlyphRangesDefault());
    if (!f) io.Fonts->AddFontDefault();
    ImFont *big = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 30.0f * scale);
    if (!big) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(dev, ctx);
    bool quit = false;
    static const char *names[] = { "Play", "Game files", "Settings", "Saves", "Controls" };
    while (!quit) {
        MSG msg; while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) quit = true; }
        if (quit) break;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (setupMode) {   /* first run (or "Run setup again"): one screen, no tabs */
            ImGui::BeginChild("setup", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
            page_setup();
            ImGui::EndChild();
            ImGui::End(); ImGui::Render();
            const float clear0[4] = { 0.09f, 0.095f, 0.11f, 1 };
            ctx->OMSetRenderTargets(1, &rtv, NULL); ctx->ClearRenderTargetView(rtv, clear0);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            sc->Present(1, 0);
            if (dirty) { dirty = false; save_settings(); }
            continue;
        }
        float sw = 170 * scale;
        ImGui::BeginChild("side", ImVec2(sw, 0), false);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        for (int i = 0; i < 5; i++) {
            bool sel = tab == i; if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.52f, 0.95f, 1));
            if (ImGui::Button(names[i], ImVec2(-1, 42 * scale))) tab = i;
            if (sel) ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("page", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
        if (tab == 0) page_play(); else if (tab == 1) page_files(); else if (tab == 2) page_settings(); else if (tab == 3) page_saves(); else page_controls();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        const float clear[4] = { 0.09f, 0.095f, 0.11f, 1 };
        ctx->OMSetRenderTargets(1, &rtv, NULL); ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        sc->Present(1, 0);
        if (dirty) { dirty = false; save_settings(); check_dotnet(); }
        if (wantPlay) {
            wantPlay = false; save_settings();
            ShowWindow(hwnd, SW_HIDE);
            S err = launch_game();
            banner = err == "!" ? "" : err.empty() ? "The game closed normally." : err; bannerErr = !err.empty() && err != "!";
            if (bannerErr) logline("ERROR: " + err);
            check_data(); check_vulkan();
            if (err.empty()) { DestroyWindow(hwnd); }   /* normal quit: done */
            else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); tab = 0; }
        }
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (rtv) rtv->Release(); sc->Release(); ctx->Release(); dev->Release();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t me[1024]; GetModuleFileNameW(NULL, me, 1024);
    here = me; here.resize(here.rfind(L'\\') + 1);
    S gv; char a[64] = {0}, b[64] = {0};
    if (!read_text(here + L"bin\\game-version.txt", gv) || sscanf(gv.c_str(), "%63s %63s", a, b) != 2) { fail("bin\\game-version.txt is missing or damaged in\n" + U(here) + "\n\nThe package looks incomplete; rebuild it with build_windows.py."); return 1; }
    dispver = a; vdir = b;
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    /* The launcher window always opens (setup on the first run, then Play / Game files / Settings / Saves / Controls).
       --play starts the game without it (scripts, benchmarks); --setup opens the Settings page; --tab <0-4> a page. */
    bool play = false; int startTab = -1;
    for (int i = 1; i < argc; i++) { if (!wcscmp(argv[i], L"--setup")) startTab = 2; else if (!wcscmp(argv[i], L"--play")) play = true; else if (!wcscmp(argv[i], L"--tab") && i + 1 < argc) startTab = _wtoi(argv[++i]); }
    if (!load_settings()) { fail("settings.bat was not found next to the launcher:\n" + U(here + L"settings.bat")); return 1; }
    check_data();
    if (play) {
        S err = launch_game();
        if (!err.empty() && err != "!") fail(err);
        return err.empty() ? 0 : 1;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    scan_siblings(); check_vulkan(); check_dotnet(); check_python();
    guess_keys(); setbuf(outP, sizeof outP, U(here) + "data");
    if (!chk.data && get("TOTK_DATA").find("%~dp0") == S::npos && !get("TOTK_DATA").empty()) setbuf(outP, sizeof outP, expand(get("TOTK_DATA")));
    setupMode = !setup_done() && startTab < 0;
    gui(startTab >= 0 ? startTab : 0);
    return 0;
}
