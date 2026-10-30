// Achievements: the title's list (names, descriptions, gamerscore, icons) from the
// SPA/XDBF resource in default.xex, unlocks the game reports (XUserWriteAchievements
// via XMsgStartIORequest), saved in <hdd>/achievements.ini, and the queue of
// "Achievement unlocked" pop-ups the overlay shows.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wet::achievements
{
    struct Achievement
    {
        uint32_t id = 0;
        std::string name, description, lockedDescription;
        uint32_t gamerscore = 0;
        uint32_t imageId = 0;
        uint32_t flags = 0;         // XDBF flags (bit 3: shown before unlocking)
        bool secret = false;
        bool unlocked = false;
        uint64_t unlockTime = 0;    // Unix seconds
    };

    // After the title module is loaded; reads the save from hddDir.
    void Load(const std::filesystem::path& hddDir);
    // An unlock from the game; ignored if already unlocked or unknown.
    void Unlock(uint32_t id);
    std::vector<Achievement> List();
    // Next pending pop-up, if any.
    bool PopPopup(Achievement& out);
    bool HasPopup();
    // PNG bytes of an achievement image (empty if none).
    std::vector<uint8_t> Image(uint32_t imageId);
}
