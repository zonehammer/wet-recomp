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
    int ConfiguredFps();      // as set: 30-360, or 0 = match the monitor
    int MonitorRefreshRate();    // the player's setting
    // Video mode (fs.cpp opens/closes .bik files): temporary FPS, 0 = none.
    void SetFpsOverride(int fps);
    bool VideoMode();
    // In-game cutscenes at 30 FPS (cutscene_30fps); the world update reports whether one is running.
    bool CutsceneFps30();
    void SetCutsceneFps30(bool v);
    void SetInCutscene(bool v);
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
    // Build every cached shader before the first frame instead of in the
    // background while the game starts (wet.ini shader_preload; restart).
    bool ShaderPreload();
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
    // Native mouse look (mouselook.cpp): the mouse turns the third-person camera directly.
    bool MouseNative();
    // Rubi's outfit colour, her top and trousers (outfit.h): 0 original, 1 red, 2 blue, 3 black, 4 white, 5 green, 6 purple, 7 pink.
    int OutfitColour();
    void SetOutfitColour(int v);
    // Your own music (music.cpp): mode 0 off, 1 following the game's music, 2 always.
    int MusicMode();
    void SetMusicMode(int v);
    int MusicVolume();          // 0-100
    void SetMusicVolume(int v);
    bool MusicShuffle();
    void SetMusicShuffle(bool v);
    std::string MusicFolder();  // empty = "music" next to the game
    void SetMusicFolder(const std::string& v);
    // Straight to the title screen: no logo / legal movies (kernel/fs.cpp).
    bool SkipIntro();
    void SetSkipIntro(bool v);
    // 16x anisotropic filtering for everything the game samples (Xenia's override).
    bool AnisotropicFiltering();
    void SetAnisotropicFiltering(bool v);
    // A copy of the saves at each start (main.cpp), the last 10 kept.
    bool SaveBackups();
    void SetSaveBackups(bool v);
    void SetMouseNative(bool v);
    std::string MouseBinding(int button);
    // button 0-4: left, right, middle, back, forward; pad: A B X Y LB RB LT RT LTHUMB RTHUMB START BACK UP DOWN LEFT RIGHT or NONE.
    void SetMouseBinding(int button, const std::string& pad);
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
    // Widescreen: keep the HUD 16:9 in the middle (else it stretches).
    bool WidescreenHud();
    void SetWidescreenHud(bool v);
    // Don't draw the dark frame at the screen edges (vignette / cinematic borders).
    bool HideBorders();
    // No tearing: presentation never splits a frame (off = immediate, for VRR). Restart.
    bool Vsync();
    // Button prompts: 0 = automatic (PlayStation with a PlayStation pad), 1 = Xbox, 2 = PlayStation.
    int ButtonPrompts();
    void SetButtonPrompts(int v);
    void SetVsync(bool v);
    void SetHideBorders(bool v);
    // The game's motion blur, depth of field and camera shake (with its film
    // shake), on as designed (wet.ini motion_blur / depth_of_field / camera_shake).
    // The game's language (Xbox language id: 1 English, 3 German, 4 French,
    // 5 Spanish, 6 Italian - the five on the disc); wet.ini language, 0 = the
    // Windows language when it is one of those, else English. Restart.
    int Language();
    int LanguageSetting();
    void SetLanguage(int v);
    bool MotionBlur();
    void SetMotionBlur(bool v);
    bool DepthOfField();
    void SetDepthOfField(bool v);
    bool CameraShake();
    void SetCameraShake(bool v);
    int Sharpness();
    void SetSharpness(int v);

    // The display to open on: its device name (\\.\DISPLAY2), empty = the main one.
    std::string Monitor();
    bool Fullscreen();
    void SetFullscreen(bool v);
    bool ShowFps();
    void SetShowFps(bool v);
}
