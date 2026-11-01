// WET Setup: one window that turns a WET disc image into a playable build.
//
// Pick the ISO and a folder, press Install. Setup then
//   1. gets the tools: Git, CMake, Ninja, Clang (llvm-mingw) and Python - the
//      ones already installed are used, the rest are downloaded (latest
//      releases, from GitHub and nuget.org) into <folder>\tools,
//   2. clones (or updates) github.com/zonehammer/wet-recomp into <folder>\wet-recomp,
//   3. runs tools\build.py --iso <ISO> there (extract, recompile, build),
//   4. adds a desktop shortcut to the launcher.
// Running it again updates and rebuilds (the ISO is only needed the first time).
//
// Setup itself contains nothing from the game, so it can be shared; the build
// it makes can't (it contains code translated from the player's disc).
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include "wet_version.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t kTitle[] = L"WET Recompiled Setup";
    constexpr char kRepoUrl[] = "https://github.com/zonehammer/wet-recomp.git";

    enum { kIso = 100, kIsoBrowse, kDir, kDirBrowse, kInstall, kStatus, kProgress, kLog, kShortcut, kPlay };

    HWND s_window;
    HFONT s_font;
    std::thread s_worker;
    std::atomic<bool> s_busy{ false }, s_cancel{ false };
    HANDLE s_job;                       // the running tool (kill on cancel)
    std::mutex s_logLock;
    std::ofstream s_logFile;
    fs::path s_installed;               // the launcher, once built

    constexpr UINT WM_APP_LOG = WM_APP + 1, WM_APP_STATUS = WM_APP + 2, WM_APP_PROGRESS = WM_APP + 3,
                   WM_APP_DONE = WM_APP + 4;

    HWND Item(int id) { return GetDlgItem(s_window, id); }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
        if (n <= 0)   // not UTF-8 (console output in the ANSI code page)
        {
            n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // ---- Messages to the window (from the worker) ----

    void Post(UINT msg, const std::wstring& text, WPARAM w = 0)
    {
        PostMessageW(s_window, msg, w, reinterpret_cast<LPARAM>(new std::wstring(text)));
    }

    void Log(const std::wstring& line)
    {
        {
            std::lock_guard lock(s_logLock);
            if (s_logFile)
                s_logFile << Narrow(line) << "\n" << std::flush;
        }
        Post(WM_APP_LOG, line);
    }
    void Status(const std::wstring& s) { Log(L"== " + s); Post(WM_APP_STATUS, s); }
    void Progress(int percent) { PostMessageW(s_window, WM_APP_PROGRESS, WPARAM(percent), 0); }   // -1 = busy

    struct Failed { std::wstring why; };
    [[noreturn]] void Fail(const std::wstring& why) { throw Failed{ why }; }
    void CheckCancel() { if (s_cancel) Fail(L"Cancelled."); }

    // ---- HTTP ----

    // GET a URL (redirects followed); to a string, or to a file with progress.
    bool Get(const std::wstring& url, std::string* body, const fs::path* file, const wchar_t* what = nullptr)
    {
        URL_COMPONENTS uc{ sizeof(uc) };
        wchar_t host[256] = {}, path[2048] = {};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        wchar_t extra[2048] = {};
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
            return false;
        std::wstring fullPath = std::wstring(path) + extra;
        HINTERNET session = WinHttpOpen(L"WET-Setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
        HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr;
        bool ok = false;
        if (request && WinHttpSendRequest(request, L"User-Agent: WET-Setup\r\n", DWORD(-1), nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                                nullptr);
            uint64_t total = 0;
            wchar_t lenText[32] = {};
            DWORD lenSize = sizeof(lenText);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenText, &lenSize, nullptr))
                total = _wcstoui64(lenText, nullptr, 10);
            std::ofstream out;
            if (file)
                out.open(*file, std::ios::binary);
            ok = status == 200 && (!file || out);
            uint64_t got = 0;
            int lastPercent = -2;
            DWORD size = 0;
            std::vector<char> chunk;
            while (ok && WinHttpQueryDataAvailable(request, &size) && size)
            {
                if (s_cancel) { ok = false; break; }
                chunk.resize(size);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), size, &read) || !read)
                    break;
                if (body) body->append(chunk.data(), read);
                if (file) out.write(chunk.data(), read);
                got += read;
                int percent = total ? int(got * 100 / total) : -1;
                if (what && percent != lastPercent)
                {
                    lastPercent = percent;
                    Progress(percent);
                    wchar_t text[256];
                    std::swprintf(text, 256, L"Downloading %ls: %.0f MB%ls", what, got / 1048576.0,
                                  total ? (L" of " + std::to_wstring(int(total / 1048576)) + L" MB").c_str() : L"");
                    Post(WM_APP_STATUS, text);
                }
            }
            ok = ok && (!total || got == total);
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }

    // ---- Running tools ----

    // Run a command line, each output line to the log (and to `onLine`).
    DWORD Run(const std::wstring& cmd, const fs::path& dir, const std::function<void(const std::string&)>& onLine = {})
    {
        Log(L"$ " + cmd);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE readPipe, writePipe;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
            Fail(L"Couldn't create a pipe.");
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring line = cmd;
        BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                      nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
        CloseHandle(writePipe);
        if (!started)
        {
            CloseHandle(readPipe);
            Fail(L"Couldn't start: " + cmd);
        }
        AssignProcessToJobObject(s_job, pi.hProcess);
        ResumeThread(pi.hThread);
        std::string pending;
        char buf[4096];
        DWORD n;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n)
        {
            pending.append(buf, n);
            size_t p;
            while ((p = pending.find_first_of("\r\n")) != std::string::npos)
            {
                std::string l = pending.substr(0, p);
                pending.erase(0, p + 1);
                if (l.empty()) continue;
                if (onLine) onLine(l);
                // Progress lines ("\r 42% ...") replace each other: only log whole lines.
                if (l.find('%') == std::string::npos || l.rfind("  ", 0) != 0)
                    Log(Widen(l));
            }
        }
        if (!pending.empty())
        {
            if (onLine) onLine(pending);
            Log(Widen(pending));
        }
        CloseHandle(readPipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CheckCancel();
        return code;
    }

    std::wstring Quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

    // ---- Tools ----

    // A program on PATH (not the Microsoft Store's python.exe placeholder).
    fs::path OnPath(const wchar_t* exe)
    {
        std::wstring path(32768, L'\0');
        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size())));
        size_t a = 0;
        while (a <= path.size())
        {
            size_t b = path.find(L';', a);
            if (b == std::wstring::npos) b = path.size();
            fs::path dir = path.substr(a, b - a);
            a = b + 1;
            if (dir.empty() || dir.wstring().find(L"WindowsApps") != std::wstring::npos) continue;
            std::error_code ec;
            if (fs::exists(dir / exe, ec)) return dir / exe;
        }
        return {};
    }

    void AddToPath(const fs::path& dir)
    {
        std::wstring path(32768, L'\0');
        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size())));
        SetEnvironmentVariableW(L"PATH", (dir.wstring() + L";" + path).c_str());
    }

    // The first file matching name under dir (a few levels down).
    fs::path FindUnder(const fs::path& dir, const wchar_t* name)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
        {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (!it->is_directory(ec) && it->path().filename() == name) return it->path();
        }
        return {};
    }

    // Latest GitHub release asset whose name matches `pattern`.
    std::wstring LatestAsset(const char* repo, const char* pattern)
    {
        std::string json;
        std::wstring api = L"https://api.github.com/repos/" + Widen(repo) + L"/releases/latest";
        if (!Get(api, &json, nullptr))
            Fail(L"Couldn't reach GitHub to find " + Widen(repo) + L". Check the internet connection.");
        std::regex url("\"browser_download_url\"\\s*:\\s*\"([^\"]+)\"");
        std::regex want(pattern, std::regex::icase);
        for (std::sregex_iterator it(json.begin(), json.end(), url), end; it != end; ++it)
        {
            std::string u = (*it)[1];
            std::string name = u.substr(u.find_last_of('/') + 1);
            if (std::regex_match(name, want)) return Widen(u);
        }
        Fail(L"No download of " + Widen(repo) + L" matched what Setup expects.");
    }

    // Python from nuget.org (a full, portable Python; the newest 3.12).
    std::wstring PythonPackage()
    {
        std::string json;
        if (!Get(L"https://api.nuget.org/v3-flatcontainer/python/index.json", &json, nullptr))
            Fail(L"Couldn't reach nuget.org for Python. Check the internet connection.");
        std::regex ver("\"(3\\.12\\.[0-9]+)\"");
        std::string best;
        int bestPatch = -1;
        for (std::sregex_iterator it(json.begin(), json.end(), ver), end; it != end; ++it)
        {
            std::string v = (*it)[1];
            int patch = std::atoi(v.c_str() + 5);
            if (patch > bestPatch) { bestPatch = patch; best = v; }
        }
        if (best.empty()) Fail(L"No Python 3.12 package on nuget.org.");
        return Widen("https://api.nuget.org/v3-flatcontainer/python/" + best + "/python." + best + ".nupkg");
    }

    struct Tool
    {
        const wchar_t* label;
        const wchar_t* exe;                         // what must end up on PATH
        std::function<std::wstring()> url;          // where to get it
    };

    void EnsureTool(const Tool& t, const fs::path& toolsDir)
    {
        CheckCancel();
        if (fs::path found = OnPath(t.exe); !found.empty())
        {
            Log(std::wstring(t.label) + L": using " + found.wstring());
            return;
        }
        fs::path dir = toolsDir / t.label;
        fs::path exe = FindUnder(dir, t.exe);
        if (exe.empty())
        {
            std::wstring url = t.url();
            Log(std::wstring(t.label) + L": downloading " + url);
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            fs::path zip = toolsDir / (std::wstring(t.label) + L".zip");
            if (!Get(url, nullptr, &zip, t.label))
            {
                CheckCancel();
                Fail(std::wstring(L"Couldn't download ") + t.label + L".");
            }
            Status(std::wstring(L"Unpacking ") + t.label);
            Progress(-1);
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            if (Run(Quote(fs::path(sys) / L"tar.exe") + L" -xf " + Quote(zip) + L" -C " + Quote(dir), {}) != 0)
                Fail(std::wstring(L"Couldn't unpack ") + t.label + L" (Windows 10 1803 or later is needed).");
            fs::remove(zip, ec);
            exe = FindUnder(dir, t.exe);
            if (exe.empty()) Fail(std::wstring(t.label) + L" didn't contain " + t.exe + L".");
        }
        Log(std::wstring(t.label) + L": " + exe.wstring());
        AddToPath(exe.parent_path());
    }

    // ---- Shortcut ----

    void MakeShortcut(const fs::path& target)
    {
        wchar_t desktop[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
            return;
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                    reinterpret_cast<void**>(&link))))
            return;
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(target.parent_path().c_str());
        link->SetDescription(L"WET Recompiled");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))))
        {
            file->Save((fs::path(desktop) / L"WET Recompiled.lnk").c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }

    // ---- The install ----

    void Install(fs::path iso, fs::path root, bool shortcut)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;
        std::wstring message;
        try
        {
            std::error_code ec;
            fs::create_directories(root, ec);
            if (!fs::is_directory(root, ec)) Fail(L"Couldn't create " + root.wstring());
            {
                std::lock_guard lock(s_logLock);
                s_logFile.open(root / "setup.log", std::ios::app);
            }
            Log(L"WET Recompiled Setup, installing to " + root.wstring());
            fs::path repo = root / L"wet-recomp";
            bool haveGame = fs::exists(repo / L"game" / L"extracted" / L"default.xex", ec);
            if (!haveGame && (iso.empty() || !fs::exists(iso, ec)))
                Fail(L"Choose your WET disc image (.iso) first.");
            ULARGE_INTEGER freeBytes{};
            GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr);
            uint64_t need = (haveGame ? 6ull : 14ull) << 30;
            if (freeBytes.QuadPart && freeBytes.QuadPart < need &&
                MessageBoxW(s_window,
                            (L"The drive has " + std::to_wstring(freeBytes.QuadPart >> 30) + L" GB free; about " +
                             std::to_wstring(need >> 30) + L" GB is needed. Continue anyway?").c_str(),
                            kTitle, MB_YESNO | MB_ICONWARNING) != IDYES)
                Fail(L"Not enough disk space.");

            // 1. Tools
            Status(L"Checking the tools");
            fs::path tools = root / L"tools";
            fs::create_directories(tools, ec);
            const Tool list[] = {
                { L"Git", L"git.exe", [] { return LatestAsset("git-for-windows/git", "MinGit-[0-9.]+-64-bit\\.zip"); } },
                { L"CMake", L"cmake.exe", [] { return LatestAsset("Kitware/CMake", "cmake-[0-9.]+-windows-x86_64\\.zip"); } },
                { L"Ninja", L"ninja.exe", [] { return LatestAsset("ninja-build/ninja", "ninja-win\\.zip"); } },
                { L"Clang", L"clang++.exe", [] { return LatestAsset("mstorsjo/llvm-mingw", "llvm-mingw-[0-9]+-ucrt-x86_64\\.zip"); } },
                { L"Python", L"python.exe", [] { return PythonPackage(); } },
            };
            for (auto& t : list)
                EnsureTool(t, tools);

            // 2. Source: the release this Setup belongs to (tag v<version>), or
            // the latest code if there's no such tag (a development build).
            Progress(-1);
            std::string version = WET_VERSION;   // "0.2.0 (abc1234)"
            const std::wstring tag = L"v" + Widen(version.substr(0, version.find(' ')));
            if (!fs::exists(repo / L".git", ec))
            {
                Status(L"Downloading the source code");
                if (fs::exists(repo, ec) && !fs::is_empty(repo, ec))
                    Fail(repo.wstring() + L" already exists and isn't a copy of the project. Choose another folder.");
                if (Run(L"git clone " + Widen(kRepoUrl) + L" " + Quote(repo), root) != 0)
                    Fail(L"Couldn't download the source code from GitHub.");
            }
            else
                Status(L"Updating the source code");
            Run(L"git -C " + Quote(repo) + L" fetch --tags origin", root);
            if (Run(L"git -C " + Quote(repo) + L" checkout --force " + tag, root) == 0)
                Log(L"Building release " + tag + L".");
            else if (Run(L"git -C " + Quote(repo) + L" checkout --force main", root) != 0 ||
                     Run(L"git -C " + Quote(repo) + L" pull --ff-only", root) != 0)
                Log(L"(couldn't update - building the code as it is)");

            // 3. Build
            Status(L"Building (the first build takes 10-30 minutes)");
            std::wstring cmd = L"python -u tools\\build.py";
            if (!haveGame) cmd += L" --iso " + Quote(iso);
            std::regex ninja("^\\[([0-9]+)/([0-9]+)\\]");
            std::regex percent("^\\s*([0-9]+)%");
            DWORD code = Run(cmd, repo, [&](const std::string& line) {
                std::smatch m;
                if (line.rfind("==> ", 0) == 0)
                {
                    Post(WM_APP_STATUS, Widen(line.substr(4)));
                    Progress(-1);
                }
                else if (std::regex_search(line, m, ninja) && std::stoi(m[2]) > 0)
                    Progress(std::stoi(m[1]) * 100 / std::stoi(m[2]));
                else if (std::regex_search(line, m, percent))
                    Progress(std::stoi(m[1]));
            });
            if (code != 0)
                Fail(L"The build failed. The log above (also in setup.log) says why; Help > Report a bug in the "
                     L"launcher, or a GitHub issue with setup.log attached, gets it looked at.");

            // 4. Done
            s_installed = repo / L"build" / L"runtime" / L"WetLauncher.exe";
            if (!fs::exists(s_installed, ec)) Fail(L"The build finished but WetLauncher.exe isn't there.");
            if (shortcut) MakeShortcut(s_installed);
            ok = true;
            message = L"Done. Start the game with the launcher" +
                      std::wstring(shortcut ? L" (\"WET Recompiled\" on the desktop)." : L".");
        }
        catch (const Failed& f)
        {
            message = f.why;
        }
        catch (const std::exception& e)
        {
            message = L"Unexpected error: " + Widen(e.what());
        }
        Log(message);
        {
            std::lock_guard lock(s_logLock);
            s_logFile.close();
        }
        CoUninitialize();
        Post(WM_APP_DONE, message, ok);
    }

    // ---- Window ----

    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0)
    {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, s_window,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, WPARAM(s_font), TRUE);
        return c;
    }

    std::wstring Text(int id)
    {
        std::wstring s(GetWindowTextLengthW(Item(id)) + 1, L'\0');
        s.resize(GetWindowTextW(Item(id), s.data(), int(s.size())));
        return s;
    }

    void BrowseIso()
    {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{ sizeof(ofn) };
        ofn.hwndOwner = s_window;
        ofn.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
            SetWindowTextW(Item(kIso), file);
    }

    void BrowseDir()
    {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                    reinterpret_cast<void**>(&dlg))))
            return;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->Show(s_window)) && SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                SetWindowTextW(Item(kDir), (fs::path(path) / L"WET-Recomp").c_str());
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dlg->Release();
    }

    void SetBusy(bool busy)
    {
        s_busy = busy;
        for (int id : { kIso, kIsoBrowse, kDir, kDirBrowse, kShortcut })
            EnableWindow(Item(id), !busy);
        SetWindowTextW(Item(kInstall), busy ? L"Cancel" : L"Install");
    }

    void SetProgress(int percent)
    {
        HWND bar = Item(kProgress);
        LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
        if (percent < 0)
        {
            if (!(style & PBS_MARQUEE))
            {
                SetWindowLongPtrW(bar, GWL_STYLE, style | PBS_MARQUEE);
                SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
            }
        }
        else
        {
            if (style & PBS_MARQUEE)
            {
                SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongPtrW(bar, GWL_STYLE, style & ~PBS_MARQUEE);
            }
            SendMessageW(bar, PBM_SETPOS, std::min(percent, 100), 0);
        }
    }

    void StartOrCancel()
    {
        if (s_busy)
        {
            s_cancel = true;
            TerminateJobObject(s_job, 1);
            SetWindowTextW(Item(kStatus), L"Cancelling...");
            return;
        }
        if (s_worker.joinable()) s_worker.join();
        fs::path iso = Text(kIso), dir = Text(kDir);
        if (dir.empty())
        {
            MessageBoxW(s_window, L"Choose a folder to install to.", kTitle, MB_ICONINFORMATION);
            return;
        }
        // The tools and the build don't like spaces or non-ASCII characters much.
        std::wstring d = dir.wstring();
        if (std::any_of(d.begin(), d.end(), [](wchar_t c) { return c > 127; }) &&
            MessageBoxW(s_window,
                        L"The folder's path has non-English characters, which some build tools can't handle. "
                        L"A folder like C:\\WET-Recomp is safer. Continue anyway?",
                        kTitle, MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        s_cancel = false;
        ShowWindow(Item(kPlay), SW_HIDE);
        SetBusy(true);
        bool shortcut = SendMessageW(Item(kShortcut), BM_GETCHECK, 0, 0) == BST_CHECKED;
        s_worker = std::thread(Install, iso, dir, shortcut);
    }

    void Play()
    {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        std::wstring cmd = Quote(s_installed);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           s_installed.parent_path().c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            PostMessageW(s_window, WM_CLOSE, 0, 0);
        }
    }

    void AppendLog(const std::wstring& line)
    {
        HWND log = Item(kLog);
        int len = GetWindowTextLengthW(log);
        if (len > 400000)   // keep the box responsive; setup.log has everything
        {
            SendMessageW(log, EM_SETSEL, 0, 100000);
            SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
            len = GetWindowTextLengthW(log);
        }
        SendMessageW(log, EM_SETSEL, len, len);
        SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case kIsoBrowse: BrowseIso(); return 0;
            case kDirBrowse: BrowseDir(); return 0;
            case kInstall: StartOrCancel(); return 0;
            case kPlay: Play(); return 0;
            }
            break;
        case WM_APP_LOG:
        case WM_APP_STATUS:
        case WM_APP_DONE:
        {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (msg == WM_APP_LOG)
                AppendLog(*text);
            else
                SetWindowTextW(Item(kStatus), text->c_str());
            if (msg == WM_APP_DONE)
            {
                SetBusy(false);
                SetProgress(wp ? 100 : 0);
                if (wp)
                {
                    SetWindowTextW(Item(kInstall), L"Update");
                    ShowWindow(Item(kPlay), SW_SHOW);
                }
                MessageBoxW(hwnd, text->c_str(), kTitle, wp ? MB_ICONINFORMATION : MB_ICONWARNING);
            }
            return 0;
        }
        case WM_APP_PROGRESS:
            SetProgress(int(wp));
            return 0;
        case WM_CLOSE:
            if (s_busy && MessageBoxW(hwnd, L"Setup is still running. Stop it?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            s_cancel = true;
            TerminateJobObject(s_job, 1);
            if (s_worker.joinable()) s_worker.join();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    fs::path DefaultFolder()
    {
        // Next to an existing install if Setup sits in one; otherwise C:\WET-Recomp
        // (short, no spaces - the build tools like that best).
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        fs::path here = fs::path(self).parent_path();
        std::error_code ec;
        if (fs::exists(here / L"wet-recomp" / L".git", ec)) return here;
        wchar_t drive[8] = L"C:";
        GetEnvironmentVariableW(L"SystemDrive", drive, 8);
        return fs::path(std::wstring(drive) + L"\\WET-Recomp");
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    s_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"WetSetup";
    RegisterClassW(&wc);
    RECT r{ 0, 0, 620, 470 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    s_window = CreateWindowW(L"WetSetup", kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             instance, nullptr);

    Make(L"STATIC",
         L"Builds WET Recompiled from your own copy of WET. You need the game's disc image (.iso) and an internet "
         L"connection; the tools it needs are downloaded if they aren't installed.",
         0, 12, 10, 596, 34, 0);
    Make(L"STATIC", L"Disc image", 0, 12, 56, 80, 20, 0);
    Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 96, 53, 420, 22, kIso);
    Make(L"BUTTON", L"Browse...", WS_TABSTOP, 524, 52, 84, 24, kIsoBrowse);
    Make(L"STATIC", L"Install to", 0, 12, 88, 80, 20, 0);
    Make(L"EDIT", DefaultFolder().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 96, 85, 420, 22, kDir);
    Make(L"BUTTON", L"Browse...", WS_TABSTOP, 524, 84, 84, 24, kDirBrowse);
    HWND sc = Make(L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 96, 114, 200, 22, kShortcut);
    SendMessageW(sc, BM_SETCHECK, BST_CHECKED, 0);
    Make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 432, 112, 84, 26, kInstall);
    Make(L"BUTTON", L"Play", WS_TABSTOP, 524, 112, 84, 26, kPlay);
    ShowWindow(Item(kPlay), SW_HIDE);
    Make(L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 148, 596, 20, kStatus);
    Make(PROGRESS_CLASSW, L"", 0, 12, 170, 596, 16, kProgress);
    Make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 194, 596, 266, kLog);
    SendMessageW(Item(kLog), EM_SETLIMITTEXT, 0, 0);

    // Already installed here: offer an update.
    std::error_code ec;
    fs::path existing = DefaultFolder() / L"wet-recomp";
    if (fs::exists(existing / L"game" / L"extracted" / L"default.xex", ec))
    {
        SetWindowTextW(Item(kInstall), L"Update");
        SetWindowTextW(Item(kStatus), L"Already installed here: Update gets the latest version and rebuilds (no disc needed).");
    }

    ShowWindow(s_window, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (IsDialogMessageW(s_window, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(s_job);
    CoUninitialize();
    return 0;
}
