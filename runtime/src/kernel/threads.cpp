// Guest threads, PCR/TLS setup and thread-related imports.
#include "threads.h"
#include <atomic>
#include "kernel.h"
#include "os.h"

#include <vector>

namespace wet
{
    static thread_local std::shared_ptr<GuestThread> t_current;

    static std::mutex s_threadLock;
    static uint32_t s_nextThreadId = 0x10;
    static std::vector<std::weak_ptr<GuestThread>> s_threads; // s_threadLock

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
    //   KTHREAD+0x14C thread_id
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
        St32(t.kthread + 0x14C, t.id);      // thread id (GetCurrentThreadId reads this)
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
            DispatchWait(lock, nullptr, 0, std::nullopt);
    }

    [[noreturn]] void ExitGuestThread(uint32_t exitCode)
    {
        auto t = t_current;
        {
            std::lock_guard lock(DispatchLock());
            t->exited = true;
            t->exitCode = exitCode;
        }
        DispatchNotify(t.get());
        Log("Thread %u exited (code 0x%X)", t->id, exitCode);
        os::ExitCurrentThread();
    }

    static void GuestThreadEntry(void* arg)
    {
        auto* holder = static_cast<std::shared_ptr<GuestThread>*>(arg);
        t_current = *holder;
        delete holder;
        GuestThread& t = *t_current;
        t.hostTid = os::CurrentHostThreadId();

        WaitWhileSuspended(t);
        PPCContext& ctx = *t.ctx;
        {
            std::lock_guard lock(t.statusLock);
            if (t.name.empty())
                t.name = "thread@" + std::to_string(t.id);
        }
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
        {
            std::lock_guard lock(s_threadLock);
            s_threads.push_back(t);
        }
        return t;
    }

    // Processor a thread runs on, from an affinity mask (Xenia's GetFakeCpuNumber):
    // the single set bit, or round robin for 0. Titles index per-CPU state with
    // KeGetCurrentProcessorNumber, so it has to follow XSetThreadProcessor.
    static uint32_t FakeCpuNumber(uint32_t mask)
    {
        static std::atomic<uint32_t> next{ 0 };
        mask &= 0x3F;
        if (!mask)
            return next.fetch_add(1) % 6;
        uint32_t cpu = 0;
        while (mask >>= 1)
            cpu++;
        return cpu;
    }

    static void SetGuestThreadCpu(GuestThread& t, uint32_t cpu)
    {
        St8(t.pcr + 0x10C, uint8_t(cpu));       // KPCR current_cpu
        St8(t.kthread + 0xBF, uint8_t(cpu));    // KTHREAD current_cpu
        t.affinity = 1u << cpu;
    }

    std::shared_ptr<GuestThread> StartGuestThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                                                  uint32_t xapiStartup, bool suspended, uint32_t cpuMask)
    {
        auto t = NewThread(stackSize);
        SetGuestThreadCpu(*t, FakeCpuNumber(cpuMask));
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
        t_current->hostTid = os::CurrentHostThreadId();
        PPCContext& ctx = *t_current->ctx;
        Log("Main thread %u: calling entry point 0x%08X", t_current->id, t_current->startAddress);
        CallGuest(ctx, t_current->startAddress);
        Log("Entry point returned (r3=0x%X); exiting", ctx.r3.u32);
        os::ExitProcess(0);
    }

    std::shared_ptr<GuestThread> StartMainThread(uint32_t entryPoint, uint32_t stackSize)
    {
        auto t = NewThread(stackSize);
        SetGuestThreadCpu(*t, 0);
        t->startAddress = entryPoint;
        if (!os::StartThread(MainThreadEntry, new std::shared_ptr<GuestThread>(t), kHostMainStackSize))
        {
            LogFatal("Could not start the main host thread");
            return nullptr;
        }
        return t;
    }

    WaitScope::WaitScope(std::string what)
    {
        if (GuestThread* t = CurrentThread())
        {
            std::lock_guard lock(t->statusLock);
            t->waitingOn = std::move(what);
            t->waitLr = t->ctx ? uint32_t(t->ctx->lr) : 0;
            t->waitSinceNs = os::MonotonicNs();
        }
    }

    WaitScope::~WaitScope()
    {
        if (GuestThread* t = CurrentThread())
        {
            std::lock_guard lock(t->statusLock);
            t->waitingOn.clear();
        }
    }

    void SetThreadName(uint32_t threadId, const std::string& name)
    {
        std::shared_ptr<GuestThread> target;
        if (threadId == 0xFFFFFFFF)
            target = t_current;
        else
        {
            std::lock_guard lock(s_threadLock);
            for (auto& w : s_threads)
                if (auto t = w.lock(); t && t->id == threadId)
                    target = t;
        }
        if (!target)
            return;
        std::lock_guard lock(target->statusLock);
        target->name = name;
    }



    std::string GuestThreadNameForHostTid(uint32_t hostTid)
    {
        std::lock_guard lock(s_threadLock);
        for (auto& w : s_threads)
            if (auto t = w.lock(); t && t->hostTid == hostTid)
            {
                std::lock_guard l2(t->statusLock);
                return t->name.empty() ? "thread@" + std::to_string(t->id) : t->name;
            }
        return {};
    }

    static void StallMonitorMain(void* arg)
    {
        uint32_t interval = uint32_t(reinterpret_cast<uintptr_t>(arg));
        uint64_t lastFrames = PresentedFrameCount();
        int reports = 0;
        for (;;)
        {
            os::SleepNs(uint64_t(interval) * 1000000000ull);
            uint64_t frames = PresentedFrameCount();
            if (frames != lastFrames)
            {
                lastFrames = frames;
                continue;
            }
            if (reports++ >= 3)
                continue;
            Log("STALL: no frame presented in the last %u s (%llu so far). Thread states:", interval,
                (unsigned long long)frames);
            std::vector<std::shared_ptr<GuestThread>> threads;
            {
                std::lock_guard lock(s_threadLock);
                for (auto& w : s_threads)
                    if (auto t = w.lock())
                        threads.push_back(t);
            }
            uint64_t now = os::MonotonicNs();
            for (auto& t : threads)
            {
                std::lock_guard lock(t->statusLock);
                std::string name = t->name.empty() ? "?" : t->name;
                if (t->exited)
                    Log("  %3u %-30s exited", t->id, name.c_str());
                else if (!t->waitingOn.empty())
                    Log("  %3u %-30s waiting %5.1fs on %s (lr=%08X)", t->id, name.c_str(),
                        now > t->waitSinceNs ? double(now - t->waitSinceNs) / 1e9 : 0.0, t->waitingOn.c_str(), t->waitLr);
                else
                    Log("  %3u %-30s running (lr=%08X, last import %s)", t->id, name.c_str(),
                        t->ctx ? uint32_t(t->ctx->lr) : 0, t->currentImport ? t->currentImport : "-");
            }
        }
    }

    void StartStallMonitor(uint32_t intervalSeconds)
    {
        os::StartThread(StallMonitorMain, reinterpret_cast<void*>(uintptr_t(intervalSeconds)), 1 << 20);
    }

    PPCContext& AdoptHostThread(uint32_t stackSize)
    {
        t_current = NewThread(stackSize);
        t_current->hostTid = os::CurrentHostThreadId();
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
        LogFatal("  guest thread %u (%s), last import: %s", t->id, t->name.c_str(), t->currentImport ? t->currentImport : "(none)");
        if (t->ctx)
        {
            auto& c = *t->ctx;
            LogFatal("  lr=0x%08llX r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r31=0x%08X",
                     (unsigned long long)c.lr, c.r1.u32, c.r3.u32, c.r4.u32, c.r5.u32, c.r31.u32);
            // Guest call stack from the back chain: each frame's first word points to
            // the caller's frame, below which (at -8) the function saved its LR.
            std::string chain;
            uint32_t sp = c.r1.u32;
            for (int i = 0; i < 24; i++)
            {
                if (sp < t->stackLimit || sp + 4 > t->stackBase)
                    break;
                uint32_t back = Ld32(sp);
                if (back <= sp || back > t->stackBase)
                    break;
                char b[16];
                std::snprintf(b, sizeof(b), " %08X", Ld32(back - 8));
                chain += b;
                sp = back;
            }
            LogFatal("  guest call stack (return addresses):%s", chain.empty() ? " (none)" : chain.c_str());
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
    LogFlush();
    os::ShowFatalMessage("WET Recomp stopped (the game called code that wasn't recompiled).\n\nThe details are at the end of wet.log.");
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

    auto t = StartGuestThread(stackSize ? stackSize : 0x10000, start, param, xapi, (flags & 1) != 0, flags >> 24);
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
    DispatchNotify(t.get());
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
    uint32_t affinity = Arg(ctx, 1);
    if (!affinity)
        return Ret(ctx, STATUS_INVALID_PARAMETER);
    uint32_t prev = t ? t->affinity : 0x3F;
    if (t)
        SetGuestThreadCpu(*t, FakeCpuNumber(affinity));
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
    if (Arg(ctx, 1))
    {
        // Alertable (SleepEx(..., TRUE)): queued APCs end the sleep.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns > 0 ? ns : 0);
        uint32_t r = WaitForObjects({}, false, deadline, true);
        return Ret(ctx, r == STATUS_USER_APC ? STATUS_USER_APC : STATUS_SUCCESS);
    }
    if (ns <= 0)
        os::YieldThread();
    else
    {
        WaitScope ws("sleep " + std::to_string(ns / 1000000) + " ms");
        os::SleepNs(uint64_t(ns));
    }
    Ret(ctx, STATUS_SUCCESS);
}

namespace wet
{
    bool DeliverApcs()
    {
        GuestThread* self = CurrentThread();
        if (!self || !self->ctx)
            return false;
        bool any = false;
        for (;;)
        {
            GuestThread::Apc apc;
            {
                std::lock_guard lock(DispatchLock());
                if (self->apcs.empty())
                    break;
                apc = self->apcs.front();
                self->apcs.pop_front();
            }
            any = true;
            // NormalRoutine(NormalContext, SystemArgument1, SystemArgument2), run on a
            // copy of the registers so the interrupted import returns normally.
            PPCContext& ctx = *self->ctx;
            PPCContext saved = ctx;
            ctx.r1.u64 = (saved.r1.u32 - 0x100) & ~0xFull;
            ctx.r3.u64 = apc.context;
            ctx.r4.u64 = apc.arg1;
            ctx.r5.u64 = apc.arg2;
            const char* import = self->currentImport;
            CallGuest(ctx, apc.routine);
            self->currentImport = import;
            ctx = saved;
        }
        return any;
    }
}

// NtQueueApcThread(thread_handle, apc_routine, apc_routine_context, arg1, arg2)
GUEST_IMPORT(NtQueueApcThread)
{
    auto t = LookupHandleAs<GuestThread>(Arg(ctx, 0), ObjType::Thread);
    if (!t)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    {
        std::lock_guard lock(DispatchLock());
        if (t->exited)
            return Ret(ctx, STATUS_UNSUCCESSFUL);
        t->apcs.push_back({ Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3), Arg(ctx, 4) });
    }
    DispatchNotify(t.get());
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
