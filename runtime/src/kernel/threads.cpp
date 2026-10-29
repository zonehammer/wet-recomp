// Guest threads, PCR/TLS setup and thread-related imports.
#include "threads.h"
#include "kernel.h"
#include "os.h"

#include <vector>

namespace wet
{
    static thread_local std::shared_ptr<GuestThread> t_current;

    static std::mutex s_threadLock;
    static uint32_t s_nextThreadId = 0x10;

    // XEX TLS directory
    static uint32_t s_tlsSlotCount = 64, s_tlsRawAddr = 0, s_tlsDataSize = 0, s_tlsRawSize = 0;
    static std::vector<bool> s_tlsUsed;

    static constexpr size_t kHostStackSize = 16 << 20;      // guest frames become host frames
    static constexpr size_t kHostMainStackSize = 32 << 20;

    GuestThread* CurrentThread() { return t_current.get(); }
    std::shared_ptr<GuestThread> CurrentThreadPtr() { return t_current; }

    void SetCurrentImport(const char* name)
    {
        if (t_current)
            t_current->currentImport = name;
    }

    void InitThreading(uint32_t tlsSlotCount, uint32_t tlsRawAddr, uint32_t tlsDataSize, uint32_t tlsRawSize)
    {
        s_tlsSlotCount = tlsSlotCount ? tlsSlotCount : 64;
        s_tlsRawAddr = tlsRawAddr;
        s_tlsDataSize = tlsDataSize;
        s_tlsRawSize = tlsRawSize;
        s_tlsUsed.assign(s_tlsSlotCount, false);
        Log("TLS: %u slots, static data 0x%X bytes (raw 0x%X at 0x%08X)", s_tlsSlotCount, tlsDataSize, tlsRawSize, tlsRawAddr);
    }

    // Layout notes (from Xenia's X_KPCR / X_KTHREAD; only the fields games read are filled):
    //   PCR+0x000 tls_ptr   PCR+0x030 pcr_ptr   PCR+0x070 stack_base   PCR+0x074 stack_end
    //   PCR+0x100 current_thread                PCR+0x10C current_cpu (u8)
    //   KTHREAD+0x00 DISPATCHER_HEADER (type 6)  +0x5C stack_base  +0x60 stack_limit  +0x68 tls_address
    static void SetupGuestThreadMemory(GuestThread& t, uint32_t stackSize)
    {
        t.stackSize = (stackSize + 0xFFF) & ~0xFFFu;
        if (t.stackSize < 0x4000)
            t.stackSize = 0x4000;
        t.stackLimit = SystemAlloc(t.stackSize, 0x1000);
        t.stackBase = t.stackLimit + t.stackSize;

        t.pcr = SystemAlloc(0x2D8, 0x1000);
        t.kthread = SystemAlloc(0xAB0, 0x10);

        uint32_t tlsTotal = s_tlsDataSize + s_tlsSlotCount * 4;
        t.tlsStatic = SystemAlloc(tlsTotal ? tlsTotal : 4, 0x10);
        if (s_tlsRawAddr && s_tlsRawSize)
            std::memcpy(HostPtr(t.tlsStatic), HostPtr(s_tlsRawAddr), s_tlsRawSize);
        t.tlsSlots = t.tlsStatic + s_tlsDataSize;

        St32(t.pcr + 0x000, t.tlsStatic);
        St32(t.pcr + 0x030, t.pcr);
        St32(t.pcr + 0x070, t.stackBase);
        St32(t.pcr + 0x074, t.stackLimit);
        St32(t.pcr + 0x100, t.kthread);
        St8 (t.pcr + 0x10C, uint8_t(t.id % 6));

        St8 (t.kthread + 0x00, kDispThread);
        St8 (t.kthread + 0x02, 0xAB0 / 4);
        St32(t.kthread + 0x5C, t.stackBase);
        St32(t.kthread + 0x60, t.stackLimit);
        St32(t.kthread + 0x68, t.tlsStatic);
    }

    void CallGuest(PPCContext& ctx, uint32_t address)
    {
        if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE)
            WetIndirectCallMiss(ctx, g_mem.base, address);
        PPCFunc* fn = PPC_LOOKUP_FUNC(g_mem.base, address);
        if (!fn)
            WetIndirectCallMiss(ctx, g_mem.base, address);
        fn(ctx, g_mem.base);
    }

    static void InitContext(GuestThread& t)
    {
        t.ctx = new PPCContext{};
        t.ctx->r1.u64 = t.stackBase - 0x80;
        t.ctx->r13.u64 = t.pcr;
        t.ctx->fpscr.loadFromHost();
    }

    static void WaitWhileSuspended(GuestThread& t)
    {
        std::unique_lock lock(DispatchLock());
        while (t.suspendCount > 0)
            DispatchCv().wait(lock);
    }

    [[noreturn]] void ExitGuestThread(uint32_t exitCode)
    {
        auto t = t_current;
        {
            std::lock_guard lock(DispatchLock());
            t->exited = true;
            t->exitCode = exitCode;
        }
        DispatchNotify();
        Log("Thread %u exited (code 0x%X)", t->id, exitCode);
        os::ExitCurrentThread();
    }

    static void GuestThreadEntry(void* arg)
    {
        auto* holder = static_cast<std::shared_ptr<GuestThread>*>(arg);
        t_current = *holder;
        delete holder;
        GuestThread& t = *t_current;

        WaitWhileSuspended(t);
        PPCContext& ctx = *t.ctx;
        if (t.xapiStartup)
        {
            ctx.r3.u64 = t.startAddress;
            ctx.r4.u64 = t.startContext;
            CallGuest(ctx, t.xapiStartup);
        }
        else
        {
            ctx.r3.u64 = t.startContext;
            CallGuest(ctx, t.startAddress);
        }
        ExitGuestThread(ctx.r3.u32);
    }

    static std::shared_ptr<GuestThread> NewThread(uint32_t stackSize)
    {
        auto t = std::make_shared<GuestThread>();
        {
            std::lock_guard lock(s_threadLock);
            t->id = s_nextThreadId++;
        }
        SetupGuestThreadMemory(*t, stackSize);
        InitContext(*t);
        RegisterGuestObject(t->kthread, t);
        return t;
    }

    std::shared_ptr<GuestThread> StartGuestThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                                                  uint32_t xapiStartup, bool suspended)
    {
        auto t = NewThread(stackSize);
        t->startAddress = startAddress;
        t->startContext = startContext;
        t->xapiStartup = xapiStartup;
        t->suspendCount = suspended ? 1 : 0;
        if (!os::StartThread(GuestThreadEntry, new std::shared_ptr<GuestThread>(t), kHostStackSize))
        {
            LogFatal("Could not start a host thread");
            return nullptr;
        }
        return t;
    }

    static void MainThreadEntry(void* arg)
    {
        auto* holder = static_cast<std::shared_ptr<GuestThread>*>(arg);
        t_current = *holder;
        delete holder;
        PPCContext& ctx = *t_current->ctx;
        Log("Main thread %u: calling entry point 0x%08X", t_current->id, t_current->startAddress);
        CallGuest(ctx, t_current->startAddress);
        Log("Entry point returned (r3=0x%X); exiting", ctx.r3.u32);
        os::ExitProcess(0);
    }

    std::shared_ptr<GuestThread> StartMainThread(uint32_t entryPoint, uint32_t stackSize)
    {
        auto t = NewThread(stackSize);
        t->startAddress = entryPoint;
        if (!os::StartThread(MainThreadEntry, new std::shared_ptr<GuestThread>(t), kHostMainStackSize))
        {
            LogFatal("Could not start the main host thread");
            return nullptr;
        }
        return t;
    }

    PPCContext& AdoptHostThread(uint32_t stackSize)
    {
        t_current = NewThread(stackSize);
        return *t_current->ctx;
    }

    void LogCrashContext()
    {
        GuestThread* t = CurrentThread();
        if (!t)
        {
            LogFatal("  (not on a guest thread)");
            return;
        }
        LogFatal("  guest thread %u, last import: %s", t->id, t->currentImport ? t->currentImport : "(none)");
        if (t->ctx)
        {
            auto& c = *t->ctx;
            LogFatal("  lr=0x%08llX r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r31=0x%08X",
                     (unsigned long long)c.lr, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r31.u32);
        }
    }

    static std::shared_ptr<GuestThread> ThreadFromGuest(uint32_t kthreadPtr)
    {
        auto obj = ObjectFromGuest(kthreadPtr);
        if (!obj || obj->type != ObjType::Thread)
            return nullptr;
        return std::static_pointer_cast<GuestThread>(obj);
    }
}

using namespace wet;

[[noreturn]] void WetIndirectCallMiss(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    LogFatal("Indirect call to 0x%08X, which is not a recompiled function (lr=0x%08llX)",
             target, (unsigned long long)ctx.lr);
    LogFatal("  If this address is real code, add it to config/WET_config.toml `functions` and recompile.");
    LogCrashContext();
    os::ExitProcess(1);
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------

// ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start_address, start_context, creation_flags)
GUEST_IMPORT(ExCreateThread)
{
    uint32_t handlePtr = Arg(ctx, 0), stackSize = Arg(ctx, 1), idPtr = Arg(ctx, 2);
    uint32_t xapi = Arg(ctx, 3), start = Arg(ctx, 4), param = Arg(ctx, 5), flags = Arg(ctx, 6);

    auto t = StartGuestThread(stackSize ? stackSize : 0x10000, start, param, xapi, (flags & 1) != 0);
    if (!t)
        return Ret(ctx, STATUS_NO_MEMORY);
    if (handlePtr)
        St32(handlePtr, CreateHandle(t));
    if (idPtr)
        St32(idPtr, t->id);
    Log("ExCreateThread: thread %u start=0x%08X ctx=0x%08X xapi=0x%08X stack=0x%X flags=0x%X",
        t->id, start, param, xapi, t->stackSize, flags);
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(ExTerminateThread)
{
    ExitGuestThread(Arg(ctx, 0));
}

// NtResumeThread(handle, previous_suspend_count*)
GUEST_IMPORT(NtResumeThread)
{
    auto t = LookupHandleAs<GuestThread>(Arg(ctx, 0), ObjType::Thread);
    if (!t)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    int32_t prev;
    {
        std::lock_guard lock(DispatchLock());
        prev = t->suspendCount;
        if (t->suspendCount > 0)
            t->suspendCount--;
    }
    DispatchNotify();
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), uint32_t(prev));
    Ret(ctx, STATUS_SUCCESS);
}

// NtSuspendThread(handle, previous_suspend_count*)
GUEST_IMPORT(NtSuspendThread)
{
    auto t = LookupHandleAs<GuestThread>(Arg(ctx, 0), ObjType::Thread);
    if (!t)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    int32_t prev;
    {
        std::lock_guard lock(DispatchLock());
        prev = t->suspendCount++;
    }
    // Only takes effect for threads that have not started yet; suspending a running
    // thread needs cooperative safe points the recompiled code doesn't have.
    TraceCall("NtSuspendThread", ctx, "suspending a running thread is not supported");
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), uint32_t(prev));
    Ret(ctx, STATUS_SUCCESS);
}

// KeSetAffinityThread(kthread, affinity, previous_affinity*)
GUEST_IMPORT(KeSetAffinityThread)
{
    auto t = ThreadFromGuest(Arg(ctx, 0));
    uint32_t prev = t ? t->affinity : 0x3F;
    if (t)
        t->affinity = Arg(ctx, 1);
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), prev);
    Ret(ctx, STATUS_SUCCESS);
}

// KeSetBasePriorityThread(kthread, increment) -> previous priority
GUEST_IMPORT(KeSetBasePriorityThread)
{
    auto t = ThreadFromGuest(Arg(ctx, 0));
    int32_t prev = t ? t->priority : 0;
    if (t)
        t->priority = int32_t(Arg(ctx, 1));
    Ret(ctx, uint32_t(prev));
}

GUEST_IMPORT(KeQueryBasePriorityThread)
{
    auto t = ThreadFromGuest(Arg(ctx, 0));
    Ret(ctx, uint32_t(t ? t->priority : 0));
}

// KeDelayExecutionThread(processor_mode, alertable, interval*)
GUEST_IMPORT(KeDelayExecutionThread)
{
    uint32_t intervalPtr = Arg(ctx, 2);
    int64_t v = intervalPtr ? int64_t(Ld64(intervalPtr)) : 0;
    int64_t ns = v < 0 ? -v * 100 : (v > 0 ? (v - int64_t(CurrentFileTime())) * 100 : 0);
    if (ns <= 0)
        os::YieldThread();
    else
        os::SleepNs(uint64_t(ns));
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(NtYieldExecution)
{
    os::YieldThread();
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(KeGetCurrentProcessType)
{
    Ret(ctx, 1); // title process
}

// ---- KeTls* (dynamic TLS slots) ----
GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(s_threadLock);
    for (uint32_t i = 0; i < s_tlsUsed.size(); i++)
    {
        if (!s_tlsUsed[i])
        {
            s_tlsUsed[i] = true;
            return Ret(ctx, i);
        }
    }
    Log("KeTlsAlloc: out of TLS slots (%zu)", s_tlsUsed.size());
    Ret(ctx, 0xFFFFFFFF);
}

GUEST_IMPORT(KeTlsFree)
{
    uint32_t i = Arg(ctx, 0);
    std::lock_guard lock(s_threadLock);
    if (i >= s_tlsUsed.size())
        return Ret(ctx, 0);
    s_tlsUsed[i] = false;
    Ret(ctx, 1);
}

GUEST_IMPORT(KeTlsGetValue)
{
    uint32_t i = Arg(ctx, 0);
    if (i >= s_tlsSlotCount)
        return Ret(ctx, 0);
    Ret(ctx, Ld32(CurrentThread()->tlsSlots + i * 4));
}

GUEST_IMPORT(KeTlsSetValue)
{
    uint32_t i = Arg(ctx, 0);
    if (i >= s_tlsSlotCount)
        return Ret(ctx, 0);
    St32(CurrentThread()->tlsSlots + i * 4, Arg(ctx, 1));
    Ret(ctx, 1);
}
