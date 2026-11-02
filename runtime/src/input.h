// Host controller backend (XInput on Windows; an idle virtual pad elsewhere).
// Kept free of <windows.h> so it never collides with the kernel's status constants.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wet
{
    struct HostPadState
    {
        uint32_t packet = 0;
        uint16_t buttons = 0;       // XINPUT_GAMEPAD_* bit layout (same as the 360)
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    bool HostPadConnected(uint32_t user);
    bool HostPadGetState(uint32_t user, HostPadState& out);
    bool HostPadSetVibration(uint32_t user, uint16_t left, uint16_t right);
    // Names of the controllers in guest slots 0-3 (empty = none), for the overlay.
    std::vector<std::string> ConnectedControllers();
    // The first connected controller is a PlayStation one (DualShock / DualSense).
    bool PlayStationControllerConnected();
    // Diagnostics (camscan.cpp): forces player 1's right stick while on.
    void SetRightStickOverride(bool on, int16_t rx = 0, int16_t ry = 0);
    // Direct mouse aim: the mouse movement (pixels) since the last call; false while
    // the mouse isn't captured for aiming or direct aim is off.
    bool TakeMouseDelta(double& dx, double& dy);
    // Mouse movement (pixels) for the camera's native look hook (mouselook.cpp)
    // since its last call; false if the mouse isn't aiming. Calling it also keeps
    // the mouse away from the right stick for the next 300 ms.
    bool TakeLookDelta(double& dx, double& dy);
}
