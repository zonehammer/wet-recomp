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
#include "prompts.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "options.h"
#include "ppc_context.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

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
    std::mutex s_lock;                 // the indices; inflate hooks and the memory scan
    std::set<uint32_t> s_patched;      // constructor bytecode already handled

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
                if (!s_patched.insert(a).second)
                    continue;
                if (ps)
                {
                    if (a >= 0xA0000000u)
                        wet::PrepareHostWrite(a + 8, 1);
                    base[a + 8] = uint8_t(s_ps3Index);
                }
                wet::Log("Prompts: menu console set to %s (bytecode at %08X)", ps ? "PS3" : "X360", a);
            }
    }
}

extern "C" void __imp__sub_831B6120(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8314D3D8(PPCContext& ctx, uint8_t* base);

// zlib inflate(strm, flush), two copies in the game (sub_831B6120 for the
// engine's own data, sub_8314D3D8 inside Scaleform for the menus):
// z_stream { next_in, avail_in, total_in, next_out (+12), avail_out, total_out (+20), ... }.
namespace
{
    void Inflate(PPCContext& ctx, uint8_t* base, void (*impl)(PPCContext&, uint8_t*))
    {
        uint32_t strm = ctx.r3.u32;
        uint32_t out0 = strm ? PPC_LOAD_U32(strm + 12) : 0;
        uint32_t total0 = strm ? PPC_LOAD_U32(strm + 20) : 0;   // total_out: the output so far is contiguous
        impl(ctx, base);
        if (!strm || wet::options::ButtonPrompts() == 1)
            return;
        uint32_t out1 = PPC_LOAD_U32(strm + 12);
        static uint32_t calls[2];
        uint32_t& n = calls[impl == __imp__sub_8314D3D8 ? 1 : 0];
        if (++n <= 3)
            wet::Log("Prompts: %s inflate call %u (%u bytes out)", impl == __imp__sub_8314D3D8 ? "Scaleform" : "engine", n,
                     out1 - out0);
        if (out1 <= out0 || out1 - out0 > (64u << 20) || total0 > out0)
            return;
        uint32_t start = out0 - total0;
        // A little before the new data too, for a pattern split between calls.
        std::lock_guard lock(s_lock);
        ScanOutput(base, start, out0 - start >= 16 ? out0 - 16 : start, out1);
    }
}

extern "C" void __imp__sub_831B6120(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_831B6120)
{
    Inflate(ctx, base, __imp__sub_831B6120);
}

extern "C" void __imp__sub_8314D3D8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8314D3D8)
{
    Inflate(ctx, base, __imp__sub_8314D3D8);
}

// ---- Memory scan after a menu file is opened ----
// The menus may reach memory already decompressed; their code only runs when
// the menu is first shown (the pause menu: when the game is first paused), so
// the CApplication pool and constructor are searched for in guest memory a few
// times after the file opens, and patched in place.
namespace
{
    void ScanGuestMemory()
    {
#ifdef _WIN32
        uint8_t* base = wet::g_mem.base;
        static const std::boyer_moore_horspool_searcher<const char*> finder(kPoolMark, kPoolMark + sizeof(kPoolMark) - 1);
        uint64_t addr = 0x10000;
        int found = 0;
        while (addr < 0x100000000ull)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(base + addr, &mbi, sizeof(mbi)))
                break;
            uint64_t start = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
            uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, 0x100000000ull);
            bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
            bool mmio = start >= 0x7FC00000ull && start < 0x80000000ull;
            if (readable && !mmio && end > start && start < 0xC0000000ull)   // 0xC/0xE views repeat 0xA
            {
                const char* first = reinterpret_cast<const char*>(base + start);
                const char* last = reinterpret_cast<const char*>(base + end);
                for (const char* it = first;;)
                {
                    it = std::search(it, last, finder);
                    if (it == last)
                        break;
                    uint32_t mark = uint32_t(reinterpret_cast<const uint8_t*>(it) - base);
                    found++;
                    std::lock_guard lock(s_lock);
                    ReadPool(base, mark, mark - uint32_t(start) > 70000 ? mark - 70000 : uint32_t(start));
                    // The constructor follows the pool (same DoInitAction).
                    uint32_t to = uint32_t(std::min<uint64_t>(end, uint64_t(mark) + 200000));
                    ScanOutput(base, uint32_t(start), mark, to);
                    it += sizeof(kPoolMark) - 1;
                }
            }
            addr = end > addr ? end : addr + 0x1000;
        }
        static int logged = 0;
        if (logged++ < 20)
            wet::Log("Prompts: memory scan found %d menu constant pool(s)", found);
#endif
    }
}

namespace wet::prompts
{
    void FileOpened(const std::string& name)
    {
        if (options::ButtonPrompts() == 1)
            return;
        std::string lower = name;
        for (auto& c : lower)
            c = char(std::tolower((unsigned char)c));
        if (lower.find("menu") == std::string::npos && lower.find("extracontent") == std::string::npos)
            return;
        if (lower.find('.') != std::string::npos && lower.find("_") != std::string::npos &&
            lower.find("menu.") == std::string::npos && lower.find("extracontent.") == std::string::npos)
            return;
        std::thread([] {
            for (int ms : { 300, 1000, 2500, 6000, 15000 })
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                ScanGuestMemory();
            }
        }).detach();
    }
}
