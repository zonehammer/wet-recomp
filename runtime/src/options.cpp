#include "options.h"
#include "keybinds.h"
#include "log.h"
#include "gpu/renderer.h"
#include "memory.h"
#include "ppc_context.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace wet::options
{
    namespace
    {
        constexpr int kRates[] = { 30, 60, 120, 144 };
        std::atomic<int> s_fps{ 30 };
        std::atomic<int> s_fpsOverride{ 0 };   // video mode: temporary 30 FPS (not saved)
        std::atomic<int> s_videoMode{ 1 };
        int s_scale = 1, s_scaleRunning = 1;
        std::atomic<int> s_aa{ 0 }, s_filter{ 0 }, s_fullscreen{ 0 }, s_showFps{ 0 }, s_sharpness{ 50 }, s_filmGrain{ 100 }, s_mouseAim{ 1 }, s_mouseSens{ 50 }, s_mouseInvert{ 0 }, s_mouseDirect{ 1 }, s_shaderCache{ 1 }, s_asyncShaders{ 1 }, s_adaptiveWatch{ 1 }, s_gpuWait{ 1 }, s_vibration{ 1 }, s_achievementPopups{ 1 };
        constexpr const char* kFile = "wet.ini";
        // Mouse button bindings: left, right, middle, back (X1), forward (X2).
        constexpr const char* kMouseKeys[5] = { "mouse_left", "mouse_right", "mouse_middle", "mouse_back", "mouse_forward" };
        std::string s_mouseBind[5] = { "RT", "LT", "RTHUMB", "LB", "RB" };

        // Keyboard bindings (keybinds.h), key_<action> = KEY[, KEY].
        std::mutex s_keyLock;
        std::string s_keyBind[keybinds::kActionCount];
        [[maybe_unused]] bool s_keyBindLoaded = [] {
            for (int i = 0; i < keybinds::kActionCount; i++)
                s_keyBind[i] = keybinds::kActions[i].fallback;
            return true;
        }();
        std::atomic<int> s_keyVersion{ 0 };

        std::string Upper(std::string s)
        {
            for (auto& c : s)
                c = char(std::toupper((unsigned char)c));
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }

        int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

        int Valid(int fps)
        {
            for (int r : kRates)
                if (fps == r)
                    return r;
            return fps >= 60 ? 60 : 30;
        }

        int CurrentFps();
        void ApplyFps()
        {
            // Above 60 the guest's vblank runs faster too (one present per vblank).
            int fps = CurrentFps();
            renderer::SetVsyncRate(fps > 60 ? uint32_t(fps) : 60u);
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
            std::fprintf(f, "# filter (scaling to the window): 0 = bilinear, 1 = AMD CAS sharpening, 2 = AMD FSR, 3 = NVIDIA Image Scaling (D3D12)\n");
            std::fprintf(f, "filter = %d\n", s_filter.load());
            std::fprintf(f, "# sharpness (NVIDIA Image Scaling): 0 to 100\n");
            std::fprintf(f, "sharpness = %d\n", s_sharpness.load());
            std::fprintf(f, "# adaptive_watch: 1 = skip write traps on memory the game rewrites every frame (faster), 0 = trap every write\n");
            std::fprintf(f, "adaptive_watch = %d\n", s_adaptiveWatch.load());
            std::fprintf(f, "# gpu_wait: 1 = the render thread sleeps while waiting for the GPU (less CPU), 0 = spins like the console\n");
            std::fprintf(f, "gpu_wait = %d\n", s_gpuWait.load());
            std::fprintf(f, "# vibration: controller rumble on (1) or off (0)\n");
            std::fprintf(f, "vibration = %d\n", s_vibration.load());
            std::fprintf(f, "# achievement_popups: show \"Achievement unlocked\" pop-ups (1) or not (0)\n");
            std::fprintf(f, "achievement_popups = %d\n", s_achievementPopups.load());
            std::fprintf(f, "# video_mode: 1 = while a video plays, run at 30 FPS with the slower thread wake-ups (avoids video flicker)\n");
            std::fprintf(f, "video_mode = %d\n", s_videoMode.load());
            std::fprintf(f, "# film_grain: the game's film grain, 0 (off) to 100 (as designed)\n");
            std::fprintf(f, "film_grain = %d\n", s_filmGrain.load());
            std::fprintf(f, "# mouse_aim: 1 = the mouse turns the camera / aims (right stick) while the game window is active\n");
            std::fprintf(f, "mouse_aim = %d\n", s_mouseAim.load());
            std::fprintf(f, "# mouse_sensitivity: 1-100; mouse_invert_y: 1 = up/down inverted\n");
            std::fprintf(f, "mouse_sensitivity = %d\n", s_mouseSens.load());
            std::fprintf(f, "mouse_invert_y = %d\n", s_mouseInvert.load());
            std::fprintf(f, "# mouse_direct: 1 = the mouse turns the camera by how far it moves (no stick speed limit); 0 = the mouse acts as a stick\n");
            std::fprintf(f, "mouse_direct = %d\n", s_mouseDirect.load());
            std::fprintf(f, "# mouse buttons: A B X Y LB RB LT RT LTHUMB RTHUMB START BACK UP DOWN LEFT RIGHT or NONE\n");
            for (int i = 0; i < 5; i++)
                std::fprintf(f, "%s = %s\n", kMouseKeys[i], s_mouseBind[i].c_str());
            std::fprintf(f, "# keyboard: key_<action> = KEY or KEY, KEY or NONE. Keys: A-Z 0-9 SPACE ENTER ESC BACKSPACE TAB LSHIFT RSHIFT\n"
                            "# LCTRL RCTRL LALT RALT CAPSLOCK UP DOWN LEFT RIGHT INSERT DELETE HOME END PAGEUP PAGEDOWN NUM0-NUM9\n"
                            "# F2-F5 F12 MINUS EQUALS LBRACKET RBRACKET SEMICOLON APOSTROPHE COMMA PERIOD SLASH BACKSLASH GRAVE\n");
            {
                std::lock_guard lock(s_keyLock);
                for (int i = 0; i < keybinds::kActionCount; i++)
                    std::fprintf(f, "key_%s = %s\n", keybinds::kActions[i].id, s_keyBind[i].c_str());
            }
            std::fprintf(f, "# shader_cache: 1 = save compiled shaders and rebuild them at startup (no stutter the second time), 0 = off (restart)\n");
            std::fprintf(f, "shader_cache = %d\n", s_shaderCache.load());
            std::fprintf(f, "# async_shaders: 1 = don't wait for new shaders (an object may appear a frame or two late instead of a stutter), 0 = wait\n");
            std::fprintf(f, "async_shaders = %d\n", s_asyncShaders.load());
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
                char key[64], text[64], rest[192];
                int value;
                if (std::sscanf(line, " key_%63[a-z0-9_] = %191[^\r\n]", key, rest) == 2)
                {
                    for (int i = 0; i < keybinds::kActionCount; i++)
                        if (!std::strcmp(key, keybinds::kActions[i].id))
                        {
                            std::lock_guard lock(s_keyLock);
                            s_keyBind[i] = Upper(rest);
                        }
                    s_keyVersion++;
                    continue;
                }
                if (std::sscanf(line, " %63[a-z0-9_] = %63s", key, text) == 2)
                    for (int i = 0; i < 5; i++)
                        if (!std::strcmp(key, kMouseKeys[i]))
                        {
                            s_mouseBind[i] = text;
                            for (auto& c : s_mouseBind[i])
                                c = char(std::toupper((unsigned char)c));
                        }
                if (std::sscanf(line, " %63[a-z0-9_] = %d", key, &value) != 2)
                    continue;
                if (!std::strcmp(key, "fps")) s_fps = Valid(value);
                else if (!std::strcmp(key, "resolution_scale")) s_scale = Clamp(value, 1, 3);
                else if (!std::strcmp(key, "antialiasing")) s_aa = Clamp(value, 0, 2);
                else if (!std::strcmp(key, "filter")) s_filter = Clamp(value, 0, 3);
                else if (!std::strcmp(key, "achievement_popups")) s_achievementPopups = value != 0;
                else if (!std::strcmp(key, "video_mode")) s_videoMode = value != 0;
                else if (!std::strcmp(key, "async_shaders")) s_asyncShaders = value != 0;
                else if (!std::strcmp(key, "shader_cache")) s_shaderCache = value != 0;
                else if (!std::strcmp(key, "mouse_aim")) s_mouseAim = value != 0;
                else if (!std::strcmp(key, "mouse_sensitivity")) s_mouseSens = Clamp(value, 1, 100);
                else if (!std::strcmp(key, "mouse_invert_y")) s_mouseInvert = value != 0;
                else if (!std::strcmp(key, "mouse_direct")) s_mouseDirect = value != 0;
                else if (!std::strcmp(key, "film_grain")) s_filmGrain = Clamp(value, 0, 100);
                else if (!std::strcmp(key, "vibration")) s_vibration = value != 0;
                else if (!std::strcmp(key, "gpu_wait")) s_gpuWait = value != 0;
                else if (!std::strcmp(key, "adaptive_watch")) s_adaptiveWatch = value != 0;
                else if (!std::strcmp(key, "sharpness")) s_sharpness = Clamp(value, 0, 100);
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
        if (const char* v = std::getenv("WET_GPU_WAIT"))
            s_gpuWait = std::atoi(v) != 0;
        if (const char* v = std::getenv("WET_ADAPTIVE_WATCH"))
            s_adaptiveWatch = std::atoi(v) != 0;
        SetAdaptiveWatch(s_adaptiveWatch != 0);
        Save();
        ApplyFps();
        Log("Options: %d FPS, rendering at %dx%d (resolution_scale %d), antialiasing %d, filter %d; saved in %s",
            s_fps.load(), 1280 * s_scale, 720 * s_scale, s_scale, s_aa.load(), s_filter.load(), kFile);
    }

    int Fps() { int o = s_fpsOverride.load(std::memory_order_relaxed); return o ? o : s_fps.load(std::memory_order_relaxed); }
    int ConfiguredFps() { return s_fps; }
    namespace { int CurrentFps() { return Fps(); } }
    bool VideoMode() { return s_videoMode != 0; }
    void SetVideoMode(bool v) { s_videoMode = v; Save(); }
    void SetFpsOverride(int fps)
    {
        s_fpsOverride = fps;
        ApplyFps();
    }
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
    void SetFilter(int v) { s_filter = Clamp(v, 0, 3); Save(); }
    int FilmGrain() { return s_filmGrain; }
    bool AsyncShaders() { return s_asyncShaders != 0; }
    void SetAsyncShaders(bool v) { s_asyncShaders = v; Save(); }
    bool ShaderCache() { return s_shaderCache != 0; }
    bool MouseAim() { return s_mouseAim.load(std::memory_order_relaxed) != 0; }
    void SetMouseAim(bool v) { s_mouseAim = v; Save(); }
    int MouseSensitivity() { return s_mouseSens.load(std::memory_order_relaxed); }
    void SetMouseSensitivity(int v) { s_mouseSens = Clamp(v, 1, 100); Save(); }
    bool MouseInvertY() { return s_mouseInvert.load(std::memory_order_relaxed) != 0; }
    void SetMouseInvertY(bool v) { s_mouseInvert = v; Save(); }
    bool MouseDirect() { return s_mouseDirect.load(std::memory_order_relaxed) != 0; }
    void SetMouseDirect(bool v) { s_mouseDirect = v; Save(); }
    std::string KeyBinding(int action)
    {
        if (action < 0 || action >= keybinds::kActionCount)
            return {};
        std::lock_guard lock(s_keyLock);
        return s_keyBind[action];
    }
    void SetKeyBinding(int action, const std::string& keys)
    {
        if (action < 0 || action >= keybinds::kActionCount)
            return;
        {
            std::lock_guard lock(s_keyLock);
            s_keyBind[action] = Upper(keys);
        }
        s_keyVersion++;
        Save();
    }
    int KeyBindingsVersion() { return s_keyVersion.load(std::memory_order_relaxed); }
    std::string MouseBinding(int button) { return button >= 0 && button < 5 ? s_mouseBind[button] : std::string(); }
    void SetFilmGrain(int v) { s_filmGrain = Clamp(v, 0, 100); Save(); }
    int Sharpness() { return s_sharpness; }
    void SetSharpness(int v) { s_sharpness = Clamp(v, 0, 100); Save(); }
    bool Fullscreen() { return s_fullscreen; }
    void SetFullscreen(bool v) { s_fullscreen = v; Save(); }
    bool AchievementPopups() { return s_achievementPopups.load() != 0; }
    void SetAchievementPopups(bool v) { s_achievementPopups = v; Save(); }
    bool Vibration() { return s_vibration.load(std::memory_order_relaxed) != 0; }
    void SetVibration(bool v) { s_vibration = v; Save(); }
    bool GpuWait() { return s_gpuWait.load(std::memory_order_relaxed) != 0; }
    void SetGpuWait(bool v) { s_gpuWait = v; Save(); }
    bool AdaptiveWatch() { return s_adaptiveWatch; }
    void SetAdaptiveWatchOption(bool v) { s_adaptiveWatch = v; SetAdaptiveWatch(v); Save(); }
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
