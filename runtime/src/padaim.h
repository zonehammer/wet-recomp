// The game's pad object and direct mouse aim. See padaim.cpp.
#pragma once
#include <cstdint>
#include <vector>

namespace wet::padaim
{
    // Guest address of user 0's pad object (0 until the game has read the pad).
    uint32_t PadAddress();

    // Diagnostics (Shift+F7): who calls the pad's GetAxis, with which axis.
    struct AxisCall { uint32_t lr, axis, count; };
    void LogAxisReaders(bool on);
    std::vector<AxisCall> AxisReaders();
}
