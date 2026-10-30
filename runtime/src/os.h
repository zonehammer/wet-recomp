// Thin platform layer: Windows for the real target, Linux/POSIX for CI and testing.
#pragma once
#include <cstddef>
#include <cstdint>

namespace wet::os
{
    // Address space
    void* Reserve(size_t size);                 // PROT_NONE / MEM_RESERVE

    // Reserves `total` bytes of address space in which every `views[i]` range maps the
    // SAME `physicalSize` bytes of shared memory (offset 0), so writes through one
    // view are visible through the others. Everything else is plain reserved space
    // usable with Commit/Decommit. Returns nullptr on failure.
    struct AliasView { uint64_t offset, size; };
    uint8_t* ReserveWithAliases(uint64_t total, const AliasView* views, int viewCount, uint64_t physicalSize,
                                const char** failureReason);
    void Release(uint8_t* base, uint64_t total);
    // One more mapping of the shared physical memory set up by ReserveWithAliases,
    // anywhere in the address space (never protected; the renderer's own access path).
    uint8_t* MapPhysicalAgain(uint64_t size);
    // Page protection on committed or mapped memory.
    bool ProtectReadOnly(void* p, size_t size);
    bool ProtectReadWrite(void* p, size_t size);

    // Called on a write fault at `hostAddress` before the crash handler; returns true
    // if it made the access valid (the faulting instruction is then retried).
    using WriteFaultHandler = bool (*)(uintptr_t hostAddress);
    void SetWriteFaultHandler(WriteFaultHandler handler);
    bool Commit(void* p, size_t size);          // make read/write, zero-filled when fresh
    void Decommit(void* p, size_t size);        // back to no-access, contents discarded

    // Threads with an explicit host stack size (guest code recurses deeply).
    using ThreadFn = void (*)(void*);
    bool StartThread(ThreadFn fn, void* arg, size_t stackSize);
    [[noreturn]] void ExitCurrentThread();
    void YieldThread();
    void SleepNs(uint64_t ns);
    uint32_t CurrentHostThreadId();

    // Auto-reset wake-up event that can be signalled from a fault handler.
    struct WakeEvent;
    WakeEvent* CreateWakeEvent();
    void SignalWake(WakeEvent* e);
    // Returns true if signalled, false on timeout.
    bool WaitWake(WakeEvent* e, uint32_t timeoutUs);

    // 1 ms scheduler/timer resolution (Windows' default is 15.6 ms, which makes every
    // short sleep in the runtime and the game 15 ms long).
    void RaiseTimerResolution();

    // Time
    uint64_t MonotonicNs();
    uint64_t SystemTimeFileTime();              // 100 ns units since 1601-01-01 UTC

    // Fatal-fault reporting (access violations etc. are translated to guest addresses).
    void InstallCrashHandler(uint8_t* guestBase, uint64_t guestSize);

    [[noreturn]] void ExitProcess(int code);
    // A message box for fatal errors when there is no console to read (the
    // Windows build is a windowed app); no-op elsewhere or with a console.
    void ShowFatalMessage(const char* text);
    // Opens a console window for the log (--console / WET_CONSOLE=1).
    void OpenConsole();
}
