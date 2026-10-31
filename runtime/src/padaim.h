// The game's pad object and direct mouse aim. See padaim.cpp.
#pragma once
#include <cstdint>

namespace wet::padaim
{
    // Guest address of user 0's pad object (0 until the game has read the pad).
    uint32_t PadAddress();
}
