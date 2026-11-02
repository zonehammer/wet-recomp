// WET Setup: one window that turns a WET disc image into a playable build.
//
// Pick the ISO (or an already extracted disc folder) and a folder, press
// Install. Setup then
//   1. gets the tools: Git, CMake, Ninja, Clang (llvm-mingw) and Python - the
//      ones already installed are used, the rest are downloaded (latest
//      releases, from GitHub and nuget.org) into <folder>\tools,
//   2. clones (or updates) github.com/zonehammer/wet-recomp into <folder>\wet-recomp
//      and checks out the latest release,
//   3. runs tools\build.py --iso <ISO or folder> there (extract or copy, recompile, build),
//   4. adds a desktop shortcut to the launcher, and if asked, deletes what only
//      the build needed (launcher/maintenance.h) and the disc image.
// Running it again updates and rebuilds (the disc is only needed the first time).
// The launcher's Update button runs it as: WetSetup.exe --update <folder>.
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
#include <shellapi.h>
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

#include "maintenance.h"

namespace fs = std::filesystem;

namespace
{
    constexpr wchar_t kTitle[] = L"WET Recompiled Setup";
    constexpr char kRepoUrl[] = "https://github.com/zonehammer/wet-recomp.git";

    enum
    {
        kIso = 100, kIsoBrowse, kDir, kDirBrowse, kInstall, kStatus, kProgress, kLog, kShortcut, kPlay,
        kFromIso, kFromFolder, kCleanup, kDeleteIso, kDetails, kSourceGroup, kSourceNote, kFree
    };

    HWND s_window;
    HFONT s_font, s_titleFont;
    bool s_updateMode = false;          // --update: started by the launcher's Update button
    bool s_showLog = false;
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

    // The latest release's tag (v0.2.75), or empty if GitHub can't be reached.
    std::wstring LatestReleaseTag()
    {
        std::string json;
        if (!Get(L"https://api.github.com/repos/zonehammer/wet-recomp/releases/latest", &json, nullptr))
            return {};
        std::smatch m;
        std::regex tag("\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
        return std::regex_search(json, m, tag) ? Widen(m[1]) : std::wstring();
    }

    // A git command's output (first line), and whether it succeeded.
    bool GitOutput(const fs::path& repo, const std::wstring& args, std::string& out)
    {
        out.clear();
        DWORD code = Run(L"git -C " + Quote(repo) + L" " + args, repo, [&](const std::string& line) {
            if (out.empty()) out = line;
        });
        return code == 0;
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

    struct Choices
    {
        fs::path source;        // ISO or extracted folder (only the first time)
        fs::path root;
        bool shortcut = true, cleanup = false, deleteIso = false;
    };

    void Install(Choices c)
    {
        const fs::path& iso = c.source;
        const fs::path& root = c.root;
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
                Fail(L"Choose your WET disc image (.iso) or extracted disc folder first.");
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

            // 2. Source: the latest release (or, if GitHub's release list can't
            // be read, the release this Setup belongs to; failing that the
            // latest code). A copy that's on a branch (someone working on the
            // code) is only fast-forwarded, and one with changes is left as it is.
            Progress(-1);
            std::string version = WET_VERSION;   // "0.2.0 (abc1234)"
            std::wstring tag = LatestReleaseTag();
            if (tag.empty())
                tag = L"v" + Widen(version.substr(0, version.find(' ')));
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
            Run(L"git -C " + Quote(repo) + L" fetch --tags --force origin", root);
            std::string branch, changes;
            bool onBranch = GitOutput(repo, L"symbolic-ref -q --short HEAD", branch) && !branch.empty();
            GitOutput(repo, L"status --porcelain --untracked-files=no --ignore-submodules=all", changes);
            if (!changes.empty())
                Log(L"The source folder has changes of its own: building it as it is (no update).");
            else if (onBranch)
            {
                Log(L"The source folder is on branch " + Widen(branch) + L": updating that branch.");
                if (Run(L"git -C " + Quote(repo) + L" pull --ff-only", root) != 0)
                    Log(L"(couldn't update the branch - building the code as it is)");
            }
            else if (Run(L"git -C " + Quote(repo) + L" checkout --force " + tag, root) == 0)
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
            if (c.shortcut) MakeShortcut(s_installed);
            message = L"Done. Start the game with the launcher" +
                      std::wstring(c.shortcut ? L" (\"WET Recompiled\" on the desktop)." : L".");

            // 5. Clean up, if asked.
            if (c.cleanup)
            {
                Status(L"Freeing up space");
                uint64_t freed = 0;
                for (const auto& part : { wet::maintenance::Leftovers(repo), wet::maintenance::Tools(repo) })
                {
                    Log(L"Deleting " + part.what + L": " + wet::maintenance::Size(part.bytes));
                    if (wet::maintenance::Delete(part))
                        freed += part.bytes;
                    else
                        Log(L"(some of it couldn't be deleted)");
                }
                message += L"\n\nFreed " + wet::maintenance::Size(freed) +
                           L" (the next update downloads the tools again and rebuilds from scratch).";
            }
            if (c.deleteIso && !haveGame && fs::is_regular_file(iso, ec) &&
                fs::exists(repo / L"game" / L"extracted" / L"default.xex", ec))
            {
                fs::remove(iso, ec);
                Log(ec ? L"Couldn't delete the disc image " + iso.wstring() : L"Deleted the disc image " + iso.wstring());
                if (!ec)
                    message += L"\n\nThe disc image was deleted (the game's files are in wet-recomp\\game\\extracted).";
            }
            ok = true;
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

    bool FromFolder() { return SendMessageW(Item(kFromFolder), BM_GETCHECK, 0, 0) == BST_CHECKED; }

    // A folder picker; empty if cancelled.
    fs::path PickFolder()
    {
        fs::path result;
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                    reinterpret_cast<void**>(&dlg))))
            return result;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->Show(s_window)) && SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        dlg->Release();
        return result;
    }

    void BrowseSource()
    {
        if (FromFolder())
        {
            fs::path dir = PickFolder();
            if (!dir.empty())
                SetWindowTextW(Item(kIso), dir.c_str());
            return;
        }
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
        fs::path dir = PickFolder();
        if (!dir.empty())
            SetWindowTextW(Item(kDir), (dir.filename() == L"WET-Recomp" ? dir : dir / L"WET-Recomp").c_str());
    }

    // The source choice: ISO or folder (the "delete the disc image" option only
    // makes sense for an ISO).
    void UpdateSourceChoice()
    {
        bool folder = FromFolder();
        SetWindowTextW(Item(kSourceNote), folder
                           ? L"The folder with default.xex, lu0, media, streams0... (from extract-xiso, Xenia, etc.). It's copied."
                           : L"A dump of your own disc (Redump's \"WET (USA, Europe)\" and others).");
        EnableWindow(Item(kDeleteIso), !folder && !s_busy);
        if (folder)
            SendMessageW(Item(kDeleteIso), BM_SETCHECK, BST_UNCHECKED, 0);
    }

    // Free space on the install folder's drive.
    void UpdateFreeSpace()
    {
        fs::path dir = Text(kDir);
        std::error_code ec;
        while (!dir.empty() && !fs::exists(dir, ec) && dir.has_parent_path() && dir.parent_path() != dir)
            dir = dir.parent_path();
        ULARGE_INTEGER freeBytes{};
        std::wstring text;
        if (!dir.empty() && GetDiskFreeSpaceExW(dir.c_str(), &freeBytes, nullptr, nullptr))
            text = wet::maintenance::Size(freeBytes.QuadPart) + L" free on that drive; about 14 GB is needed (6 GB once "
                                                                 L"the disc is extracted).";
        SetWindowTextW(Item(kFree), text.c_str());
    }

    void SetBusy(bool busy)
    {
        s_busy = busy;
        for (int id : { kIso, kIsoBrowse, kDir, kDirBrowse, kShortcut, kFromIso, kFromFolder, kCleanup })
            EnableWindow(Item(id), !busy);
        EnableWindow(Item(kDeleteIso), !busy && !FromFolder());
        SetWindowTextW(Item(kInstall), busy ? L"Cancel" : L"Install");
    }

    // The window, with or without the detailed log.
    void Layout()
    {
        RECT r{ 0, 0, 640, s_showLog ? 660 : 422 };
        AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        SetWindowPos(s_window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        ShowWindow(Item(kLog), s_showLog ? SW_SHOW : SW_HIDE);
        SetWindowTextW(Item(kDetails), s_showLog ? L"Hide details" : L"Show details");
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
        Choices c;
        c.source = Text(kIso);
        c.root = Text(kDir);
        const fs::path& dir = c.root;
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
        auto checked = [](int id) { return SendMessageW(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; };
        c.shortcut = checked(kShortcut);
        c.cleanup = checked(kCleanup);
        c.deleteIso = checked(kDeleteIso) && !FromFolder();
        std::error_code ec;
        if (FromFolder() && !c.source.empty() && !fs::is_directory(c.source, ec))
        {
            MessageBoxW(s_window, L"That isn't a folder. Choose the folder of your extracted disc.", kTitle, MB_ICONINFORMATION);
            return;
        }
        if (c.deleteIso && !s_updateMode &&
            MessageBoxW(s_window,
                        (L"Delete the disc image when it's been extracted?\n\n" + c.source.wstring() +
                         L"\n\nThe game only needs the extracted files; you'd need the disc image again only to "
                         L"install somewhere else.").c_str(),
                        kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        s_cancel = false;
        ShowWindow(Item(kPlay), SW_HIDE);
        SetBusy(true);
        s_worker = std::thread(Install, c);
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
            case kIsoBrowse: BrowseSource(); return 0;
            case kDirBrowse: BrowseDir(); UpdateFreeSpace(); return 0;
            case kFromIso:
            case kFromFolder: SetWindowTextW(Item(kIso), L""); UpdateSourceChoice(); return 0;
            case kDetails: s_showLog = !s_showLog; Layout(); return 0;
            case kDir:
                if (HIWORD(wp) == EN_KILLFOCUS) UpdateFreeSpace();
                return 0;
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
                    if (s_updateMode)
                    {
                        // Started by the launcher's Update button: back to the launcher.
                        Play();
                        return 0;
                    }
                }
                else if (!s_showLog)
                {
                    s_showLog = true;   // show what went wrong
                    Layout();
                }
                MessageBoxW(hwnd, text->c_str(), kTitle, wp ? MB_ICONINFORMATION : MB_ICONWARNING);
            }
            return 0;
        }
        case WM_APP_PROGRESS:
            SetProgress(int(wp));
            return 0;
        case WM_CTLCOLORSTATIC:
            if (HWND(lp) == Item(kSourceNote) || HWND(lp) == Item(kFree))
            {
                SetTextColor(HDC(wp), GetSysColor(COLOR_GRAYTEXT));
                SetBkMode(HDC(wp), TRANSPARENT);
                return LRESULT(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
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
    // --update <folder>: the launcher's Update button (update and rebuild, then
    // back to the launcher).
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path updateRoot;
    for (int i = 1; i + 1 < argc; i++)
        if (std::wstring(argv[i]) == L"--update")
            updateRoot = argv[i + 1];
    LocalFree(argv);
    s_updateMode = !updateRoot.empty();

    s_window = CreateWindowW(L"WetSetup", kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, instance, nullptr);
    LOGFONTW title = ncm.lfMessageFont;
    title.lfHeight = title.lfHeight * 3 / 2;
    title.lfWeight = FW_BOLD;
    s_titleFont = CreateFontIndirectW(&title);

    // Header: the icon, the name and what Setup does.
    HWND icon = Make(L"STATIC", L"", SS_ICON, 16, 12, 48, 48, 0);
    SendMessageW(icon, STM_SETICON,
                 WPARAM(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR)), 0);
    HWND name = Make(L"STATIC", L"WET Recompiled Setup", 0, 76, 14, 540, 26, 0);
    SendMessageW(name, WM_SETFONT, WPARAM(s_titleFont), TRUE);
    Make(L"STATIC",
         L"Builds the game from your own copy of WET. The build tools are downloaded if they aren't installed; "
         L"nothing from the game is downloaded.",
         0, 76, 40, 548, 32, 0);

    // 1. The game
    Make(L"BUTTON", L"Your copy of WET", BS_GROUPBOX, 12, 78, 616, 96, kSourceGroup);
    HWND fromIso = Make(L"BUTTON", L"Disc image (.iso)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 28, 98, 150, 20, kFromIso);
    Make(L"BUTTON", L"Extracted disc folder", BS_AUTORADIOBUTTON, 190, 98, 190, 20, kFromFolder);
    SendMessageW(fromIso, BM_SETCHECK, BST_CHECKED, 0);
    Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 28, 122, 488, 23, kIso);
    Make(L"BUTTON", L"Browse...", WS_TABSTOP, 524, 121, 88, 25, kIsoBrowse);
    Make(L"STATIC", L"", 0, 28, 150, 588, 18, kSourceNote);

    // 2. Where
    Make(L"BUTTON", L"Install folder", BS_GROUPBOX, 12, 182, 616, 74, 0);
    Make(L"EDIT", (s_updateMode ? updateRoot : DefaultFolder()).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 28,
         204, 488, 23, kDir);
    Make(L"BUTTON", L"Browse...", WS_TABSTOP, 524, 203, 88, 25, kDirBrowse);
    Make(L"STATIC", L"", 0, 28, 232, 588, 18, kFree);

    // 3. Options
    Make(L"BUTTON", L"When it's done", BS_GROUPBOX, 12, 264, 616, 86, 0);
    HWND sc = Make(L"BUTTON", L"Add a desktop shortcut to the launcher", BS_AUTOCHECKBOX | WS_TABSTOP, 28, 284, 580, 20, kShortcut);
    SendMessageW(sc, BM_SETCHECK, BST_CHECKED, 0);
    Make(L"BUTTON",
         L"Free up space: delete the build tools and leftovers (2-4 GB; the next update gets them again and takes longer)",
         BS_AUTOCHECKBOX | WS_TABSTOP, 28, 304, 590, 20, kCleanup);
    Make(L"BUTTON", L"Delete the disc image once it's extracted (the game doesn't need it)",
         BS_AUTOCHECKBOX | WS_TABSTOP, 28, 324, 580, 20, kDeleteIso);

    // Progress and buttons
    Make(L"STATIC", L"Ready.", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 12, 364, 410, 20, kStatus);
    Make(L"BUTTON", L"Play", WS_TABSTOP, 434, 359, 90, 28, kPlay);
    ShowWindow(Item(kPlay), SW_HIDE);
    Make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 530, 359, 98, 28, kInstall);
    Make(PROGRESS_CLASSW, L"", 0, 12, 394, 504, 16, kProgress);
    Make(L"BUTTON", L"Show details", WS_TABSTOP, 524, 390, 104, 24, kDetails);
    Make(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 422, 616, 228, kLog);
    SendMessageW(Item(kLog), EM_SETLIMITTEXT, 0, 0);
    UpdateSourceChoice();
    UpdateFreeSpace();
    Layout();
    RECT wr;
    GetWindowRect(s_window, &wr);
    SetWindowPos(s_window, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 3, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // Already installed there: offer an update (no disc needed).
    std::error_code ec;
    fs::path existing = fs::path(Text(kDir)) / L"wet-recomp";
    if (fs::exists(existing / L"game" / L"extracted" / L"default.xex", ec))
    {
        SetWindowTextW(Item(kInstall), L"Update");
        SetWindowTextW(Item(kStatus), L"Already installed here: Update gets the latest version and rebuilds.");
        for (int id : { kFromIso, kFromFolder, kIso, kIsoBrowse, kDeleteIso })
            EnableWindow(Item(id), FALSE);
        SetWindowTextW(Item(kSourceNote), L"Not needed: the game's files are already in this install.");
    }

    ShowWindow(s_window, show);
    if (s_updateMode)
        PostMessageW(s_window, WM_COMMAND, kInstall, 0);
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
