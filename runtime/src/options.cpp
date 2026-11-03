#include "options.h"
#include "photo.h"
#include "keybinds.h"
#include "gpu_info.h"
#include "log.h"
#include "gpu/renderer.h"
#include "memory.h"
#include "os.h"
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
        // F6 cycles through these; 0 = the monitor's refresh rate.
        constexpr int kRates[] = { 30, 60, 120, 144, 165, 240, 0 };
        constexpr int kMaxFps = 360;
        std::atomic<int> s_monitorHz{ 0 };   // read once, when "match monitor" is first used
        std::atomic<int> s_fps{ 30 };
        std::atomic<int> s_fpsOverride{ 0 };   // video mode: temporary 30 FPS (not saved)
        std::atomic<int> s_cutsceneFps{ 1 };   // in-game cutscenes at 30 FPS
        std::atomic<bool> s_inCutscene{ false };
        std::atomic<int> s_videoMode{ 1 };
        int s_scale = 1, s_scaleRunning = 1;
        std::atomic<int> s_aa{ 0 }, s_filter{ 0 }, s_fullscreen{ 0 }, s_showFps{ 0 }, s_sharpness{ 50 }, s_filmGrain{ 100 }, s_fov{ 90 }, s_widescreen{ 0 }, s_widescreenHud{ 1 }, s_hideBorders{ 0 }, s_vsync{ 0 }, s_buttonPrompts{ 0 }, s_mouseAim{ 1 }, s_mouseSens{ 50 }, s_mouseInvert{ 0 }, s_mouseDirect{ 1 }, s_mouseNative{ 1 }, s_skipIntro{ 1 }, s_musicMode{ 0 }, s_musicVolume{ 80 }, s_musicShuffle{ 1 }, s_outfit{ 0 }, s_aniso{ 1 }, s_autoBackup{ 1 }, s_shaderCache{ 1 }, s_asyncShaders{ 1 }, s_shaderPreload{ 0 }, s_adaptiveWatch{ 1 }, s_gpuWait{ 1 }, s_vibration{ 1 }, s_achievementPopups{ 1 }, s_motionBlur{ 1 }, s_depthOfField{ 1 }, s_cameraShake{ 1 }, s_language{ 0 };
        constexpr const char* kFile = "wet.ini";
        // Mouse button bindings: left, right, middle, back (X1), forward (X2).
        constexpr const char* kMouseKeys[5] = { "mouse_left", "mouse_right", "mouse_middle", "mouse_back", "mouse_forward" };
        std::string s_mouseBind[5] = { "RT", "LT", "RTHUMB", "LB", "RB" };
        std::mutex s_monitorLock;
        std::string s_monitor;   // display device name (\\.\DISPLAY2), empty = the main display
        std::string s_musicFolder;   // your music (empty = "music" next to the game)

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

        // Any rate from 30 to 360, or 0 (match the monitor).
        int Valid(int fps)
        {
            if (fps == 0)
                return 0;
            return Clamp(fps, 30, kMaxFps);
        }

        int MonitorHz()
        {
            int hz = s_monitorHz.load(std::memory_order_relaxed);
            if (!hz)
            {
                hz = Clamp(os::DisplayRefreshRate(), 30, kMaxFps);
                s_monitorHz = hz;
            }
            return hz;
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
            std::fprintf(f, "# fps: 30 (the game's own) up to 360, or 0 = match the monitor's refresh rate - F6 cycles in game\n");
            std::fprintf(f, "fps = %d\n", s_fps.load());
            std::fprintf(f, "# resolution_scale: 1 = 720p (the game's own), 2 = 1440p, 3 = 2160p (4K); needs a restart\n");
            std::fprintf(f, "resolution_scale = %d\n", s_scale);
            std::fprintf(f, "# antialiasing: 0 = off, 1 = FXAA, 2 = FXAA extreme, 3 = SMAA (sharper, best with Direct3D 12)\n");
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
            std::fprintf(f, "# cutscene_30fps: 1 = in-game cutscenes run at 30 FPS, as on the console (keeps speech in sync)\n");
            std::fprintf(f, "cutscene_30fps = %d\n", s_cutsceneFps.load());
            std::fprintf(f, "# film_grain: the game's film grain, 0 (off) to 100 (as designed)\n");
            std::fprintf(f, "film_grain = %d\n", s_filmGrain.load());
            std::fprintf(f, "# fov: horizontal field of view in degrees at 16:9, 60-130 (90 = the game's own)\n");
            std::fprintf(f, "fov = %d\n", s_fov.load());
            std::fprintf(f, "# widescreen: 1 = fill wider screens and windows (21:9, 32:9) instead of 16:9 with bars\n");
            std::fprintf(f, "widescreen = %d\n", s_widescreen.load());
            std::fprintf(f, "# widescreen_hud: 1 = keep the HUD and menus 16:9 in the middle of a wider screen, 0 = stretch them\n");
            std::fprintf(f, "widescreen_hud = %d\n", s_widescreenHud.load());
            std::fprintf(f, "# hide_borders: 1 = don't draw the dark frame at the screen edges (vignette / cinematic borders)\n");
            std::fprintf(f, "hide_borders = %d\n", s_hideBorders.load());
            std::fprintf(f, "# vsync: 1 = never tear (frames wait for the screen's refresh); 0 = present at once (best with G-Sync / FreeSync); restart\n");
            std::fprintf(f, "vsync = %d\n", s_vsync.load());
            std::fprintf(f, "# button_prompts: 0 = automatic (keyboard / mouse with no controller at startup, PlayStation symbols with a PlayStation controller, else Xbox), 1 = Xbox, 2 = PlayStation, 3 = keyboard / mouse\n");
            std::fprintf(f, "button_prompts = %d\n", s_buttonPrompts.load());
            std::fprintf(f, "# mouse_aim: 1 = the mouse turns the camera / aims (right stick) while the game window is active\n");
            std::fprintf(f, "mouse_aim = %d\n", s_mouseAim.load());
            std::fprintf(f, "# mouse_sensitivity: 1-200; mouse_invert_y: 1 = up/down inverted\n");
            std::fprintf(f, "mouse_sensitivity = %d\n", s_mouseSens.load());
            std::fprintf(f, "mouse_invert_y = %d\n", s_mouseInvert.load());
            std::fprintf(f, "# mouse_direct: 1 = the mouse turns the camera by how far it moves (no stick speed limit); 0 = the mouse acts as a stick\n");
            std::fprintf(f, "mouse_direct = %d\n", s_mouseDirect.load());
            std::fprintf(f, "# mouse_native: 1 = the mouse turns the third-person camera directly (native mouse look); 0 = through the right stick\n");
            std::fprintf(f, "mouse_native = %d\n", s_mouseNative.load());
            std::fprintf(f, "# outfit_colour: Rubi's top and trousers - 0 original, 1 red, 2 blue, 3 black, 4 white, 5 green, 6 purple, 7 pink\n");
            std::fprintf(f, "outfit_colour = %d\n", s_outfit.load());
            std::fprintf(f, "# skip_intro: 1 = straight to the title screen (no logo / legal movies at startup)\n");
            std::fprintf(f, "skip_intro = %d\n", s_skipIntro.load());
            std::fprintf(f, "# anisotropic_filtering: 1 = 16x (sharper floors and walls at an angle), 0 = as the game sets it\n");
            std::fprintf(f, "anisotropic_filtering = %d\n", s_aniso.load());
            std::fprintf(f, "# save_backups: 1 = copy the saves to game\\hdd\\save-backups at each start (the last 10 are kept)\n");
            std::fprintf(f, "save_backups = %d\n", s_autoBackup.load());
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
            std::fprintf(f, "# motion_blur, depth_of_field, camera_shake: the game's effects (1 = on, as designed; 0 = off, also a little faster)\n");
            std::fprintf(f, "motion_blur = %d\ndepth_of_field = %d\ncamera_shake = %d\n", s_motionBlur.load(), s_depthOfField.load(),
                         s_cameraShake.load());
            std::fprintf(f, "# language: 0 = Windows' language if the game has it, else English; 1 = English, 3 = German, 4 = French, 5 = Spanish, 6 = Italian (text and speech; restart)\n");
            std::fprintf(f, "language = %d\n", s_language.load());
            std::fprintf(f, "# shader_preload: 1 = build every cached shader before the first frame (slower start, no pop-in); 0 = build them in the background while the game starts\n");
            std::fprintf(f, "shader_preload = %d\n", s_shaderPreload.load());
            {
                std::lock_guard lock(s_monitorLock);
                std::fprintf(f, "# monitor: the display the game opens on (as the launcher lists it, e.g. \\\\.\\DISPLAY2); empty = the main display\n");
                std::fprintf(f, "monitor = %s\n", s_monitor.c_str());
            }
            {
                std::lock_guard lock(s_monitorLock);
                std::fprintf(f, "# music_mode: your own music instead of the game's - 0 = off, 1 = a song when the game's music plays, 2 = all the time\n");
                std::fprintf(f, "music_mode = %d\n", s_musicMode.load());
                std::fprintf(f, "# music_folder: where your songs are (MP3, M4A/AAC, FLAC, WAV, WMA); empty = the music folder next to the game\n");
                std::fprintf(f, "music_folder = %s\n", s_musicFolder.c_str());
                std::fprintf(f, "music_volume = %d\nmusic_shuffle = %d\n", s_musicVolume.load(), s_musicShuffle.load());
            }
            std::fprintf(f, "fullscreen = %d\n", s_fullscreen.load());
            std::fprintf(f, "show_fps = %d\n", s_showFps.load());
            std::fclose(f);
        }
    }

    void Load()
    {
        FILE* f = std::fopen(kFile, "r");
        if (!f)
        {
            // First start (no wet.ini): resolution and anti-aliasing to suit the
            // graphics card instead of 720p for everyone.
            gpu_info::Info gpu = gpu_info::Query();
            gpu_info::Recommended r = gpu_info::Recommend(gpu);
            s_scale = r.scale;
            s_aa = r.aa;
            s_filter = r.filter;
            if (gpu.found)
                Log("Options: first start - %s with %llu MB is %s: resolution scale %d, antialiasing %d, filter %d",
                    gpu.name.c_str(), (unsigned long long)gpu.dedicatedMB, r.tier, r.scale, r.aa, r.filter);
        }
        if (f)
        {
            char line[1024];
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
                if (const char* m = std::strstr(line, "music_folder"); m && std::strspn(line, " \t") == size_t(m - line))
                {
                    const char* eq = std::strchr(m, '=');
                    std::string v = eq ? eq + 1 : "";
                    size_t x = v.find_first_not_of(" \t"), y = v.find_last_not_of(" \t\r\n");
                    v = x == std::string::npos ? std::string() : v.substr(x, y - x + 1);
                    std::lock_guard lock(s_monitorLock);
                    s_musicFolder = v;
                    continue;
                }
                if (const char* m = std::strstr(line, "monitor"); m && std::strspn(line, " \t") == size_t(m - line) && (m[7] == ' ' || m[7] == '='))
                {
                    const char* eq = std::strchr(m, '=');
                    std::string v = eq ? eq + 1 : "";
                    size_t x = v.find_first_not_of(" \t"), y = v.find_last_not_of(" \t\r\n");
                    v = x == std::string::npos ? std::string() : v.substr(x, y - x + 1);
                    std::lock_guard lock(s_monitorLock);
                    s_monitor = v.find("DISPLAY") != std::string::npos ? v : std::string();
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
                else if (!std::strcmp(key, "antialiasing")) s_aa = Clamp(value, 0, 3);
                else if (!std::strcmp(key, "filter")) s_filter = Clamp(value, 0, 3);
                else if (!std::strcmp(key, "achievement_popups")) s_achievementPopups = value != 0;
                else if (!std::strcmp(key, "video_mode")) s_videoMode = value != 0;
                else if (!std::strcmp(key, "cutscene_30fps")) s_cutsceneFps = value != 0;
                else if (!std::strcmp(key, "async_shaders")) s_asyncShaders = value != 0;
                else if (!std::strcmp(key, "shader_preload")) s_shaderPreload = value != 0;
                else if (!std::strcmp(key, "motion_blur")) s_motionBlur = value != 0;
                else if (!std::strcmp(key, "language")) s_language = (value == 1 || (value >= 3 && value <= 6)) ? value : 0;
                else if (!std::strcmp(key, "depth_of_field")) s_depthOfField = value != 0;
                else if (!std::strcmp(key, "camera_shake")) s_cameraShake = value != 0;
                else if (!std::strcmp(key, "shader_cache")) s_shaderCache = value != 0;
                else if (!std::strcmp(key, "mouse_aim")) s_mouseAim = value != 0;
                else if (!std::strcmp(key, "mouse_sensitivity")) s_mouseSens = Clamp(value, 1, 200);
                else if (!std::strcmp(key, "mouse_invert_y")) s_mouseInvert = value != 0;
                else if (!std::strcmp(key, "mouse_direct")) s_mouseDirect = value != 0;
                else if (!std::strcmp(key, "mouse_native")) s_mouseNative = value != 0;
                else if (!std::strcmp(key, "skip_intro")) s_skipIntro = value != 0;
                else if (!std::strcmp(key, "music_mode")) s_musicMode = Clamp(value, 0, 2);
                else if (!std::strcmp(key, "outfit_colour")) s_outfit = Clamp(value, 0, 7);
                else if (!std::strcmp(key, "music_volume")) s_musicVolume = Clamp(value, 0, 100);
                else if (!std::strcmp(key, "music_shuffle")) s_musicShuffle = value != 0;
                else if (!std::strcmp(key, "anisotropic_filtering")) s_aniso = value != 0;
                else if (!std::strcmp(key, "save_backups")) s_autoBackup = value != 0;
                else if (!std::strcmp(key, "film_grain")) s_filmGrain = Clamp(value, 0, 100);
                else if (!std::strcmp(key, "fov")) s_fov = Clamp(value, 60, 130);
                else if (!std::strcmp(key, "widescreen")) s_widescreen = value != 0;
                else if (!std::strcmp(key, "widescreen_hud")) s_widescreenHud = value != 0;
                else if (!std::strcmp(key, "hide_borders")) s_hideBorders = value != 0;
                else if (!std::strcmp(key, "vsync")) s_vsync = value != 0;
                else if (!std::strcmp(key, "button_prompts")) s_buttonPrompts = Clamp(value, 0, 3);
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

    int Fps()
    {
        int o = s_fpsOverride.load(std::memory_order_relaxed);
        if (o)
            return o;
        if (s_inCutscene.load(std::memory_order_relaxed) && s_cutsceneFps.load(std::memory_order_relaxed))
            return 30;
        int f = s_fps.load(std::memory_order_relaxed);
        return f ? f : MonitorHz();
    }
    int MonitorRefreshRate() { return MonitorHz(); }
    int ConfiguredFps() { return s_fps; }
    namespace { int CurrentFps() { return Fps(); } }
    bool VideoMode() { return s_videoMode != 0; }
    bool CutsceneFps30() { return s_cutsceneFps != 0; }
    void SetCutsceneFps30(bool v)
    {
        s_cutsceneFps = v;
        ApplyFps();
        Save();
    }
    void SetInCutscene(bool v)
    {
        if (s_inCutscene.exchange(v) == v)
            return;
        ApplyFps();
        if (s_cutsceneFps.load())
            Log(v ? "Cutscene: 30 FPS until it ends" : "Cutscene over: back to %d FPS", Fps());
    }
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
        if (s_fps.load())
            Log("Options: %d FPS", s_fps.load());
        else
            Log("Options: %d FPS (the monitor's refresh rate)", MonitorHz());
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
    void SetAntialiasing(int v) { s_aa = Clamp(v, 0, 3); Save(); }
    int Filter() { return s_filter; }
    void SetFilter(int v) { s_filter = Clamp(v, 0, 3); Save(); }
    int FilmGrain() { return s_filmGrain; }
    bool AsyncShaders() { return s_asyncShaders != 0; }
    bool ShaderPreload() { return s_shaderPreload != 0; }
    void SetAsyncShaders(bool v) { s_asyncShaders = v; Save(); }
    bool ShaderCache() { return s_shaderCache != 0; }
    bool MouseAim() { return s_mouseAim.load(std::memory_order_relaxed) != 0; }
    void SetMouseAim(bool v) { s_mouseAim = v; Save(); }
    int MouseSensitivity() { return s_mouseSens.load(std::memory_order_relaxed); }
    void SetMouseSensitivity(int v) { s_mouseSens = Clamp(v, 1, 200); Save(); }
    bool MouseInvertY() { return s_mouseInvert.load(std::memory_order_relaxed) != 0; }
    void SetMouseInvertY(bool v) { s_mouseInvert = v; Save(); }
    bool MouseDirect() { return s_mouseDirect.load(std::memory_order_relaxed) != 0; }
    void SetMouseDirect(bool v) { s_mouseDirect = v; Save(); }
    bool MouseNative() { return s_mouseNative.load(std::memory_order_relaxed) != 0; }
    bool SkipIntro()
    {
        // The soak test's recordings and script expect the movies (WET_NO_SKIP_INTRO).
        static const bool forcedOff = std::getenv("WET_NO_SKIP_INTRO") != nullptr;
        return !forcedOff && s_skipIntro.load(std::memory_order_relaxed) != 0;
    }
    void SetSkipIntro(bool v) { s_skipIntro = v; Save(); }
    int OutfitColour() { return s_outfit.load(std::memory_order_relaxed); }
    void SetOutfitColour(int v) { s_outfit = Clamp(v, 0, 7); Save(); }
    int MusicMode() { return s_musicMode.load(std::memory_order_relaxed); }
    void SetMusicMode(int v) { s_musicMode = Clamp(v, 0, 2); Save(); }
    int MusicVolume() { return s_musicVolume.load(std::memory_order_relaxed); }
    void SetMusicVolume(int v) { s_musicVolume = Clamp(v, 0, 100); Save(); }
    bool MusicShuffle() { return s_musicShuffle.load(std::memory_order_relaxed) != 0; }
    void SetMusicShuffle(bool v) { s_musicShuffle = v; Save(); }
    std::string MusicFolder()
    {
        std::lock_guard lock(s_monitorLock);
        return s_musicFolder;
    }
    void SetMusicFolder(const std::string& v)
    {
        {
            std::lock_guard lock(s_monitorLock);
            s_musicFolder = v;
        }
        Save();
    }
    bool AnisotropicFiltering() { return s_aniso.load(std::memory_order_relaxed) != 0; }
    void SetAnisotropicFiltering(bool v) { s_aniso = v; Save(); }
    bool SaveBackups() { return s_autoBackup.load(std::memory_order_relaxed) != 0; }
    void SetSaveBackups(bool v) { s_autoBackup = v; Save(); }
    void SetMouseNative(bool v) { s_mouseNative = v; Save(); }
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
    void SetMouseBinding(int button, const std::string& pad)
    {
        if (button < 0 || button >= 5)
            return;
        s_mouseBind[button] = pad.empty() ? "NONE" : pad;
        Save();
    }
    void SetFilmGrain(int v) { s_filmGrain = Clamp(v, 0, 100); Save(); }
    int Fov() { return s_fov.load(std::memory_order_relaxed); }
    bool Widescreen() { return s_widescreen.load(std::memory_order_relaxed) != 0; }
    void SetWidescreen(bool v) { s_widescreen = v; Save(); }
    bool WidescreenHud() { return s_widescreenHud.load(std::memory_order_relaxed) != 0; }
    void SetWidescreenHud(bool v) { s_widescreenHud = v; Save(); }
    bool HideBorders() { return s_hideBorders.load(std::memory_order_relaxed) != 0; }
    bool Vsync() { return s_vsync.load(std::memory_order_relaxed) != 0; }
    int ButtonPrompts() { return s_buttonPrompts.load(std::memory_order_relaxed); }
    void SetButtonPrompts(int v) { s_buttonPrompts = Clamp(v, 0, 3); Save(); }
    void SetVsync(bool v) { s_vsync = v; Save(); }
    void SetHideBorders(bool v) { s_hideBorders = v; Save(); }
    int Language()
    {
        int v = s_language.load(std::memory_order_relaxed);
        if (v)
            return v;
        // Automatic: the Windows display language, when the game has it.
        int windows = os::UserLanguageId();   // primary language id (LANG_*)
        switch (windows)
        {
        case 0x07: return 3;   // German
        case 0x0C: return 4;   // French
        case 0x0A: return 5;   // Spanish
        case 0x10: return 6;   // Italian
        default: return 1;     // English
        }
    }
    int LanguageSetting() { return s_language.load(std::memory_order_relaxed); }
    void SetLanguage(int v) { s_language = (v == 1 || (v >= 3 && v <= 6)) ? v : 0; Save(); }
    bool MotionBlur() { return s_motionBlur.load(std::memory_order_relaxed) != 0; }
    void SetMotionBlur(bool v) { s_motionBlur = v; Save(); }
    bool DepthOfField() { return s_depthOfField.load(std::memory_order_relaxed) != 0; }
    void SetDepthOfField(bool v) { s_depthOfField = v; Save(); }
    bool CameraShake() { return s_cameraShake.load(std::memory_order_relaxed) != 0; }
    void SetCameraShake(bool v) { s_cameraShake = v; Save(); }
    void SetFov(int v) { s_fov = Clamp(v, 60, 130); Save(); }
    int Sharpness() { return s_sharpness; }
    void SetSharpness(int v) { s_sharpness = Clamp(v, 0, 100); Save(); }
    std::string Monitor()
    {
        std::lock_guard lock(s_monitorLock);
        return s_monitor;
    }
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

// Effect switches (config/WET_config.toml, after the original instruction).
// Motion blur: the patch loads the strength from a field that is zero instead.
void WetMotionBlurHook(PPCRegister& r9, PPCRegister& f31)
{
    if (!wet::options::MotionBlur())
    {
        uint8_t* base = wet::g_mem.base;
        uint32_t bits = PPC_LOAD_U32(r9.u32 + 10912);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        f31.f64 = double(value);
    }
}

void WetDepthOfFieldHook(PPCRegister& r11)
{
    if (!wet::options::DepthOfField())
        r11.u64 = 0;
}

void WetCameraShakeHook(PPCRegister& r3, PPCRegister& r10)
{
    if (!wet::options::CameraShake())
    {
        uint8_t* base = wet::g_mem.base;
        PPC_STORE_U8(r3.u32 + 21, r10.u8);
    }
}

void WetFilmShakeHook(PPCRegister& r10)
{
    if (!wet::options::CameraShake())
        r10.u64 = 0;
}

// The game world's per-frame update (A2M world, f1 = time step in seconds; it
// stores it at +196 for everything else). The game steps 1/30 s every frame, so
// at higher frame rates everything sped up: the step becomes 1/fps.
extern "C" void __imp__sub_82C32BF8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_828D5AE0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82C32BF8)
{
    // GameStateUtil::IsCutsceneActive (sub_828D5AE0): the top game state is
    // GSCutscene. Real-time cutscenes are timed for 30 FPS: their speech
    // drifts out of sync at higher frame rates, so they run at 30 (as videos do).
    if (wet::options::CutsceneFps30())
    {
        PPCContext saved = ctx;
        __imp__sub_828D5AE0(ctx, base);
        bool active = ctx.r3.u32 != 0;
        ctx = saved;
        wet::options::SetInCutscene(active);
    }
    static uint64_t calls = 0;
    static int lastHalved = -1;
    // Photo mode (F5): starts / stops here on the game thread and freezes time.
    if (double step = wet::photo::GameTick(ctx, base, ctx.f1.f64); wet::photo::Active())
    {
        ctx.f1.f64 = step;
        __imp__sub_82C32BF8(ctx, base);
        return;
    }
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
