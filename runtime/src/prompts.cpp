// PlayStation button prompts.
//
// WET's menus (Scaleform movies) were shared with the PS3 version: they carry
// the PlayStation button art, and their CApplication class picks the set from
// one value set in its constructor,
//     consoleType = "X360";  gamePadDevice.setConfig(consoleType);
// (bytecode: Push r1, pool[consoleType], pool["X360"]; SetMember). The same
// constant pool also holds "PS3" (onLoad's setConsole("PS3")).
//
// Telling the whole game it runs on a PS3 (the scripts' GetCurrentPlateform)
// left the main menu black, so only the menus are changed: the movies are
// zlib-compressed and inflated by the game's zlib (inflate = sub_831B6120);
// whatever inflate writes is checked for the CApplication constant pool
// (remembering where consoleType / "X360" / "PS3" are in it) and for the
// constructor's push, whose "X360" pool index becomes the "PS3" one - before
// the movie ever runs. setConfig("PS3") only switches the key names the menus
// use (CROSS for A, ...); the movies handle both, as on the PS3.
#include "input.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <cstring>

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

    // Pool indices of the last CApplication constant pool seen (-1 = none yet).
    int s_ctIndex = -1, s_x360Index = -1, s_ps3Index = -1;

    constexpr char kPoolMark[] = "gameEnginePresent\0consoleType\0X360\0";   // inside CApplication's pool

    // A constant pool ending after `mark` (guest address of kPoolMark): find its
    // ConstantPool action (0x88, u16 length, u16 count, strings) and the indices.
    void ReadPool(uint8_t* base, uint32_t mark, uint32_t lowest)
    {
        for (uint32_t p = mark; p > lowest && mark - p < 60000; p--)
        {
            if (base[p - 1] != 0x88)
                continue;
            uint32_t at = p - 1;
            uint32_t len = base[at + 1] | base[at + 2] << 8, count = base[at + 3] | base[at + 4] << 8;
            if (count == 0 || count > 4000 || at + 3 + len < mark + sizeof(kPoolMark) - 1)
                continue;
            // The strings must end exactly at the action's end.
            uint32_t s = at + 5, end = at + 3 + len, n = 0;
            int ct = -1, x360 = -1, ps3 = -1;
            while (s < end && n < count)
            {
                const char* str = reinterpret_cast<const char*>(base + s);
                size_t l = strnlen(str, end - s);
                if (s + l >= end)
                    break;
                if (l == 11 && !std::memcmp(str, "consoleType", 11)) ct = int(n);
                else if (l == 4 && !std::memcmp(str, "X360", 4)) x360 = int(n);
                else if (l == 3 && !std::memcmp(str, "PS3", 3)) ps3 = int(n);
                s += uint32_t(l) + 1;
                n++;
            }
            if (n != count || s != end)
                continue;
            s_ctIndex = ct;
            s_x360Index = x360;
            s_ps3Index = ps3;
            wet::Log("Prompts: menu constant pool at %08X (%u strings): consoleType %d, X360 %d, PS3 %d", at, count,
                     ct, x360, ps3);
            return;
        }
    }

    // [from, to): new output; start: where this stream's output began (nothing
    // before it is read).
    void ScanOutput(uint8_t* base, uint32_t start, uint32_t from, uint32_t to)
    {
        // Pool marker.
        for (uint32_t a = from; a + sizeof(kPoolMark) - 1 <= to; a++)
            if (base[a] == 'g' && !std::memcmp(base + a, kPoolMark, sizeof(kPoolMark) - 1))
                ReadPool(base, a, a - start > 70000 ? a - 70000 : start);
        if (s_ctIndex < 0 || s_x360Index < 0 || s_ps3Index < 0 || s_ctIndex > 255 || s_x360Index > 255 ||
            s_ps3Index > 255)
            return;
        // Constructor: Push r1, pool[consoleType], pool[X360]; SetMember.
        const uint8_t pat[] = { 0x96, 6, 0, 4, 1, 8, uint8_t(s_ctIndex), 8, uint8_t(s_x360Index), 0x4F };
        for (uint32_t a = from; a + sizeof(pat) <= to; a++)
            if (base[a] == 0x96 && !std::memcmp(base + a, pat, sizeof(pat)))
            {
                bool ps = WantPlayStation();
                if (ps)
                    base[a + 8] = uint8_t(s_ps3Index);
                wet::Log("Prompts: menu console set to %s (bytecode at %08X)", ps ? "PS3" : "X360", a);
            }
    }
}

// zlib inflate(strm, flush): z_stream { next_in, avail_in, total_in, next_out (+12), ... }.
extern "C" void __imp__sub_831B6120(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_831B6120)
{
    uint32_t strm = ctx.r3.u32;
    uint32_t out0 = strm ? PPC_LOAD_U32(strm + 12) : 0;
    uint32_t total0 = strm ? PPC_LOAD_U32(strm + 20) : 0;   // total_out: the output so far is contiguous
    __imp__sub_831B6120(ctx, base);
    if (!strm || wet::options::ButtonPrompts() == 1)
        return;
    uint32_t out1 = PPC_LOAD_U32(strm + 12);
    if (out1 <= out0 || out1 - out0 > (64u << 20) || total0 > out0)
        return;
    uint32_t start = out0 - total0;
    // A little before the new data too, for a pattern split between calls.
    ScanOutput(base, start, out0 - start >= 16 ? out0 - 16 : start, out1);
}
