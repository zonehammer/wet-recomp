#include "achievements.h"
#include "gpu/renderer.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"
#include "module.h"
#include "ppc_context.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>

namespace wet::achievements
{
    namespace
    {
        std::mutex s_lock;
        std::vector<Achievement> s_list;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> s_images;   // image id -> guest address, size
        std::deque<Achievement> s_popups;
        std::filesystem::path s_file;

        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
        uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

        // The SPA resource is named after the title id ("425307DB").
        bool FindSpa(uint32_t& address, uint32_t& size)
        {
            uint32_t info = XexHeaderField(g_module.guestHeader, 0x000002FF);
            if (!info)
                return false;
            char want[9];
            std::snprintf(want, sizeof(want), "%08X", g_module.titleId);
            uint32_t count = (Ld32(info) - 4) / 16;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t e = info + 4 + i * 16;
                char name[9] = {};
                std::memcpy(name, HostPtr(e), 8);
                if (!std::strncmp(name, want, 8))
                {
                    address = Ld32(e + 8);
                    size = Ld32(e + 12);
                    return true;
                }
            }
            return false;
        }

        struct Xdbf
        {
            const uint8_t* data = nullptr;
            size_t size = 0;
            uint32_t entryCount = 0, entryUsed = 0, freeCount = 0;
            const uint8_t* entries = nullptr;
            const uint8_t* content = nullptr;

            bool Open(const uint8_t* d, size_t n)
            {
                if (n < 24 || Be32(d) != 0x58444246)   // 'XDBF'
                    return false;
                data = d;
                size = n;
                entryCount = Be32(d + 8);
                entryUsed = Be32(d + 12);
                freeCount = Be32(d + 16);
                entries = d + 24;
                content = entries + size_t(entryCount) * 18 + size_t(freeCount) * 8;
                return content <= d + n;
            }

            // Returns the block, or nullptr.
            const uint8_t* Get(uint16_t section, uint64_t id, uint32_t* blockSize = nullptr, uint32_t* offset = nullptr) const
            {
                for (uint32_t i = 0; i < entryUsed && i < entryCount; i++)
                {
                    const uint8_t* e = entries + i * 18;
                    if (Be16(e) == section && Be64(e + 2) == id)
                    {
                        uint32_t off = Be32(e + 10), sz = Be32(e + 14);
                        if (content + off + sz > data + size)
                            return nullptr;
                        if (blockSize) *blockSize = sz;
                        if (offset) *offset = uint32_t(content - data) + off;
                        return content + off;
                    }
                }
                return nullptr;
            }

            std::map<uint16_t, std::string> Strings(uint32_t language) const
            {
                std::map<uint16_t, std::string> out;
                uint32_t sz = 0;
                const uint8_t* b = Get(3, language, &sz);
                if (!b || sz < 14 || Be32(b) != 0x58535452)   // 'XSTR'
                    return out;
                uint16_t count = Be16(b + 12);
                const uint8_t* p = b + 14, *end = b + sz;
                for (uint16_t i = 0; i < count && p + 4 <= end; i++)
                {
                    uint16_t id = Be16(p), len = Be16(p + 2);
                    p += 4;
                    if (p + len > end)
                        break;
                    out[id] = std::string(reinterpret_cast<const char*>(p), len);
                    p += len;
                }
                return out;
            }
        };

        void SaveLocked()
        {
            if (s_file.empty())
                return;
            FILE* f = std::fopen(s_file.string().c_str(), "w");
            if (!f)
                return;
            std::fprintf(f, "# WET Recomp achievements: id = unlock time (Unix seconds). Delete a line to re-lock it.\n");
            for (auto& a : s_list)
                if (a.unlocked)
                    std::fprintf(f, "%u = %llu  # %s\n", a.id, (unsigned long long)a.unlockTime, a.name.c_str());
            std::fclose(f);
        }
    }

    void Load(const std::filesystem::path& hddDir)
    {
        std::lock_guard lock(s_lock);
        uint32_t address = 0, size = 0;
        Xdbf x;
        if (!FindSpa(address, size) || !x.Open(HostPtr(address), size))
        {
            Log("Achievements: no achievement data in default.xex");
            return;
        }
        uint32_t sz = 0;
        const uint8_t* xach = x.Get(1, 0x58414348, &sz);   // 'XACH'
        if (!xach || sz < 14 || Be32(xach) != 0x58414348)
        {
            Log("Achievements: the title has none");
            return;
        }
        uint32_t language = 1;   // English
        if (const uint8_t* xstc = x.Get(1, 0x58535443); xstc && Be32(xstc) == 0x58535443)
            language = Be32(xstc + 12);
        auto strings = x.Strings(1);
        if (strings.empty())
            strings = x.Strings(language);
        uint16_t count = Be16(xach + 12);
        const uint8_t* p = xach + 14;
        for (uint16_t i = 0; i < count && p + 0x24 <= xach + sz; i++, p += 0x24)
        {
            Achievement a;
            a.id = Be16(p);
            a.name = strings[Be16(p + 2)];
            a.description = strings[Be16(p + 4)];
            a.lockedDescription = strings[Be16(p + 6)];
            a.imageId = Be32(p + 8);
            a.gamerscore = Be16(p + 12);
            a.flags = Be32(p + 16);
            a.secret = !(a.flags & 0x8);
            s_list.push_back(a);
            uint32_t imgSize = 0, imgOffset = 0;
            if (x.Get(2, a.imageId, &imgSize, &imgOffset))
                s_images[a.imageId] = { address + imgOffset, imgSize };
        }
        s_file = hddDir / "achievements.ini";
        if (FILE* f = std::fopen(s_file.string().c_str(), "r"))
        {
            char line[512];
            while (std::fgets(line, sizeof(line), f))
            {
                unsigned id;
                unsigned long long t;
                if (std::sscanf(line, " %u = %llu", &id, &t) != 2)
                    continue;
                for (auto& a : s_list)
                    if (a.id == id)
                    {
                        a.unlocked = true;
                        a.unlockTime = t;
                    }
            }
            std::fclose(f);
        }
        uint32_t total = 0, got = 0, unlocked = 0;
        for (auto& a : s_list)
        {
            total += a.gamerscore;
            if (a.unlocked)
            {
                got += a.gamerscore;
                unlocked++;
            }
        }
        Log("Achievements: %u/%zu unlocked, %u/%u gamerscore (%s)", unlocked, s_list.size(), got, total,
            s_file.string().c_str());
    }

    void Unlock(uint32_t id)
    {
        {
            std::lock_guard lock(s_lock);
            Achievement* found = nullptr;
            for (auto& a : s_list)
                if (a.id == id)
                    found = &a;
            if (!found)
            {
                Log("Achievements: the game unlocked unknown achievement %u", id);
                return;
            }
            if (found->unlocked)
                return;
            found->unlocked = true;
            found->unlockTime = uint64_t(std::time(nullptr));
            SaveLocked();
            s_popups.push_back(*found);
            Log("Achievements: unlocked \"%s\" (%uG)", found->name.c_str(), found->gamerscore);
        }
        renderer::AchievementUnlocked();
    }

    void UnlockMany(const std::vector<uint32_t>& ids)
    {
        std::vector<Achievement> fresh;
        {
            std::lock_guard lock(s_lock);
            for (uint32_t id : ids)
                for (auto& a : s_list)
                    if (a.id == id && !a.unlocked)
                    {
                        a.unlocked = true;
                        a.unlockTime = uint64_t(std::time(nullptr));
                        fresh.push_back(a);
                        Log("Achievements: unlocked \"%s\" (%uG)", a.name.c_str(), a.gamerscore);
                    }
            if (fresh.empty())
                return;
            SaveLocked();
            if (fresh.size() <= 2)
                for (auto& a : fresh)
                    s_popups.push_back(a);
            else
            {
                Achievement summary = fresh.front();
                uint32_t score = 0;
                for (auto& a : fresh)
                    score += a.gamerscore;
                summary.name = std::to_string(fresh.size()) + " achievements unlocked";
                summary.description = "Earned in earlier play - see the F1 overlay";
                summary.gamerscore = score;
                s_popups.push_back(summary);
            }
        }
        renderer::AchievementUnlocked();
    }

    std::vector<Achievement> List()
    {
        std::lock_guard lock(s_lock);
        return s_list;
    }

    bool PopPopup(Achievement& out)
    {
        std::lock_guard lock(s_lock);
        if (s_popups.empty())
            return false;
        out = s_popups.front();
        s_popups.pop_front();
        return true;
    }

    bool HasPopup()
    {
        std::lock_guard lock(s_lock);
        return !s_popups.empty();
    }

    std::vector<uint8_t> Image(uint32_t imageId)
    {
        std::lock_guard lock(s_lock);
        auto it = s_images.find(imageId);
        if (it == s_images.end())
            return {};
        const uint8_t* p = HostPtr(it->second.first);
        return std::vector<uint8_t>(p, p + it->second.second);
    }
}

// Diagnostics: the title's achievement flush (a Lua-side queue of
// { user, achievement id } pairs handed to XUserWriteAchievements). Logs each
// call, so a missing pop-up can be told apart from the game never awarding.
extern "C" void __imp__sub_82CDDE00(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82CDDE00)
{
    static int logged = 0;
    uint32_t queue = PPC_LOAD_U32(ctx.r3.u32 + 64);
    uint32_t count = queue ? (PPC_LOAD_U32(queue + 8) - PPC_LOAD_U32(queue + 4)) / 8 : 0;
    if (logged < 200)
    {
        logged++;
        std::string ids;
        for (uint32_t i = 0; i < count && i < 8; i++)
            ids += " " + std::to_string(PPC_LOAD_U32(PPC_LOAD_U32(queue + 4) + i * 8 + 4));
        wet::Log("Achievements: the game is writing %u achievement(s):%s", count, ids.c_str());
    }
    __imp__sub_82CDDE00(ctx, base);
}
