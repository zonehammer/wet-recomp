#include "options.h"
#include "log.h"
#include "ppc_context.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace wet::options
{
    namespace
    {
        std::atomic<bool> s_fps60{ false };
        constexpr const char* kFile = "wet.ini";

        void Save()
        {
            if (FILE* f = std::fopen(kFile, "w"))
            {
                std::fprintf(f, "# WET Recomp options (F6 toggles fps60 in game)\n");
                std::fprintf(f, "fps60 = %d\n", s_fps60.load() ? 1 : 0);
                std::fclose(f);
            }
        }
    }

    void Load()
    {
        if (FILE* f = std::fopen(kFile, "r"))
        {
            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                char key[64];
                int value;
                if (std::sscanf(line, " %63[a-z0-9_] = %d", key, &value) == 2 && !std::strcmp(key, "fps60"))
                    s_fps60 = value != 0;
            }
            std::fclose(f);
        }
        else
            Save();   // write the defaults so the file is there to edit
        if (const char* v = std::getenv("WET_FPS"))
            s_fps60 = std::atoi(v) >= 60;
        Log("Options: %s FPS (F6 toggles; saved in %s)", s_fps60 ? "60" : "30", kFile);
    }

    bool Fps60() { return s_fps60; }

    void ToggleFps60()
    {
        s_fps60 = !s_fps60;
        Log("Options: %s FPS", s_fps60 ? "60" : "30");
        Save();
    }
}

// Midasm hook (config/WET_config.toml): the present interval D3D's Present has
// just chosen, in vblanks per frame. The game runs at 2 (30 FPS).
void WetPresentIntervalHook(PPCRegister& r11)
{
    if (r11.u32 == 2 && wet::options::Fps60())
        r11.u64 = 1;
}

// The game world's per-frame update (A2M world, f1 = time step in seconds; it
// stores it at +196 for everything else). The game steps 1/30 s every frame, so
// at 60 FPS everything ran at double speed: halve the step while 60 FPS is on.
extern "C" void __imp__sub_82C32BF8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82C32BF8)
{
    static uint64_t calls = 0;
    static int lastHalved = -1;
    double dt = ctx.f1.f64;
    bool halve = wet::options::Fps60() && dt > 1.0 / 30.0 - 1e-4 && dt < 1.0 / 30.0 + 1e-4;
    if (halve)
        ctx.f1.f64 = dt * 0.5;
    // Diagnostics: the first steps, every change, and one a minute.
    if (calls++ < 3 || int(halve) != lastHalved || calls % 3600 == 0)
        wet::Log("World update: step %.6f s%s", dt, halve ? " (halved for 60 FPS)" : "");
    lastHalved = halve;
    __imp__sub_82C32BF8(ctx, base);
}
