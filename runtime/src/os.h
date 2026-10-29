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
    bool Commit(void* p, size_t size);          // make read/write, zero-filled when fresh
    void Decommit(void* p, size_t size);        // back to no-access, contents discarded

    // Threads with an explicit host stack size (guest code recurses deeply).
    using ThreadFn = void (*)(void*);
    bool StartThread(ThreadFn fn, void* arg, size_t stackSize);
    [[noreturn]] void ExitCurrentThread();
    void YieldThread();
    void SleepNs(uint64_t ns);
    uint32_t CurrentHostThreadId();

    // Time
    uint64_t MonotonicNs();
    uint64_t SystemTimeFileTime();              // 100 ns units since 1601-01-01 UTC

    // Fatal-fault reporting (access violations etc. are translated to guest addresses).
    void InstallCrashHandler(uint8_t* guestBase, uint64_t guestSize);

    [[noreturn]] void ExitProcess(int code);
}
