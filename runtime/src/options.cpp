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
