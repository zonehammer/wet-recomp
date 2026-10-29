// Host controller backend (XInput on Windows; an idle virtual pad elsewhere).
// Kept free of <windows.h> so it never collides with the kernel's status constants.
#pragma once
#include <cstdint>

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
}
