// Your own music (music.cpp): a playlist from a folder that plays instead of
// the game's music, either following it (a song starts when the game starts a
// music track, fades out when the game's music stops) or all the time.
#pragma once
#include <cstdint>
#include <string>

namespace wet::music
{
    // Starts the player thread (main.cpp); does nothing if the mode is off.
    void Start();

    // kernel/fs.cpp: the game opened / closed one of its music streams (mu_*.xma).
    void GameMusicOpened(const std::string& name);
    void GameMusicClosed(const std::string& name);

    // kernel/fs.cpp: a file read into guest memory; `music` if it was one of
    // the game's music streams. Where the game's music lands, so its XMA
    // decoding can be silenced while your songs play.
    void GameRead(uint32_t guest, uint32_t length, bool music);
    // The XMA decoder (apu bridge): the gain for a context decoding the input
    // buffer at this physical address (0 while your songs replace the game's).
    float GameMusicGain(uint32_t inputPhys);

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
