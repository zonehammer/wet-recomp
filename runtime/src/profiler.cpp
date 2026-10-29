// Sampling profiler for performance work (Windows).
//
// F10 (or WET_PROFILE=<seconds>[,<start delay>]) samples every thread of the
// process about 1000 times a second: each thread is suspended, its instruction
// pointer and a few unwound return addresses are recorded, and it is resumed.
// On stop, profile.txt gets per-thread totals and the most frequent call
// chains as module-relative addresses; tools/resolve_profile.py turns those
// into function names with the linker map (build/runtime/WetRecomp.map), where
// recompiled guest functions appear as __imp__sub_XXXXXXXX.
#include "profiler.h"
#include "kernel/threads.h"
#include "log.h"
#include "memory.h"
#include "os.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#endif

namespace wet::profiler
{
#ifdef _WIN32
    namespace
    {
        constexpr int kFrames = 8;
        std::atomic<bool> s_running{ false };
        std::mutex s_lock;

        struct Chain
        {
            uint64_t pc[kFrames];
            uint32_t tid;
            bool operator==(const Chain& o) const
            {
                return tid == o.tid && std::memcmp(pc, o.pc, sizeof(pc)) == 0;
            }
        };
        struct ChainHash
        {
            size_t operator()(const Chain& c) const
            {
                uint64_t h = c.tid * 0x9E3779B97F4A7C15ull;
                for (uint64_t p : c.pc) h = (h ^ p) * 0x100000001B3ull;
                return size_t(h);
            }
        };

        // Module-relative address as "module+offset" with the preferred image base
        // added for the main executable, so it matches the linker map.
        std::string Describe(uint64_t pc)
        {
            if (!pc)
                return "-";
            HMODULE mod = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(pc), &mod) || !mod)
            {
                char b[32];
                std::snprintf(b, sizeof(b), "?%llx", (unsigned long long)pc);
                return b;
            }
            char path[MAX_PATH] = {};
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            name = name ? name + 1 : path;
            uint64_t off = pc - uint64_t(mod);
            char b[MAX_PATH + 40];
            if (mod == GetModuleHandleW(nullptr))
                std::snprintf(b, sizeof(b), "exe:%llx", (unsigned long long)off);   // RVA, as in the map
            else
                std::snprintf(b, sizeof(b), "%s+%llx", name, (unsigned long long)off);
            return b;
        }

        std::string ThreadName(uint32_t tid, HANDLE h)
        {
            std::string n = GuestThreadNameForHostTid(tid);
            if (!n.empty())
                return "guest " + n;
            using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
            static auto fn = reinterpret_cast<GetDesc>(
                (void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
            PWSTR w = nullptr;
            if (fn && SUCCEEDED(fn(h, &w)) && w)
            {
                char b[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b) - 1, nullptr, nullptr);
                LocalFree(w);
                if (b[0])
                    return b;
            }
            return "host";
        }

        void Unwind(CONTEXT ctx, uint64_t* out)
        {
            for (int i = 0; i < kFrames; i++)
            {
                out[i] = ctx.Rip;
                if (!ctx.Rip)
                    break;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    // Leaf function: the return address is at [rsp].
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(reinterpret_cast<void*>(ctx.Rsp), &mbi, sizeof(mbi)) ||
                        mbi.State != MEM_COMMIT)
                        break;
                    ctx.Rip = *reinterpret_cast<uint64_t*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
        }

        void SamplerMain(void* arg)
        {
            double seconds = *static_cast<double*>(arg);
            delete static_cast<double*>(arg);
            std::unordered_map<Chain, uint32_t, ChainHash> chains;
            std::map<uint32_t, uint64_t> perThread;
            std::map<uint32_t, std::string> names;
            std::vector<std::pair<uint32_t, HANDLE>> threads;
            uint32_t self = GetCurrentThreadId();
            uint64_t samples = 0, rounds = 0;
            uint64_t faults0, protects0;
            WatchStats(faults0, protects0);
            uint64_t t0 = os::MonotonicNs(), lastList = 0;
            timeBeginPeriod(1);
            while (s_running.load())
            {
                uint64_t now = os::MonotonicNs();
                if (seconds > 0 && double(now - t0) / 1e9 >= seconds)
                    break;
                if (now - lastList > 500'000'000ull)
                {
                    lastList = now;
                    for (auto& [tid, h] : threads)
                        ::CloseHandle(h);
                    threads.clear();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    THREADENTRY32 te{ sizeof(te) };
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                    {
                        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
                            continue;
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                              FALSE, te.th32ThreadID);
                        if (h)
                        {
                            threads.push_back({ te.th32ThreadID, h });
                            if (!names.count(te.th32ThreadID))
                                names[te.th32ThreadID] = ThreadName(te.th32ThreadID, h);
                        }
                    }
                    ::CloseHandle(snap);
                }
                for (auto& [tid, h] : threads)
                {
                    if (SuspendThread(h) == DWORD(-1))
                        continue;
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    Chain c{};
                    c.tid = tid;
                    bool got = GetThreadContext(h, &ctx) != 0;
                    if (got)
                        Unwind(ctx, c.pc);
                    ResumeThread(h);
                    if (!got)
                        continue;
                    chains[c]++;
                    perThread[tid]++;
                    samples++;
                }
                rounds++;
                Sleep(1);
            }
            timeEndPeriod(1);
            double elapsed = double(os::MonotonicNs() - t0) / 1e9;
            uint64_t faults1, protects1;
            WatchStats(faults1, protects1);

            FILE* f = std::fopen("profile.txt", "w");
            if (f)
            {
                std::fprintf(f, "# WET profile: %.1f s, %llu rounds (%.0f/s), %llu samples\n", elapsed,
                             (unsigned long long)rounds, rounds / elapsed, (unsigned long long)samples);
                std::fprintf(f, "# write-watch faults %.0f/s, watch calls %.0f/s\n",
                             (faults1 - faults0) / elapsed, (protects1 - protects0) / elapsed);
                std::fprintf(f, "# threads: samples tid name\n");
                for (auto& [tid, n] : perThread)
                    std::fprintf(f, "T %llu %u %s\n", (unsigned long long)n, tid, names[tid].c_str());
                std::vector<std::pair<uint32_t, const Chain*>> sorted;
                for (auto& [c, n] : chains)
                    sorted.push_back({ n, &c });
                std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
                std::fprintf(f, "# chains: count tid frame0 (innermost) ... frame%d\n", kFrames - 1);
                std::map<uint64_t, std::string> described;
                size_t limit = std::min<size_t>(sorted.size(), 20000);
                for (size_t i = 0; i < limit; i++)
                {
                    std::fprintf(f, "C %u %u", sorted[i].first, sorted[i].second->tid);
                    for (uint64_t pc : sorted[i].second->pc)
                    {
                        auto it = described.find(pc);
                        if (it == described.end())
                            it = described.emplace(pc, Describe(pc)).first;
                        std::fprintf(f, " %s", it->second.c_str());
                    }
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
            for (auto& [tid, h] : threads)
                ::CloseHandle(h);
            Log("Profiler: stopped after %.1f s, %llu samples; wrote profile.txt", elapsed, (unsigned long long)samples);
            s_running = false;
        }
    }

    void Start(double seconds)
    {
        std::lock_guard lock(s_lock);
        if (s_running.exchange(true))
            return;
        if (seconds > 0)
            Log("Profiler: sampling every thread for %.0f s", seconds);
        else
            Log("Profiler: sampling every thread (F10 again to stop)");
        if (!os::StartThread(SamplerMain, new double(seconds), 1 << 20))
            s_running = false;
    }

    void Toggle()
    {
        if (s_running.load())
            s_running = false;
        else
            Start(0);
    }

    void StartFromEnv()
    {
        const char* v = std::getenv("WET_PROFILE");
        if (!v)
            return;
        double seconds = std::atof(v), delay = 0;
        if (const char* comma = std::strchr(v, ','))
            delay = std::atof(comma + 1);
        if (seconds <= 0)
            seconds = 30;
        struct Args { double seconds, delay; };
        os::StartThread([](void* p) {
            Args a = *static_cast<Args*>(p);
            delete static_cast<Args*>(p);
            os::SleepNs(uint64_t(a.delay * 1e9));
            Start(a.seconds);
        }, new Args{ seconds, delay }, 1 << 16);
    }
#else
    void Start(double) {}
    void Toggle() {}
    void StartFromEnv() {}
#endif
}
