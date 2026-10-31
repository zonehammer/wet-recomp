// Camera finder (diagnostics for direct mouse aim). F7 in game, standing still in
// gameplay with the camera free to turn:
//
//  1. The right stick is driven by the runtime (right, rest, left, rest, right,
//     then the same up/down) while guest memory is compared between the phases.
//     Floats that move one way for "right", hold still at rest, move the other way
//     for "left" and the first way again are camera-angle candidates.
//  2. The best candidates' pages get write watchpoints for a few seconds: every
//     write to a candidate records the host instruction, which the linker map turns
//     into the guest function (the camera update) - tools/resolve_profile.py style.
//
// Results go to camscan.txt and wet.log. Windows only.
#include "camscan.h"
#include "input.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"
#include "os.h"
#include "padaim.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wet::camscan
{
#ifdef _WIN32
    namespace
    {
        std::atomic<bool> s_running{ false };

        struct Region { uint32_t guest; std::vector<uint8_t> data; };
        struct Candidate { uint32_t addr; float v[8]; };

        float LoadFloat(uint32_t guest)
        {
            uint32_t u = Ld32(guest);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        // Finite, not absurd, and not a denormal (integers and flags read as floats).
        bool Plausible(float f) { return std::isfinite(f) && std::fabs(f) < 1.0e5f && (f == 0.0f || std::fabs(f) > 1.0e-6f); }

        // Readable, committed guest memory: everything below 0xC0000000 except MMIO
        // (the 0xC/0xE physical views repeat the 0xA one).
        std::vector<Region> Snapshot()
        {
            std::vector<Region> out;
            uint8_t* base = g_mem.base;
            uint64_t addr = 0x10000;
            while (addr < 0xC0000000ull)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(base + addr, &mbi, sizeof(mbi)))
                    break;
                uint64_t start = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
                uint64_t end = std::min<uint64_t>(start + mbi.RegionSize, 0xC0000000ull);
                bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                                (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY));
                bool mmio = start >= kMmioBase && start < 0x80000000ull;
                if (readable && !mmio && end > start)
                {
                    Region r;
                    r.guest = uint32_t(start);
                    r.data.assign(base + start, base + end);
                    out.push_back(std::move(r));
                }
                addr = end > addr ? end : addr + 0x1000;
            }
            return out;
        }

        void Hold(int16_t rx, int16_t ry, double seconds)
        {
            SetRightStickOverride(true, rx, ry);
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        }

        // One axis: returns the candidates, best first.
        std::vector<Candidate> ScanAxis(bool vertical, FILE* report)
        {
            const int16_t kPush = 32767;
            auto push = [&](int sign, double s) { vertical ? Hold(0, int16_t(sign * kPush), s) : Hold(int16_t(sign * kPush), 0, s); };

            Hold(0, 0, 0.8);
            std::vector<Region> before = Snapshot();
            size_t bytes = 0;
            for (auto& r : before)
                bytes += r.data.size();
            Log("Camera scan (%s): compared %zu MB of guest memory", vertical ? "up/down" : "left/right", bytes >> 20);

            push(+1, 0.25);
            std::vector<Candidate> c;
            for (auto& r : before)
                for (size_t o = 0; o + 4 <= r.data.size(); o += 4)
                {
                    uint32_t u;
                    std::memcpy(&u, &r.data[o], 4);
                    u = __builtin_bswap32(u);
                    float v0;
                    std::memcpy(&v0, &u, 4);
                    uint32_t addr = r.guest + uint32_t(o);
                    float v1 = LoadFloat(addr);
                    if (v1 != v0 && Plausible(v0) && Plausible(v1) && std::fabs(v1 - v0) < 50.0f && c.size() < 4000000)
                        c.push_back({ addr, { v0, v1 } });
                }
            before.clear();
            before.shrink_to_fit();
            size_t first = c.size();
            // Still pushing: a camera angle keeps moving the same way (a flag or a
            // stick value set once doesn't).
            push(+1, 0.25);
            for (auto& x : c)
            {
                float now = LoadFloat(x.addr);
                x.v[1] = (now - x.v[1]) * (x.v[1] - x.v[0]) > 0 ? now : NAN;
            }
            c.erase(std::remove_if(c.begin(), c.end(), [](const Candidate& x) { return std::isnan(x.v[1]); }), c.end());

            auto sample = [&](int slot) {
                for (auto& x : c)
                    x.v[slot] = LoadFloat(x.addr);
            };
            auto keep = [&](auto pred) {
                c.erase(std::remove_if(c.begin(), c.end(), [&](const Candidate& x) { return !pred(x); }), c.end());
            };
            auto sign = [](float d) { return d > 0 ? 1 : d < 0 ? -1 : 0; };

            Hold(0, 0, 0.6);
            sample(2);
            Hold(0, 0, 0.2);
            sample(3);
            keep([](const Candidate& x) { return x.v[2] == x.v[3]; });               // still at rest
            push(-1, 0.25);
            sample(4);
            keep([&](const Candidate& x) { return sign(x.v[4] - x.v[3]) == -sign(x.v[1] - x.v[0]) && x.v[4] != x.v[3]; });
            push(-1, 0.25);
            keep([&](const Candidate& x) {
                float now = LoadFloat(x.addr);
                return sign(now - x.v[4]) == sign(x.v[4] - x.v[3]);
            });
            sample(4);
            Hold(0, 0, 0.6);
            sample(5);
            Hold(0, 0, 0.2);
            sample(6);
            keep([](const Candidate& x) { return x.v[5] == x.v[6]; });
            push(+1, 0.5);
            sample(7);
            keep([&](const Candidate& x) { return sign(x.v[7] - x.v[6]) == sign(x.v[1] - x.v[0]); });
            Hold(0, 0, 0.5);

            keep([](const Candidate& x) {
                for (float v : x.v)
                    if (!Plausible(v))
                        return false;
                // Values flipping between two exact states (double-buffered render
                // data, flags) repeat bit for bit; a turned angle never lands exactly
                // where it was.
                if (x.v[7] == x.v[1] || x.v[4] == x.v[0] || x.v[6] == x.v[0])
                    return false;
                // An angle holds where it was left (allowing for a little smoothing).
                float push = std::fabs(x.v[1] - x.v[0]);
                return std::fabs(x.v[3] - x.v[1]) <= 0.5f * push;
            });
            // Best: the same size of change each time (a steady turn rate).
            auto score = [](const Candidate& x) {
                float a = std::fabs(x.v[1] - x.v[0]), b = std::fabs(x.v[4] - x.v[3]), d = std::fabs(x.v[7] - x.v[6]);
                float hi = std::max({ a, b, d }), lo = std::min({ a, b, d });
                return hi > 0 ? lo / hi : 0.0f;
            };
            std::sort(c.begin(), c.end(), [&](const Candidate& a, const Candidate& b) { return score(a) > score(b); });

            Log("Camera scan (%s): %zu floats changed, %zu behave like a camera angle", vertical ? "up/down" : "left/right",
                first, c.size());
            std::fprintf(report, "\n== %s: %zu floats changed after the first push, %zu candidates ==\n",
                         vertical ? "UP/DOWN (pitch)" : "LEFT/RIGHT (yaw)", first, c.size());
            std::fprintf(report, "# address    score  rest0 -> push1 | rest -> opposite | rest -> push again\n");
            for (size_t i = 0; i < c.size() && i < 60; i++)
            {
                auto& x = c[i];
                std::fprintf(report, "%08X  %.2f  %g -> %g | %g -> %g | %g -> %g\n", x.addr, score(x), x.v[0], x.v[1],
                             x.v[3], x.v[4], x.v[6], x.v[7]);
            }
            return c;
        }

        // ---- Write watchpoints on the best candidates ----
        // Each watched guest page is read-only in every view it has; a write records
        // the instruction, unprotects just that page for the one instruction (the
        // thread single-steps) and the step handler protects it again. Writes to
        // physical pages also go through PrepareHostWrite, so the GPU emulation's
        // own write watches still see them.
        std::mutex s_watchLock;
        std::map<uintptr_t, uint32_t> s_watchPages;                // host page -> guest page
        std::map<uint32_t, uint32_t> s_watchAddrs;                 // guest candidate address -> index
        std::map<std::pair<uint32_t, uintptr_t>, uint32_t> s_writers; // (candidate, host pc) -> count
        std::atomic<bool> s_watching{ false };
        thread_local uintptr_t t_steppingPage = 0;

        uint32_t HostToGuest(uintptr_t host)
        {
            uint64_t off = uint64_t(host - uintptr_t(g_mem.base));
            if (off >= 0xC0000000ull)
            {
                // 0xC / 0xE physical views -> 0xA
                if (off < 0xE0000000ull)
                    return uint32_t(off - 0xC0000000ull + 0xA0000000ull);
                return uint32_t(off - (0xE0000000ull - g_mem.eViewOffset) + 0xA0000000ull);
            }
            return uint32_t(off);
        }

        bool Writable(uintptr_t page)
        {
            MEMORY_BASIC_INFORMATION mbi;
            return VirtualQuery(reinterpret_cast<void*>(page), &mbi, sizeof(mbi)) &&
                   (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
        }

        // ---- Pad reader watch (Shift+F7) ----
        // The pad object's page is made no-access for a few seconds; every access
        // to the first 64 bytes (sticks at +8..+20, triggers +56/+60) records the
        // host instruction, then the thread single-steps it with the page open.
        std::atomic<bool> s_padWatching{ false };
        std::vector<uintptr_t> s_padPages;   // host views of the pad's page
        thread_local uintptr_t t_padPage = 0;
        uint32_t s_padGuest = 0;
        std::map<std::pair<uint32_t, uintptr_t>, uint32_t> s_padReaders;   // (offset, host pc) -> count
        thread_local bool t_padStepping = false;

        int OnPadFault(uintptr_t host, uintptr_t pc)
        {
            uintptr_t page = host & ~uintptr_t(0xFFF);
            std::lock_guard lock(s_watchLock);
            if (std::find(s_padPages.begin(), s_padPages.end(), page) == s_padPages.end())
                return 0;
            if (!s_padWatching.load())
                return Writable(page) ? 2 : 0;   // released meanwhile: retry
            uint32_t guest = HostToGuest(host);
            if (guest >= s_padGuest && guest < s_padGuest + 64)
                s_padReaders[{ guest - s_padGuest, pc }]++;
            if (s_padGuest >= 0xA0000000u)
                PrepareHostWrite(s_padGuest & ~0xFFFu, 0x1000);   // in case it's a write
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
            t_padStepping = true;
            t_padPage = page;
            return 1;
        }

        int OnDiagFault(uintptr_t host, uintptr_t pc)
        {
            if (int r = OnPadFault(host, pc))
                return r;
            uintptr_t page = host & ~uintptr_t(0xFFF);
            std::lock_guard lock(s_watchLock);
            auto it = s_watchPages.find(page);
            if (it == s_watchPages.end())
                return 0;
            if (!s_watching.load())
                return Writable(page) ? 2 : 0;   // released meanwhile: retry; else not ours any more
            uint32_t guest = HostToGuest(host) & ~3u;
            if (s_watchAddrs.count(guest))
                s_writers[{ guest, pc }]++;
            if (it->second >= 0xA0000000u)
                PrepareHostWrite(it->second, 0x1000);   // the GPU watches' invalidation
            os::ProtectReadWrite(reinterpret_cast<void*>(page), 0x1000);
            t_steppingPage = page;
            return 1;
        }

        void OnDiagStep()
        {
            if (t_padStepping)
            {
                t_padStepping = false;
                std::lock_guard lock(s_watchLock);
                if (s_padWatching.load() && t_padPage)
                {
                    DWORD old;
                    VirtualProtect(reinterpret_cast<void*>(t_padPage), 0x1000, PAGE_NOACCESS, &old);
                }
                t_padPage = 0;
                return;
            }
            uintptr_t page = t_steppingPage;
            t_steppingPage = 0;
            std::lock_guard lock(s_watchLock);
            if (page && s_watching.load())
                os::ProtectReadOnly(reinterpret_cast<void*>(page), 0x1000);
        }

        void ReleaseAll()
        {
            for (auto& [page, guest] : s_watchPages)
            {
                if (guest >= 0xA0000000u)
                    RestorePhysicalPageProtection((guest - 0xA0000000u) / 0x1000);
                else
                    os::ProtectReadWrite(reinterpret_cast<void*>(page), 0x1000);
            }
        }

        std::string Describe(uintptr_t pc)
        {
            HMODULE mod = nullptr;
            char text[96];
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(pc), &mod) &&
                mod == GetModuleHandleW(nullptr))
                std::snprintf(text, sizeof(text), "exe:%llx", (unsigned long long)(pc - uintptr_t(mod)));
            else
                std::snprintf(text, sizeof(text), "?%llx", (unsigned long long)pc);
            return text;
        }

        void WatchWriters(const std::vector<Candidate>& yaw, const std::vector<Candidate>& pitch, FILE* report)
        {
            {
                std::lock_guard lock(s_watchLock);
                s_watchPages.clear();
                s_watchAddrs.clear();
                s_writers.clear();
                auto add = [&](const std::vector<Candidate>& list) {
                    for (size_t i = 0; i < list.size() && i < 150; i++)
                    {
                        uint32_t a = list[i].addr;
                        s_watchAddrs[a] = uint32_t(i);
                        uint32_t gpage = a & ~0xFFFu;
                        if (a >= 0xA0000000u)
                        {
                            uint8_t* views[3];
                            PhysicalPageViews((a - 0xA0000000u) / 0x1000, views);
                            for (auto* v : views)
                                if (v)
                                    s_watchPages[uintptr_t(v)] = gpage;
                        }
                        else
                            s_watchPages[uintptr_t(g_mem.base + gpage)] = gpage;
                    }
                };
                add(yaw);
                add(pitch);
                os::SetDiagnosticWatch(OnDiagFault, OnDiagStep);
                s_watching = true;
                for (auto& [page, guest] : s_watchPages)
                    os::ProtectReadOnly(reinterpret_cast<void*>(page), 0x1000);
            }
            // Turn both ways meanwhile so the angles are written.
            Hold(32767, 0, 0.6);
            Hold(0, 32767, 0.6);
            Hold(-32767, 0, 0.6);
            Hold(0, -32767, 0.6);
            Hold(0, 0, 0.6);
            {
                std::lock_guard lock(s_watchLock);
                s_watching = false;
                ReleaseAll();
            }
            // The handler stays installed: a write that faulted just before the pages
            // were released retries instead of crashing (OnDiagFault returns 2).

            std::fprintf(report, "\n== Writers (address: host instruction x count; resolve exe:offsets with the map) ==\n");
            for (auto& [key, count] : s_writers)
                std::fprintf(report, "%08X: %s x%u\n", key.first, Describe(key.second).c_str(), count);
            Log("Camera scan: %zu (candidate, instruction) writer pairs recorded", s_writers.size());
        }

        void Run()
        {
            Log("Camera scan: started - don't touch the controls for about 15 s");
            FILE* report = std::fopen("camscan.txt", "w");
            if (!report)
            {
                s_running = false;
                return;
            }
            std::fprintf(report, "# WET Recomp camera scan (F7)\n");
            auto yaw = ScanAxis(false, report);
            auto pitch = ScanAxis(true, report);
            WatchWriters(yaw, pitch, report);
            SetRightStickOverride(false);
            std::fclose(report);
            Log("Camera scan: finished - results in camscan.txt");
            s_running = false;
        }
        void RunPadReaders()
        {
            uint32_t pad = padaim::PadAddress();
            if (!pad)
            {
                Log("Pad readers: no pad object yet - start gameplay first");
                s_running = false;
                return;
            }
            Log("Pad readers: watching the pad object at %08X for 4 s - the stick is driven, don't touch it", pad);
            {
                std::lock_guard lock(s_watchLock);
                s_padReaders.clear();
                s_padGuest = pad;
                s_padPages.clear();
                if (pad >= 0xA0000000u)
                {
                    uint8_t* views[3];
                    PhysicalPageViews((pad - 0xA0000000u) / 0x1000, views);
                    for (auto* v : views)
                        if (v)
                            s_padPages.push_back(uintptr_t(v));
                }
                else
                    s_padPages.push_back(uintptr_t(g_mem.base + (pad & ~0xFFFu)));
                os::SetDiagnosticWatch(OnDiagFault, OnDiagStep);
                s_padWatching = true;
                for (auto page : s_padPages)
                {
                    DWORD old;
                    VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_NOACCESS, &old);
                }
            }
            padaim::LogAxisReaders(true);
            Hold(16000, 0, 1.0);
            Hold(-16000, 0, 1.0);
            Hold(0, 16000, 1.0);
            Hold(0, -16000, 1.0);
            {
                std::lock_guard lock(s_watchLock);
                s_padWatching = false;
                if (pad >= 0xA0000000u)
                    RestorePhysicalPageProtection((pad - 0xA0000000u) / 0x1000);
                else
                    for (auto page : s_padPages)
                    {
                        DWORD old;
                        VirtualProtect(reinterpret_cast<void*>(page), 0x1000, PAGE_READWRITE, &old);
                    }
            }
            padaim::LogAxisReaders(false);
            SetRightStickOverride(false);
            FILE* report = std::fopen("padreaders.txt", "w");
            if (report)
            {
                std::fprintf(report, "# GetAxis (sub_82BF1CC0) callers: lr, axis (2 = right X, 3 = right Y) x count\n");
                for (auto& c : padaim::AxisReaders())
                    std::fprintf(report, "lr %08X axis %u x%u\n", c.lr, c.axis, c.count);
                std::fprintf(report, "# Pad object %08X: offset, host instruction x count (resolve exe: with the map)\n", pad);
                for (auto& [key, count] : s_padReaders)
                    std::fprintf(report, "+%u: %s x%u\n", key.first, Describe(key.second).c_str(), count);
                std::fclose(report);
            }
            Log("Pad readers: %zu (offset, instruction) pairs in padreaders.txt", s_padReaders.size());
            s_running = false;
        }
    }

    void StartPadReaders()
    {
        if (s_running.exchange(true))
            return;
        std::thread(RunPadReaders).detach();
    }

    void Start()
    {
        if (s_running.exchange(true))
            return;
        std::thread(Run).detach();
    }
#else
    void Start() {}
    void StartPadReaders() {}
#endif
}
