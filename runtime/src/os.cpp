#include "os.h"
#include "log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
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

    static LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS info)
    {
        auto* rec = info->ExceptionRecord;
        switch (rec->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            ReportFault(rec->ExceptionInformation[0] == 1 ? "Write access violation" : "Read access violation",
                        uintptr_t(rec->ExceptionInformation[1]));
            break;
        case EXCEPTION_STACK_OVERFLOW:     LogFatal("Host stack overflow"); LogCrashContext(); break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_BREAKPOINT:         LogFatal("Trap/breakpoint at host %p", rec->ExceptionAddress); LogCrashContext(); break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: LogFatal("Integer divide by zero at host %p", rec->ExceptionAddress); LogCrashContext(); break;
        default: break;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void InstallCrashHandler(uint8_t* guestBase, uint64_t guestSize)
    {
        s_guestBase = guestBase;
        s_guestSize = guestSize;
        AddVectoredExceptionHandler(1, VectoredHandler);
    }
#else
    void* Reserve(size_t size)
    {
        void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : p;
    }
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
    uint32_t CurrentHostThreadId() { return uint32_t(syscall(SYS_gettid)); }

    static void SignalHandler(int sig, siginfo_t* info, void*)
    {
        switch (sig)
        {
        case SIGSEGV: ReportFault("Access violation", uintptr_t(info->si_addr)); break;
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
