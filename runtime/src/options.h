// Player options, kept in wet.ini next to the logs (key = value lines).
// Changed in game with the F1 settings overlay (gpu/xenia_bridge.cpp) or hotkeys.
#pragma once
#include <string>

namespace wet::options
{
    void Load();
    void Save();

    // Frame rate: 30 (the game's own), 60, 120 or 144. F6 cycles through them.
    int Fps();              // in effect (30 while video mode holds it)
    int ConfiguredFps();    // the player's setting
    // Video mode (fs.cpp opens/closes .bik files): temporary FPS, 0 = none.
    void SetFpsOverride(int fps);
    bool VideoMode();
    void SetVideoMode(bool v);
    void SetFps(int fps);
    void CycleFps();

    // Rendering resolution scale: 1 (720p, the game's own), 2 (1440p), 3 (2160p / 4K).
    // Takes effect at the next start; RunningResolutionScale is the current one.
    int ResolutionScale();
    int RunningResolutionScale();
    void SetResolutionScale(int scale);

    // Post-processing (Xenia's presenter): 0 off / 1 FXAA / 2 FXAA extreme, and
    // the scaling filter 0 bilinear / 1 AMD CAS / 2 AMD FSR.
    int Antialiasing();
    void SetAntialiasing(int v);
    int Filter();
    void SetFilter(int v);
    // NVIDIA Image Scaling sharpness, 0-100.
    // Adaptive write watching (memory.cpp), wet.ini adaptive_watch.
    // Render thread sleeps instead of spinning on GPU fences (gpu_wait.cpp).
    // Controller rumble (wet.ini vibration).
    // "Achievement unlocked" pop-ups (wet.ini achievement_popups).
    bool AchievementPopups();
    void SetAchievementPopups(bool v);
    bool Vibration();
    void SetVibration(bool v);
    bool GpuWait();
    void SetGpuWait(bool v);
    bool AdaptiveWatch();
    void SetAdaptiveWatchOption(bool v);
    // The game's film grain, 0-100 % (wet.ini film_grain).
    // Mouse aim (input.cpp): the mouse drives the right stick; buttons are bound
    // to pad buttons by name (0 left, 1 right, 2 middle, 3 back, 4 forward).
    // Shader / pipeline cache in <hdd>/shaders (wet.ini shader_cache; restart).
    bool ShaderCache();
    // Skip draws whose shader is still compiling instead of waiting (wet.ini async_shaders).
    bool AsyncShaders();
    void SetAsyncShaders(bool v);
    bool MouseAim();
    void SetMouseAim(bool v);
    int MouseSensitivity();
    void SetMouseSensitivity(int v);
    bool MouseInvertY();
    void SetMouseInvertY(bool v);
    // Direct mouse aim (padaim.cpp): mouse movement goes straight into the game's
    // right-stick value each frame, without the stick's -1..1 limit.
    bool MouseDirect();
    void SetMouseDirect(bool v);
    std::string MouseBinding(int button);
    // Keyboard bindings (keybinds.h actions): "KEY" or "KEY, KEY" or "NONE".
    std::string KeyBinding(int action);
    void SetKeyBinding(int action, const std::string& keys);
    int KeyBindingsVersion();   // changes whenever a binding does
    int FilmGrain();
    void SetFilmGrain(int v);
    // Horizontal field of view at 16:9 in degrees (90 = the game's own; fov.cpp).
    int Fov();
    void SetFov(int v);
    // Fill screens and windows wider than 16:9 (Hor+: the view widens).
    bool Widescreen();
    void SetWidescreen(bool v);
    int Sharpness();
    void SetSharpness(int v);

    bool Fullscreen();
    void SetFullscreen(bool v);
    bool ShowFps();
    void SetShowFps(bool v);
}
