// Your own music (music.cpp): a playlist from a folder that plays instead of
// the game's music, either following it (a song starts when the game starts a
// music track, fades out when the game's music stops) or all the time.
#pragma once
#include <string>

namespace wet::music
{
    // Starts the player thread (main.cpp); does nothing if the mode is off.
    void Start();

    // kernel/fs.cpp: the game opened / closed one of its music streams (mu_*.xma).
    void GameMusicOpened(const std::string& name);
    void GameMusicClosed(const std::string& name);

    // Controls (F1, media keys).
    void Next();
    void Previous();
    void TogglePause();
    // The playlist folder was changed or rescanned (F1's button).
    void Rescan();

    // For the overlay.
    struct Status
    {
        bool running = false;     // the player is on
        bool playing = false;     // a song is playing now
        bool paused = false;
        int tracks = 0;           // in the playlist
        std::string title;        // the current song (file name without the extension)
        std::string folder;
    };
    Status GetStatus();
}
