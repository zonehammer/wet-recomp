#include "options.h"
#include "log.h"
#include "gpu/renderer.h"
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
        constexpr int kRates[] = { 30, 60, 120, 144 };
        std::atomic<int> s_fps{ 30 };
        int s_scale = 1, s_scaleRunning = 1;
        std::atomic<int> s_aa{ 0 }, s_filter{ 0 }, s_fullscreen{ 0 }, s_showFps{ 0 };
        constexpr const char* kFile = "wet.ini";

        int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

        int Valid(int fps)
        {
            for (int r : kRates)
                if (fps == r)
                    return r;
            return fps >= 60 ? 60 : 30;
        }

        void ApplyFps()
        {
            // Above 60 the guest's vblank runs faster too (one present per vblank).
            renderer::SetVsyncRate(s_fps > 60 ? uint32_t(s_fps.load()) : 60u);
        }
    }

    void Save()
    {
        if (FILE* f = std::fopen(kFile, "w"))
        {
            std::fprintf(f, "# WET Recomp options (F1 in game opens the settings overlay)\n");
            std::fprintf(f, "# fps: 30 (the game's own), 60, 120 or 144 - F6 cycles in game\n");
            std::fprintf(f, "fps = %d\n", s_fps.load());
            std::fprintf(f, "# resolution_scale: 1 = 720p (the game's own), 2 = 1440p, 3 = 2160p (4K); needs a restart\n");
            std::fprintf(f, "resolution_scale = %d\n", s_scale);
            std::fprintf(f, "# antialiasing: 0 = off, 1 = FXAA, 2 = FXAA extreme\n");
            std::fprintf(f, "antialiasing = %d\n", s_aa.load());
            std::fprintf(f, "# filter (scaling to the window): 0 = bilinear, 1 = AMD CAS sharpening, 2 = AMD FSR\n");
            std::fprintf(f, "filter = %d\n", s_filter.load());
            std::fprintf(f, "fullscreen = %d\n", s_fullscreen.load());
            std::fprintf(f, "show_fps = %d\n", s_showFps.load());
            std::fclose(f);
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
                if (std::sscanf(line, " %63[a-z0-9_] = %d", key, &value) != 2)
                    continue;
                if (!std::strcmp(key, "fps")) s_fps = Valid(value);
                else if (!std::strcmp(key, "resolution_scale")) s_scale = Clamp(value, 1, 3);
                else if (!std::strcmp(key, "antialiasing")) s_aa = Clamp(value, 0, 2);
                else if (!std::strcmp(key, "filter")) s_filter = Clamp(value, 0, 2);
                else if (!std::strcmp(key, "fullscreen")) s_fullscreen = value != 0;
                else if (!std::strcmp(key, "show_fps")) s_showFps = value != 0;
                else if (!std::strcmp(key, "fps60")) s_fps = value ? 60 : 30;   // earlier versions
            }
            std::fclose(f);
        }
        if (const char* v = std::getenv("WET_FPS"))
            s_fps = Valid(std::atoi(v));
        if (const char* v = std::getenv("WET_RES_SCALE"))
            s_scale = Clamp(std::atoi(v), 1, 3);
        s_scaleRunning = s_scale;
        renderer::SetResolutionScale(uint32_t(s_scale));
        Save();
        ApplyFps();
        Log("Options: %d FPS, rendering at %dx%d (resolution_scale %d), antialiasing %d, filter %d; saved in %s",
            s_fps.load(), 1280 * s_scale, 720 * s_scale, s_scale, s_aa.load(), s_filter.load(), kFile);
    }

    int Fps() { return s_fps; }
    void SetFps(int fps)
    {
        s_fps = Valid(fps);
        ApplyFps();
        Log("Options: %d FPS", s_fps.load());
        Save();
    }
    void CycleFps()
    {
        int cur = s_fps, next = kRates[0];
        for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
            if (kRates[i] == cur)
                next = kRates[(i + 1) % (sizeof(kRates) / sizeof(kRates[0]))];
        SetFps(next);
    }

    int ResolutionScale() { return s_scale; }
    int RunningResolutionScale() { return s_scaleRunning; }
    void SetResolutionScale(int scale) { s_scale = Clamp(scale, 1, 3); Save(); }

    int Antialiasing() { return s_aa; }
    void SetAntialiasing(int v) { s_aa = Clamp(v, 0, 2); Save(); }
    int Filter() { return s_filter; }
    void SetFilter(int v) { s_filter = Clamp(v, 0, 2); Save(); }
    bool Fullscreen() { return s_fullscreen; }
    void SetFullscreen(bool v) { s_fullscreen = v; Save(); }
    bool ShowFps() { return s_showFps; }
    void SetShowFps(bool v) { s_showFps = v; Save(); }
}

// Midasm hook (config/WET_config.toml): the present interval D3D's Present has
// just chosen, in vblanks per frame. The game runs at 2 (30 FPS).
void WetPresentIntervalHook(PPCRegister& r11)
{
    if (r11.u32 == 2 && wet::options::Fps() > 30)
        r11.u64 = 1;
}

// The game world's per-frame update (A2M world, f1 = time step in seconds; it
// stores it at +196 for everything else). The game steps 1/30 s every frame, so
// at higher frame rates everything sped up: the step becomes 1/fps.
extern "C" void __imp__sub_82C32BF8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82C32BF8)
{
    static uint64_t calls = 0;
    static int lastHalved = -1;
    double dt = ctx.f1.f64;
    int fps = wet::options::Fps();
    bool scale = fps > 30 && dt > 1.0 / 30.0 - 1e-4 && dt < 1.0 / 30.0 + 1e-4;
    if (scale)
        ctx.f1.f64 = 1.0 / fps;
    // Diagnostics: the first steps, every change, and one a minute.
    int state = scale ? fps : 30;
    if (calls++ < 3 || state != lastHalved || calls % 3600 == 0)
        wet::Log("World update: step %.6f s%s", ctx.f1.f64, scale ? " (scaled for the frame rate option)" : "");
    lastHalved = state;
    __imp__sub_82C32BF8(ctx, base);
}
