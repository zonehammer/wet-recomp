// Rubi's outfit colour (outfit.h for the recolouring). Her clothes' texture is
// recognised by a hash of its data when a draw first reads it (the GPU
// emulation's texture hook); the original is kept, so a new colour (F1) is
// applied from it, and Original puts it back.
#include "outfit.h"
#include "log.h"
#include "memory.h"
#include "options.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace wet::outfit
{
    namespace
    {
        // Her textures (1024x1024, the base level as stored in memory).
        struct Target
        {
            const char* what;
            uint64_t hash;
            uint32_t format;   // 18 DXT1, 20 DXT5
            uint32_t size;
            Pick pick;
        };
        const Target kTargets[] = {
            { "top", 0x7e3ef1552e320708ull, 18, 524288, Pick::kRed },        // with her skin and tattoos
            { "trousers", 0xe10a6dbb59e40b74ull, 20, 1048576, Pick::kAll },  // brown leather
        };

        struct Entry
        {
            const Target* target = nullptr;
            std::vector<uint8_t> base, mips;   // as the game loaded them
            uint64_t written = 0;              // hash of what's in memory now
            int colour = 0;
        };
        std::mutex s_lock;
        std::unordered_map<uint32_t, Entry> s_entries;

        uint64_t Hash(const uint8_t* p, size_t n)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < n; i++)
                h = (h ^ p[i]) * 0x100000001b3ull;
            return h;
        }

        void Recolour(std::vector<uint8_t>& data, const Target& t, int colour)
        {
            // 8-in-16 byte order in memory.
            for (size_t i = 0; i + 1 < data.size(); i += 2)
                std::swap(data[i], data[i + 1]);
            RecolourDxt(data.data(), data.size(), t.format == 20, Colour(colour), t.pick);
            for (size_t i = 0; i + 1 < data.size(); i += 2)
                std::swap(data[i], data[i + 1]);
        }

        void Write(uint32_t phys, uint8_t* dst, const std::vector<uint8_t>& src)
        {
            if (!dst || src.empty())
                return;
            // The GPU emulation's copy goes (write-watch), then the new data.
            PrepareHostWrite(0xA0000000u + phys, uint32_t(src.size()));
            std::memcpy(dst, src.data(), src.size());
        }
    }

    void TextureSeen(uint32_t phys, uint8_t* data, uint32_t format, uint32_t baseSize, uint32_t endian,
                     uint32_t mipPhys, uint8_t* mipData, uint32_t mipSize)
    {
        if (endian != 1 || !data)
            return;
        const Target* sized = nullptr;
        for (const Target& t : kTargets)
            if (t.format == format && t.size == baseSize)
                sized = &t;
        std::lock_guard lock(s_lock);
        auto it = s_entries.find(phys);
        if (!sized)
        {
            if (it != s_entries.end())
                s_entries.erase(it);
            return;
        }
        int colour = options::OutfitColour();
        uint64_t now = Hash(data, baseSize);
        if (it != s_entries.end() && it->second.written == now)
        {
            if (it->second.colour == colour)
                return;   // already as wanted
        }
        else
        {
            const Target* target = nullptr;
            for (const Target& t : kTargets)
                if (t.format == format && t.size == baseSize && t.hash == now)
                    target = &t;
            if (!target)
            {
                if (it != s_entries.end())
                    s_entries.erase(it);   // something else is there now
                return;
            }
            Entry e;
            e.target = target;
            e.base.assign(data, data + baseSize);
            if (mipData && mipSize)
                e.mips.assign(mipData, mipData + mipSize);
            e.written = now;
            e.colour = 0;
            it = s_entries.insert_or_assign(phys, std::move(e)).first;
            if (colour == 0)
            {
                Log("Outfit: Rubi's %s at %08X (left as it is: Original)", target->what, phys);
                return;
            }
        }
        Entry& e = it->second;
        std::vector<uint8_t> base = e.base, mips = e.mips;
        if (colour != 0)
        {
            Recolour(base, *e.target, colour);
            Recolour(mips, *e.target, colour);
        }
        Write(phys, data, base);
        if (mipData && mips.size() == mipSize)
            Write(mipPhys, mipData, mips);
        e.written = Hash(data, baseSize);
        e.colour = colour;
        Log("Outfit: Rubi's %s at %08X now %s", e.target->what, phys, kNames[std::clamp(colour, 0, int(kCount) - 1)]);
    }
}
