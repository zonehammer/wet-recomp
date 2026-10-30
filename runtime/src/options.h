// Player options, kept in wet.ini next to the logs (key = value lines).
#pragma once

namespace wet::options
{
    void Load();
    // 60 FPS instead of the game's 30 (present every vblank). F6 toggles it.
    bool Fps60();
    void ToggleFps60();
}
