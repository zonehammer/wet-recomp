// Player options, kept in wet.ini next to the logs (key = value lines).
#pragma once

namespace wet::options
{
    void Load();
    // Frame rate: 30 (the game's own), 60, 120 or 144. F6 cycles through them.
    int Fps();
    void CycleFps();
    // Rendering resolution scale: 1 (720p, the game's own), 2 (1440p), 3 (2160p / 4K).
    // Takes effect at the next start.
    int ResolutionScale();
}
