// WetRecomp host: boots the recompiled WET executable.
//
//   WetRecomp [--game <dir>] [--hdd <dir>] [--log <file>] [--selftest]
//
// --game  folder holding the extracted disc (default.xex, media, lu0, ...).
//         Default: game/extracted, searched from the current folder upwards.
// --hdd   writable folder for save data and cache. Default: <game>/../hdd
// --log   log file. Default: wet.log in the current folder.
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

namespace fs = std::filesystem;
using namespace wet;

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
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
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
    Log("WET Recomp (first-boot runtime)");

    if (gameDir.empty())
        gameDir = FindDefaultGameDir(argv[0]);
    if (gameDir.empty() || !fs::exists(gameDir / "default.xex"))
    {
        LogFatal("Could not find the game. Run tools/prepare_game.py first, or pass --game <folder containing default.xex>.");
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
