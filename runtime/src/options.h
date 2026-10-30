// Player options, kept in wet.ini next to the logs (key = value lines).
// Changed in game with the F1 settings overlay (gpu/xenia_bridge.cpp) or hotkeys.
#pragma once

namespace wet::options
{
    void Load();
    void Save();

    // Frame rate: 30 (the game's own), 60, 120 or 144. F6 cycles through them.
    int Fps();
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
    bool GpuWait();
    void SetGpuWait(bool v);
    bool AdaptiveWatch();
    void SetAdaptiveWatchOption(bool v);
    int Sharpness();
    void SetSharpness(int v);

    bool Fullscreen();
    void SetFullscreen(bool v);
    bool ShowFps();
    void SetShowFps(bool v);
}
