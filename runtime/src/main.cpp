// WetRecomp host: boots the recompiled WET executable.
//
//   WetRecomp [--game <dir>] [--hdd <dir>] [--log <file>] [--selftest]
//
// --game  folder holding the extracted disc (default.xex, media, lu0, ...).
//         Default: game/extracted, searched from the current folder upwards.
// --hdd   writable folder for save data and cache. Default: <game>/../hdd
// --log   log file. Default: wet.log in the current folder.
#include "wet_version.h"
#include <cstdlib>
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/kernel.h"
#include "kernel/threads.h"
#include "log.h"
#include "profiler.h"
#include "options.h"
#include "achievements.h"
#include "gpu/renderer.h"
#include "kernel/xma.h"
#include "memory.h"
#include "module.h"
#include "os.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <map>

namespace fs = std::filesystem;
using namespace wet;

// save_backups: a copy of the saves (and achievements.ini) in
// <hdd>/save-backups/auto-<date>, unless they're the same as the last copy;
// the newest 10 are kept.
static void BackUpSaves(const fs::path& hdd)
{
    std::error_code ec;
    fs::path saves = hdd / "content" / "425307DB";
    if (!fs::exists(saves, ec) || fs::is_empty(saves, ec))
        return;
    auto read = [](const fs::path& p) {
        std::string s;
        if (FILE* f = std::fopen(p.string().c_str(), "rb"))
        {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                s.append(buf, n);
            std::fclose(f);
        }
        return s;
    };
    // Files relative to the saves folder -> contents.
    auto collect = [&](const fs::path& dir, const fs::path& ini) {
        std::map<std::string, std::string> files;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                files[fs::relative(it->path(), dir, ec).generic_string()] = read(it->path());
        if (fs::exists(ini, ec))
            files["achievements.ini"] = read(ini);
        return files;
    };
    fs::path root = hdd / "save-backups";
    std::vector<fs::path> backups;
    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec))
        if (it->is_directory(ec) && it->path().filename().string().rfind("auto-", 0) == 0)
            backups.push_back(it->path());
    std::sort(backups.begin(), backups.end());
    auto now = collect(saves, hdd / "achievements.ini");
    if (!backups.empty() && collect(backups.back() / "425307DB", backups.back() / "achievements.ini") == now)
    {
        Log("Saves: unchanged since the last backup (%s)", backups.back().filename().string().c_str());
        return;
    }
    std::time_t t = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof(name), "auto-%Y-%m-%d_%H-%M-%S", std::localtime(&t));
    fs::path dst = root / name;
    fs::create_directories(dst, ec);
    fs::copy(saves, dst / "425307DB", fs::copy_options::recursive, ec);
    if (fs::exists(hdd / "achievements.ini"))
        fs::copy_file(hdd / "achievements.ini", dst / "achievements.ini", ec);
    if (ec)
    {
        Log("Saves: couldn't back up to %s (%s)", dst.string().c_str(), ec.message().c_str());
        return;
    }
    backups.push_back(dst);
    while (backups.size() > 10)
    {
        fs::remove_all(backups.front(), ec);
        backups.erase(backups.begin());
    }
    Log("Saves: backed up to %s (%zu kept)", dst.string().c_str(), backups.size());
}

static fs::path FindGameDirFrom(fs::path start)
{
    std::error_code ec;
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path())
    {
        fs::path candidate = dir / "game" / "extracted";
        if (fs::exists(candidate / "default.xex", ec))
            return candidate;
        if (dir == dir.parent_path())
            break;
    }
    return {};
}

// game/extracted above the current folder, or above the exe's folder (a
// packaged build started from a shortcut or another folder).
static fs::path FindDefaultGameDir(const char* argv0)
{
    std::error_code ec;
    fs::path found = FindGameDirFrom(fs::current_path(ec));
    if (found.empty() && argv0 && *argv0)
        found = FindGameDirFrom(fs::absolute(fs::path(argv0), ec).parent_path());
    return found;
}

int main(int argc, char** argv)
{
    fs::path gameDir, hddDir;
    std::string logPath = "wet.log";
    // The Windows build is a windowed app (no console); --console or WET_CONSOLE=1
    // opens one showing the log as it is written.
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--console")
            os::OpenConsole();
    if (const char* v = std::getenv("WET_CONSOLE"); v && *v == '1')
        os::OpenConsole();
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--console")
            continue;
        if (a == "--game" && i + 1 < argc) gameDir = argv[++i];
        else if (a == "--hdd" && i + 1 < argc) hddDir = argv[++i];
        else if (a == "--log" && i + 1 < argc) logPath = argv[++i];
        else if (a == "--selftest")
        {
            LogInit("selftest.log");
            if (!g_mem.Init())
                return 1;
            os::InstallCrashHandler(g_mem.base, kGuestSize);
            return g_mem.SelfTest() ? 0 : 1;
        }
        else
        {
            std::fprintf(stderr, "usage: %s [--game <extracted disc folder>] [--hdd <save folder>] [--log <file>]\n", argv[0]);
            return 2;
        }
    }

    LogInit(logPath.c_str());
    Log("WET Recomp %s", WET_VERSION);

    if (gameDir.empty())
        gameDir = FindDefaultGameDir(argv[0]);
    if (gameDir.empty() || !fs::exists(gameDir / "default.xex"))
    {
        LogFatal("Could not find the game. Run tools/prepare_game.py first, or pass --game <folder containing default.xex>.");
        os::ShowFatalMessage("Could not find the game files.\n\nPut your extracted WET disc in game\\extracted next to "
                             "WetRecomp.exe, so that game\\extracted\\default.xex exists.");
        return 1;
    }
    gameDir = fs::absolute(gameDir);
    if (hddDir.empty())
        hddDir = gameDir.parent_path() / "hdd";
    hddDir = fs::absolute(hddDir);

    if (!g_mem.Init())
        return 1;
    os::InstallCrashHandler(g_mem.base, kGuestSize);

    if (!LoadTitleModule(gameDir / "default.xex"))
        return 1;

    InitThreading(g_module.tlsSlotCount, g_module.tlsRawAddr, g_module.tlsDataSize, g_module.tlsRawSize);
    options::Load();
    InitKernelVariables();
    InitXma();
    if (options::SaveBackups())
        BackUpSaves(hddDir);
    InitFileSystem(gameDir, hddDir);
    achievements::Load(hddDir);
    if (options::ShaderCache())
        renderer::SetShaderCacheRoot((hddDir / "shaders").string());

    if (!StartMainThread(g_module.entryPoint, g_module.stackSize))
        return 1;
    os::RaiseTimerResolution();
    StartStallMonitor(10);
    profiler::StartFromEnv();

    // The title runs on its own threads; the process ends when it exits or faults.
    for (;;)
        os::SleepNs(1000000000ull);
}
