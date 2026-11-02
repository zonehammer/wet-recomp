// Photo mode (F5, or L3 + R3 on a controller): the game freezes, the HUD hides
// and a free camera flies anywhere; screenshots go to screenshots\. See photo.cpp.
#pragma once
#include <cstdint>
#include <string>

struct PPCContext;

namespace wet
{
    struct HostPadState;
}

namespace wet::photo
{
    // Photo mode is on: the game's input is held at rest and its time frozen.
    bool Active();
    // F5 / L3 + R3 (any thread): turns it on (in third-person play) or off.
    void RequestToggle();

    // While active, input.cpp hands over the controller (keyboard and mouse
    // buttons mapped in) and the mouse movement instead of giving them to the game.
    void NotePad(const HostPadState& pad);
    void AddMouse(double dx, double dy);

    // The third-person camera's input gather ran (mouselook.cpp): photo mode can start.
    void NoteThirdPersonCamera();
    // The pause menu is open (kernel/gpu.cpp): photo mode can't start.
    void SetPauseMenu(bool open);

    // The game world's per-frame update (options.cpp), on the game thread: starts
    // and stops photo mode, hides / shows the HUD. Returns the time step to use.
    double GameTick(PPCContext& ctx, uint8_t* base, double step);

    // The game's "set camera" (fov.cpp) with the main view matrix at `view`: while
    // active, writes the free camera's view to `out` and returns true.
    bool OverrideView(uint8_t* base, uint32_t view, uint32_t proj, uint32_t out);
    // Horizontal field of view to use while active (degrees at 16:9).
    double Fov();

    // For the overlay (UI thread).
    struct Status
    {
        bool active = false;
        bool hudHidden = false;
        bool hudAvailable = false;
        double fov = 90;
        bool fast = false;
    };
    Status GetStatus();
    // A short message for the overlay ("Screenshot saved ...", "Photo mode only
    // works in third-person play"); empty when there's none.
    std::string Toast();
    void SetToast(const std::string& text);
}
