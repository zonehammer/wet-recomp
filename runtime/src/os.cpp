#include "os.h"
#include "log.h"

#include <chrono>
#include <cstdio>
#include <vector>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace wet::os
{
    static uint8_t* s_guestBase = nullptr;
    static uint64_t s_guestSize = 0;

    static void ReportFault(const char* what, uintptr_t addr)
    {
        if (s_guestBase && addr >= uintptr_t(s_guestBase) && addr < uintptr_t(s_guestBase) + s_guestSize)
            LogFatal("%s at guest address 0x%08X (host %p)", what, uint32_t(addr - uintptr_t(s_guestBase)), (void*)addr);
        else
            LogFatal("%s at host address %p", what, (void*)addr);
        LogCrashContext();
    }

#ifdef _WIN32
    void* Reserve(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

    static HANDLE s_physicalSection = nullptr;
    static WriteFaultHandler s_writeFaultHandler = nullptr;

    // Placeholder APIs (Windows 10 1803+), loaded at run time so older headers work.
    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

    uint8_t* ReserveWithAliases(uint64_t total, const AliasView* views, int viewCount, uint64_t physicalSize,
                                const char** failureReason)
    {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        auto va2 = kb ? reinterpret_cast<VirtualAlloc2Fn>(reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2"))) : nullptr;
        auto mv3 = kb ? reinterpret_cast<MapViewOfFile3Fn>(reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3"))) : nullptr;
        if (!va2 || !mv3)
        {
            *failureReason = "VirtualAlloc2/MapViewOfFile3 unavailable (needs Windows 10 1803 or later)";
            return nullptr;
        }

        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                            DWORD(physicalSize >> 32), DWORD(physicalSize), nullptr);
        if (!section)
        {
            *failureReason = "CreateFileMapping failed";
            return nullptr;
        }

        HANDLE proc = GetCurrentProcess();
        auto* base = static_cast<uint8_t*>(va2(proc, nullptr, SIZE_T(total), MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                               PAGE_NOACCESS, nullptr, 0));
        if (!base)
        {
            CloseHandle(section);
            *failureReason = "VirtualAlloc2 placeholder reservation failed";
            return nullptr;
        }

        // Segments in address order: views and the plain gaps between them.
        struct Seg { uint64_t start, size; bool view; };
        std::vector<Seg> segs;
        uint64_t cursor = 0;
        for (int i = 0; i < viewCount; i++)
        {
            if (views[i].offset > cursor)
                segs.push_back({ cursor, views[i].offset - cursor, false });
            segs.push_back({ views[i].offset, views[i].size, true });
            cursor = views[i].offset + views[i].size;
        }
        if (cursor < total)
            segs.push_back({ cursor, total - cursor, false });

        bool ok = true;
        // Split the placeholder into one placeholder per segment.
        for (size_t i = 0; ok && i + 1 < segs.size(); i++)
            ok = VirtualFree(base + segs[i].start, SIZE_T(segs[i].size), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
        if (!ok)
            *failureReason = "splitting the placeholder failed (alignment?)";
        // Replace each placeholder: views map the section, gaps become normal reservations.
        for (size_t i = 0; ok && i < segs.size(); i++)
        {
            void* r = segs[i].view
                ? mv3(section, proc, base + segs[i].start, 0, SIZE_T(segs[i].size), MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)
                : va2(proc, base + segs[i].start, SIZE_T(segs[i].size), MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
            if (!r)
            {
                ok = false;
                *failureReason = segs[i].view ? "MapViewOfFile3 failed (alignment?)" : "VirtualAlloc2 replace failed";
            }
        }
        if (ok)
            s_physicalSection = section; // kept for MapPhysicalAgain
        else
            CloseHandle(section);
        if (!ok)
        {
            // Best effort cleanup; the caller may retry with a different layout.
            for (auto& sg : segs)
            {
                if (sg.view) UnmapViewOfFile(base + sg.start);
                VirtualFree(base + sg.start, 0, MEM_RELEASE);
            }
            VirtualFree(base, 0, MEM_RELEASE);
            return nullptr;
        }
        return base;
    }

    void Release(uint8_t* base, uint64_t total) { VirtualFree(base, 0, MEM_RELEASE); }

    uint8_t* MapPhysicalAgain(uint64_t size)
    {
        if (!s_physicalSection)
            return nullptr;
        return static_cast<uint8_t*>(MapViewOfFile(s_physicalSection, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(size)));
    }
    bool ProtectReadOnly(void* p, size_t size)
    {
        DWORD old;
        return VirtualProtect(p, size, PAGE_READONLY, &old) != 0;
    }
    bool ProtectReadWrite(void* p, size_t size)
    {
        DWORD old;
        return VirtualProtect(p, size, PAGE_READWRITE, &old) != 0;
    }
    void SetWriteFaultHandler(WriteFaultHandler handler) { s_writeFaultHandler = handler; }

    // First in line for every exception: resolves write-watch faults.
    static DiagFaultHandler s_diagFault = nullptr;
    static DiagStepHandler s_diagStep = nullptr;
    static thread_local bool t_diagStepping = false;
    void SetDiagnosticWatch(DiagFaultHandler fault, DiagStepHandler step)
    {
        s_diagStep = step;
        s_diagFault = fault;
    }

    static LONG WINAPI WatchVectoredHandler(PEXCEPTION_POINTERS info)
    {
        auto* rec = info->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_diagStepping)
        {
            t_diagStepping = false;
            if (s_diagStep)
                s_diagStep();
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (auto diag = s_diagFault; diag && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            rec->NumberParameters >= 2 && rec->ExceptionInformation[0] <= 1)   // read or write
        {
            int r = diag(uintptr_t(rec->ExceptionInformation[1]), uintptr_t(rec->ExceptionAddress));
            if (r == 1)
            {
                t_diagStepping = true;
                info->ContextRecord->EFlags |= 0x100;   // trap after this one instruction
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (r == 2)
                return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 &&
            rec->ExceptionInformation[0] == 1 && s_writeFaultHandler &&
            s_writeFaultHandler(uintptr_t(rec->ExceptionInformation[1])))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    bool Commit(void* p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
    void Decommit(void* p, size_t size) { VirtualFree(p, size, MEM_DECOMMIT); }

    struct ThreadStart { ThreadFn fn; void* arg; };
    static DWORD WINAPI ThreadThunk(LPVOID p)
    {
        ThreadStart s = *static_cast<ThreadStart*>(p);
        delete static_cast<ThreadStart*>(p);
        s.fn(s.arg);
        return 0;
    }

    bool StartThread(ThreadFn fn, void* arg, size_t stackSize)
    {
        HANDLE h = CreateThread(nullptr, stackSize, ThreadThunk, new ThreadStart{ fn, arg },
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h)
            return false;
        CloseHandle(h);
        return true;
    }

    void ExitCurrentThread() { ExitThread(0); }
    uint32_t CurrentHostThreadId() { return GetCurrentThreadId(); }

    struct WakeEvent { HANDLE h; };
    WakeEvent* CreateWakeEvent() { return new WakeEvent{ CreateEventW(nullptr, FALSE, FALSE, nullptr) }; }
    void SignalWake(WakeEvent* e) { SetEvent(e->h); }
    bool WaitWake(WakeEvent* e, uint32_t timeoutUs)
    {
        DWORD ms = timeoutUs / 1000;
        if (!ms && timeoutUs) ms = 1;
        return WaitForSingleObject(e->h, ms) == WAIT_OBJECT_0;
    }
    void RaiseTimerResolution()
    {
        // winmm's timeBeginPeriod, loaded dynamically so nothing else needs linking.
        if (HMODULE m = LoadLibraryW(L"winmm.dll"))
            if (auto fn = reinterpret_cast<UINT(WINAPI*)(UINT)>((void*)GetProcAddress(m, "timeBeginPeriod")))
                fn(1);
    }

    // Called only for exceptions nothing else handled, i.e. real crashes.
    static LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS info)
    {
        auto* rec = info->ExceptionRecord;
        uintptr_t pc = uintptr_t(rec->ExceptionAddress);
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &mod);
        char modName[MAX_PATH] = "?";
        if (mod)
            GetModuleFileNameA(mod, modName, MAX_PATH);
        const char* shortName = modName;
        for (const char* c = modName; *c; c++)
            if (*c == '\\' || *c == '/')
                shortName = c + 1;

        switch (rec->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            ReportFault(rec->ExceptionInformation[0] == 1 ? "Write access violation" : "Read access violation",
                        uintptr_t(rec->ExceptionInformation[1]));
            break;
        case EXCEPTION_STACK_OVERFLOW:     LogFatal("Host stack overflow"); LogCrashContext(); break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_BREAKPOINT:         LogFatal("Trap/breakpoint"); LogCrashContext(); break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: LogFatal("Integer divide by zero"); LogCrashContext(); break;
        default: LogFatal("Unhandled exception 0x%08lX", rec->ExceptionCode); LogCrashContext(); break;
        }
        LogFatal("  faulting code: %s+0x%llX", shortName,
                 (unsigned long long)(mod ? pc - uintptr_t(mod) : pc));
        LogFlush();
        ShowFatalMessage("WET Recomp crashed.\n\nThe details are at the end of wet.log, next to WetRecomp.exe.");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ShowFatalMessage(const char* text)
    {
        if (!GetConsoleWindow() && !std::getenv("CI") && !std::getenv("WET_NO_DIALOGS"))
            MessageBoxA(nullptr, text, "WET Recomp", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }

    void OpenConsole()
    {
        if (GetConsoleWindow() || !AllocConsole())
            return;
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }

    void InstallCrashHandler(uint8_t* guestBase, uint64_t guestSize)
    {
        s_guestBase = guestBase;
        s_guestSize = guestSize;
        SetUnhandledExceptionFilter(UnhandledFilter);
        AddVectoredExceptionHandler(1, WatchVectoredHandler);
    }
#else
    void* Reserve(size_t size)
    {
        void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : p;
    }
    static int s_physicalFd = -1;
    static WriteFaultHandler s_writeFaultHandler = nullptr;

    uint8_t* ReserveWithAliases(uint64_t total, const AliasView* views, int viewCount, uint64_t physicalSize,
                                const char** failureReason)
    {
        int fd = memfd_create("wet-physical", 0);
        if (fd < 0 || ftruncate(fd, off_t(physicalSize)) != 0)
        {
            *failureReason = "memfd_create/ftruncate failed";
            return nullptr;
        }
        auto* base = static_cast<uint8_t*>(Reserve(size_t(total)));
        if (!base)
        {
            close(fd);
            *failureReason = "address space reservation failed";
            return nullptr;
        }
        for (int i = 0; i < viewCount; i++)
        {
            void* r = mmap(base + views[i].offset, size_t(views[i].size), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, 0);
            if (r == MAP_FAILED)
            {
                close(fd);
                munmap(base, size_t(total));
                *failureReason = "mmap of physical view failed";
                return nullptr;
            }
        }
        s_physicalFd = fd; // kept for MapPhysicalAgain
        return base;
    }

    uint8_t* MapPhysicalAgain(uint64_t size)
    {
        if (s_physicalFd < 0)
            return nullptr;
        void* p = mmap(nullptr, size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, s_physicalFd, 0);
        return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    }
    bool ProtectReadOnly(void* p, size_t size) { return mprotect(p, size, PROT_READ) == 0; }
    bool ProtectReadWrite(void* p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }
    void SetWriteFaultHandler(WriteFaultHandler handler) { s_writeFaultHandler = handler; }

    void Release(uint8_t* base, uint64_t total) { munmap(base, size_t(total)); }

    bool Commit(void* p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }
    void Decommit(void* p, size_t size)
    {
        madvise(p, size, MADV_DONTNEED);
        mprotect(p, size, PROT_NONE);
    }

    struct ThreadStart { ThreadFn fn; void* arg; };
    static void* ThreadThunk(void* p)
    {
        ThreadStart s = *static_cast<ThreadStart*>(p);
        delete static_cast<ThreadStart*>(p);
        s.fn(s.arg);
        return nullptr;
    }

    bool StartThread(ThreadFn fn, void* arg, size_t stackSize)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stackSize);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        int r = pthread_create(&t, &attr, ThreadThunk, new ThreadStart{ fn, arg });
        pthread_attr_destroy(&attr);
        return r == 0;
    }

    void ExitCurrentThread() { pthread_exit(nullptr); }
    void ShowFatalMessage(const char*) {}
    void SetDiagnosticWatch(DiagFaultHandler, DiagStepHandler) {}
    void OpenConsole() {}
    uint32_t CurrentHostThreadId() { return uint32_t(syscall(SYS_gettid)); }

    struct WakeEvent { int fd; };
    WakeEvent* CreateWakeEvent() { return new WakeEvent{ eventfd(0, EFD_NONBLOCK) }; }
    void SignalWake(WakeEvent* e)
    {
        uint64_t one = 1;
        (void)!write(e->fd, &one, sizeof(one));   // async-signal-safe
    }
    bool WaitWake(WakeEvent* e, uint32_t timeoutUs)
    {
        pollfd p{ e->fd, POLLIN, 0 };
        timespec ts{ time_t(timeoutUs / 1000000), long(timeoutUs % 1000000) * 1000 };
        if (ppoll(&p, 1, &ts, nullptr) <= 0)
            return false;
        uint64_t v;
        (void)!read(e->fd, &v, sizeof(v));
        return true;
    }
    void RaiseTimerResolution() {}

    static void SignalHandler(int sig, siginfo_t* info, void*)
    {
        switch (sig)
        {
        case SIGSEGV:
            // Write-watched pages are read-only, so any fault on them is a write.
            if (s_writeFaultHandler && s_writeFaultHandler(uintptr_t(info->si_addr)))
                return;
            ReportFault("Access violation", uintptr_t(info->si_addr));
            break;
        case SIGBUS:  ReportFault("Bus error", uintptr_t(info->si_addr)); break;
        case SIGILL:
        case SIGTRAP: LogFatal("Trap/illegal instruction at host %p", info->si_addr); LogCrashContext(); break;
        case SIGFPE:  LogFatal("Arithmetic fault at host %p", info->si_addr); LogCrashContext(); break;
        }
        signal(sig, SIG_DFL);   // returning re-executes the faulting instruction and terminates
    }

    void InstallCrashHandler(uint8_t* guestBase, uint64_t guestSize)
    {
        s_guestBase = guestBase;
        s_guestSize = guestSize;
        // Faults may be stack overflows, so handle them on an alternate stack.
        static uint8_t altStack[1 << 16];
        stack_t ss{};
        ss.ss_sp = altStack;
        ss.ss_size = sizeof(altStack);
        sigaltstack(&ss, nullptr);
        struct sigaction sa{};
        sa.sa_sigaction = SignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE })
            sigaction(sig, &sa, nullptr);
    }
#endif

    void YieldThread() { std::this_thread::yield(); }
    void SleepNs(uint64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }

    uint64_t MonotonicNs()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    uint64_t SystemTimeFileTime()
    {
        // 1601-01-01 -> 1970-01-01 is 11644473600 seconds.
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return uint64_t(ns / 100) + 116444736000000000ull;
    }

    void ExitProcess(int code)
    {
        LogFlush();
        std::_Exit(code);
    }
}
