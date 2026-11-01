// PlayStation button prompts.
//
// Every button icon the game shows - menus, HUD, tutorials - is one of a set of
// small DDS textures in global.lu (button_A_b.dds, button_3.dds, button_a.dds,
// ...), created through the HUD's image creator (A2MImageCreator, slot 1 =
// sub_82BA0318), which reads each one from the archive with a memory stream and
// the DDS loader sub_82B9AC80(stream). The Xbox disc has no PlayStation art
// (the movies' consoleType / the scripts' GetCurrentPlateform only change key
// handling, TRC messages and rendering paths), so the art is redrawn: as the
// loader is about to read one of the button textures, its pixels in the
// archive are replaced by the PlayStation version (prompt_art.h) - same shape,
// tint and size, a cross / circle / square / triangle or L1 / R1 / L2 / R2.
// Each texture is only redrawn the first time it is read: the game then
// converts it to the GPU's layout in place, so the choice is made at startup.
//
// The stream (sub_82B9C1A8): +16 the archive entry { +0 name, +8 data, +12 size }.
//
// Setting button_prompts: 0 = automatic (PlayStation art when a PlayStation
// controller is connected as the images load), 1 = Xbox, 2 = PlayStation.
#include "input.h"
#include "log.h"
#include "memory.h"
#include "options.h"
#include "ppc_context.h"
#include "prompt_art.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    bool WantPlayStation()
    {
        switch (wet::options::ButtonPrompts())
        {
        case 1: return false;
        case 2: return true;
        default: return wet::PlayStationControllerConnected();
        }
    }

    std::mutex s_lock;
    std::unordered_map<uint32_t, uint64_t> s_seen;   // data address -> hash of its DDS header
    int s_logged = 0, s_redrawn = 0;

    uint64_t Hash(const uint8_t* p, size_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }

    bool Valid(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000u; }

    void Redraw(uint8_t* base, uint32_t stream)
    {
        uint32_t entry = PPC_LOAD_U32(stream + 16);
        if (!Valid(entry))
            return;
        uint32_t namePtr = PPC_LOAD_U32(entry + 0), data = PPC_LOAD_U32(entry + 8), size = PPC_LOAD_U32(entry + 12);
        if (!Valid(namePtr) || !Valid(data) || size < 128 || size > (1u << 20) || data + size < data)
            return;
        std::string name(reinterpret_cast<const char*>(base + namePtr), strnlen(reinterpret_cast<const char*>(base + namePtr), 128));
        bool orb, flatten;
        bool button = wet::prompt_art::ForName(name, orb, flatten) != wet::prompt_art::Symbol::None;

        std::lock_guard lock(s_lock);
        if (s_logged < 6 || (button && s_logged < 12))
        {
            s_logged++;
            wet::Log("Prompts: image \"%s\" (%u bytes at %08X)%s", name.c_str(), size, data, button ? " - a button" : "");
        }
        if (!button)
            return;
        // Only the first time a texture is read: after that the game has
        // converted its pixels to the GPU's layout in place (redrawing those
        // gave garbage when the setting changed mid-game).
        uint8_t* dds = base + data;
        auto it = s_seen.find(data);
        if (it != s_seen.end() && it->second == Hash(dds, 128))
            return;
        if (!WantPlayStation())
        {
            s_seen[data] = Hash(dds, 128);
            return;
        }
        if (data >= 0xA0000000u)
            wet::PrepareHostWrite(data, size);
        if (!wet::prompt_art::RedrawDds(name, dds, size))
        {
            wet::Log("Prompts: \"%s\" isn't in a format the PlayStation art handles", name.c_str());
            return;
        }
        s_seen[data] = Hash(dds, 128);
        if (++s_redrawn <= 3 || s_redrawn % 20 == 0)
            wet::Log("Prompts: \"%s\" redrawn with PlayStation art (%d so far)", name.c_str(), s_redrawn);
    }
}

// The HUD's DDS loader: reads the texture from the stream (r3).
extern "C" void __imp__sub_82B9AC80(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82B9AC80)
{
    if (ctx.r3.u32 && wet::options::ButtonPrompts() != 1)
        Redraw(base, ctx.r3.u32);
    __imp__sub_82B9AC80(ctx, base);
}
