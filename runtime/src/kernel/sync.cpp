// Events, semaphores, mutants, waits, critical sections, spinlocks, handles.
#include "kernel.h"
#include "objects.h"
#include "os.h"
#include "threads.h"

#include <atomic>
#include <unordered_map>

using namespace wet;

// Result of a wait on a single object, mapped back to NTSTATUS.
static uint32_t WaitOne(const ObjectPtr& obj, uint32_t timeoutPtr)
{
    if (!obj)
        return STATUS_INVALID_HANDLE;
    return WaitForObjects({ obj }, false, DeadlineFromTimeout(timeoutPtr)); // 0 == STATUS_WAIT_0
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// NtCreateEvent(handle*, object_attributes*, event_type, initial_state)
GUEST_IMPORT(NtCreateEvent)
{
    auto e = std::make_shared<KEvent>(Arg(ctx, 2) == 0 /* NotificationEvent */, Arg(ctx, 3) != 0);
    St32(Arg(ctx, 0), CreateHandle(e));
    Ret(ctx, STATUS_SUCCESS);
}

static uint32_t SetEventState(const std::shared_ptr<KEvent>& e, int op /* 0 clear, 1 set, 2 pulse */)
{
    uint32_t prev;
    {
        std::lock_guard lock(DispatchLock());
        e->SyncFromGuest();
        prev = e->signaled;
        if (op == 2)
        {
            // Pulse: release whoever is waiting right now, then reset.
            e->Set(true);
            DispatchNotify();
            e->Set(false);
        }
        else
        {
            e->Set(op == 1);
        }
    }
    DispatchNotify();
    return prev;
}

// NtSetEvent(handle, previous_state*)
GUEST_IMPORT(NtSetEvent)
{
    auto e = LookupHandleAs<KEvent>(Arg(ctx, 0), ObjType::Event);
    if (!e)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t prev = SetEventState(e, 1);
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), prev);
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(NtClearEvent)
{
    auto e = LookupHandleAs<KEvent>(Arg(ctx, 0), ObjType::Event);
    if (!e)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    SetEventState(e, 0);
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(NtPulseEvent)
{
    auto e = LookupHandleAs<KEvent>(Arg(ctx, 0), ObjType::Event);
    if (!e)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t prev = SetEventState(e, 2);
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), prev);
    Ret(ctx, STATUS_SUCCESS);
}

// KeSetEvent(kevent*, increment, wait) -> previous state
GUEST_IMPORT(KeSetEvent)
{
    auto obj = ObjectFromGuest(Arg(ctx, 0));
    if (!obj || obj->type != ObjType::Event)
        return Ret(ctx, 0);
    Ret(ctx, SetEventState(std::static_pointer_cast<KEvent>(obj), 1));
}

// KeResetEvent(kevent*) -> previous state
GUEST_IMPORT(KeResetEvent)
{
    auto obj = ObjectFromGuest(Arg(ctx, 0));
    if (!obj || obj->type != ObjType::Event)
        return Ret(ctx, 0);
    Ret(ctx, SetEventState(std::static_pointer_cast<KEvent>(obj), 0));
}

// ---------------------------------------------------------------------------
// Semaphores / mutants
// ---------------------------------------------------------------------------

// NtCreateSemaphore(handle*, object_attributes*, initial_count, maximum_count)
GUEST_IMPORT(NtCreateSemaphore)
{
    auto s = std::make_shared<KSemaphore>(int32_t(Arg(ctx, 2)), int32_t(Arg(ctx, 3)));
    St32(Arg(ctx, 0), CreateHandle(s));
    Ret(ctx, STATUS_SUCCESS);
}

// NtReleaseSemaphore(handle, release_count, previous_count*)
GUEST_IMPORT(NtReleaseSemaphore)
{
    auto s = LookupHandleAs<KSemaphore>(Arg(ctx, 0), ObjType::Semaphore);
    if (!s)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    int32_t release = int32_t(Arg(ctx, 1)), prev;
    {
        std::lock_guard lock(DispatchLock());
        s->SyncFromGuest();
        prev = s->count;
        if (int64_t(s->count) + release > s->max)
            return Ret(ctx, STATUS_SEMAPHORE_LIMIT_EXCEEDED);
        s->SetCount(s->count + release);
    }
    DispatchNotify();
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), uint32_t(prev));
    Ret(ctx, STATUS_SUCCESS);
}

// NtCreateMutant(handle*, object_attributes*, initial_owner)
GUEST_IMPORT(NtCreateMutant)
{
    auto m = std::make_shared<KMutant>();
    if (Arg(ctx, 2))
    {
        m->owner = CurrentThread();
        m->recursion = 1;
    }
    St32(Arg(ctx, 0), CreateHandle(m));
    Ret(ctx, STATUS_SUCCESS);
}

// NtReleaseMutant(handle, previous_count*)
GUEST_IMPORT(NtReleaseMutant)
{
    auto m = LookupHandleAs<KMutant>(Arg(ctx, 0), ObjType::Mutant);
    if (!m)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    {
        std::lock_guard lock(DispatchLock());
        if (m->owner != CurrentThread())
            return Ret(ctx, STATUS_MUTANT_NOT_OWNED);
        if (Arg(ctx, 1))
            St32(Arg(ctx, 1), uint32_t(m->recursion));
        if (--m->recursion == 0)
            m->owner = nullptr;
    }
    DispatchNotify();
    Ret(ctx, STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// Waits
// ---------------------------------------------------------------------------

// NtWaitForSingleObjectEx(handle, wait_mode, alertable, timeout*)
GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    Ret(ctx, WaitOne(LookupHandle(Arg(ctx, 0)), Arg(ctx, 3)));
}

// NtWaitForMultipleObjectsEx(count, handles*, wait_type, wait_mode, alertable, timeout*)
GUEST_IMPORT(NtWaitForMultipleObjectsEx)
{
    uint32_t count = Arg(ctx, 0), handles = Arg(ctx, 1);
    std::vector<ObjectPtr> objs;
    for (uint32_t i = 0; i < count; i++)
    {
        auto o = LookupHandle(Ld32(handles + i * 4));
        if (!o)
            return Ret(ctx, STATUS_INVALID_HANDLE);
        objs.push_back(o);
    }
    Ret(ctx, WaitForObjects(objs, Arg(ctx, 2) == 0 /* WaitAll */, DeadlineFromTimeout(Arg(ctx, 5))));
}

// KeWaitForSingleObject(object*, wait_reason, processor_mode, alertable, timeout*)
GUEST_IMPORT(KeWaitForSingleObject)
{
    Ret(ctx, WaitOne(ObjectFromGuest(Arg(ctx, 0)), Arg(ctx, 4)));
}

// KeWaitForMultipleObjects(count, objects**, wait_type, reason, mode, alertable, timeout*, wait_blocks*)
GUEST_IMPORT(KeWaitForMultipleObjects)
{
    uint32_t count = Arg(ctx, 0), array = Arg(ctx, 1);
    std::vector<ObjectPtr> objs;
    for (uint32_t i = 0; i < count; i++)
    {
        auto o = ObjectFromGuest(Ld32(array + i * 4));
        if (!o)
            return Ret(ctx, STATUS_INVALID_PARAMETER);
        objs.push_back(o);
    }
    Ret(ctx, WaitForObjects(objs, Arg(ctx, 2) == 0, DeadlineFromTimeout(Arg(ctx, 6))));
}

// ---------------------------------------------------------------------------
// I/O completion ports
// ---------------------------------------------------------------------------

// NtCreateIoCompletion(handle*, desired_access, object_attributes*, concurrent_threads)
GUEST_IMPORT(NtCreateIoCompletion)
{
    St32(Arg(ctx, 0), CreateHandle(std::make_shared<KIoCompletion>()));
    Ret(ctx, STATUS_SUCCESS);
}

// NtSetIoCompletion(handle, key_context, apc_context, completion_status, information)
GUEST_IMPORT(NtSetIoCompletion)
{
    auto port = LookupHandleAs<KIoCompletion>(Arg(ctx, 0), ObjType::IoCompletion);
    if (!port)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    {
        std::lock_guard lock(DispatchLock());
        port->queue.push_back({ Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3), Arg(ctx, 4) });
    }
    DispatchNotify();
    Ret(ctx, STATUS_SUCCESS);
}

// NtRemoveIoCompletion(handle, key_context*, apc_context*, io_status_block*, timeout*)
GUEST_IMPORT(NtRemoveIoCompletion)
{
    auto port = LookupHandleAs<KIoCompletion>(Arg(ctx, 0), ObjType::IoCompletion);
    if (!port)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    auto deadline = DeadlineFromTimeout(Arg(ctx, 4));
    std::unique_lock lock(DispatchLock());
    while (port->queue.empty())
    {
        if (deadline)
        {
            if (DispatchCv().wait_until(lock, *deadline) == std::cv_status::timeout && port->queue.empty())
                return Ret(ctx, STATUS_TIMEOUT);
        }
        else
        {
            DispatchCv().wait(lock);
        }
    }
    auto e = port->queue.front();
    port->queue.pop_front();
    lock.unlock();
    if (Arg(ctx, 1)) St32(Arg(ctx, 1), e.key);
    if (Arg(ctx, 2)) St32(Arg(ctx, 2), e.apcContext);
    if (Arg(ctx, 3)) { St32(Arg(ctx, 3), e.status); St32(Arg(ctx, 3) + 4, e.information); }
    Ret(ctx, STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// Handles / object manager
// ---------------------------------------------------------------------------

GUEST_IMPORT(NtClose)
{
    Ret(ctx, CloseHandle(Arg(ctx, 0)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}

// NtDuplicateObject(handle, new_handle*, options)
GUEST_IMPORT(NtDuplicateObject)
{
    uint32_t h = Arg(ctx, 0);
    auto obj = LookupHandle(h);
    if (!obj)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), CreateHandle(obj));
    if (Arg(ctx, 2) & 1) // DUPLICATE_CLOSE_SOURCE
        CloseHandle(h);
    Ret(ctx, STATUS_SUCCESS);
}

// ObReferenceObjectByHandle(handle, object_type*, object**)
GUEST_IMPORT(ObReferenceObjectByHandle)
{
    auto obj = LookupHandle(Arg(ctx, 0));
    if (!obj)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), EnsureGuestObject(obj));
    Ret(ctx, STATUS_SUCCESS);
}

// Objects are kept alive by the handle table / guest registry, so references are no-ops.
GUEST_IMPORT(ObReferenceObject) { Ret(ctx, 0); }
GUEST_IMPORT(ObDereferenceObject) { Ret(ctx, 0); }
GUEST_IMPORT(ObIsTitleObject) { Ret(ctx, 1); }

// ---------------------------------------------------------------------------
// Critical sections
//
// X_RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10), lock_count +0x10 (starts -1),
// recursion_count +0x14, owning_thread +0x18. The lock itself is a host recursive
// mutex; the guest fields are kept up to date for code that inspects them.
// ---------------------------------------------------------------------------
namespace
{
    std::mutex s_csMapLock;
    std::unordered_map<uint32_t, std::unique_ptr<std::recursive_mutex>> s_cs;

    std::recursive_mutex& CsFor(uint32_t cs)
    {
        std::lock_guard lock(s_csMapLock);
        auto& m = s_cs[cs];
        if (!m)
            m = std::make_unique<std::recursive_mutex>();
        return *m;
    }

    void CsInit(uint32_t cs, uint32_t spin)
    {
        St8(cs + 0, kDispSynchronizationEvent);
        St8(cs + 1, uint8_t(spin / 256));
        St8(cs + 2, 0x10 / 4);
        St32(cs + 4, 0);
        St32(cs + 0x10, 0xFFFFFFFF);
        St32(cs + 0x14, 0);
        St32(cs + 0x18, 0);
        CsFor(cs);
    }

    void CsAcquired(uint32_t cs)
    {
        St32(cs + 0x10, Ld32(cs + 0x10) + 1);
        St32(cs + 0x14, Ld32(cs + 0x14) + 1);
        St32(cs + 0x18, CurrentThread() ? CurrentThread()->kthread : 0);
    }
}

GUEST_IMPORT(RtlInitializeCriticalSection)
{
    CsInit(Arg(ctx, 0), 0);
}

GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount)
{
    CsInit(Arg(ctx, 0), Arg(ctx, 1));
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = Arg(ctx, 0);
    CsFor(cs).lock();
    CsAcquired(cs);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = Arg(ctx, 0);
    if (!CsFor(cs).try_lock())
        return Ret(ctx, 0);
    CsAcquired(cs);
    Ret(ctx, 1);
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = Arg(ctx, 0);
    uint32_t rec = Ld32(cs + 0x14) - 1;
    St32(cs + 0x14, rec);
    St32(cs + 0x10, Ld32(cs + 0x10) - 1);
    if (rec == 0)
        St32(cs + 0x18, 0);
    CsFor(cs).unlock();
}

// ---------------------------------------------------------------------------
// Spinlocks (a guest u32: 0 = free, else owner)
// ---------------------------------------------------------------------------
static void SpinAcquire(uint32_t lockPtr)
{
    auto* p = reinterpret_cast<uint32_t*>(HostPtr(lockPtr));
    uint32_t me = __builtin_bswap32(CurrentThread() ? CurrentThread()->kthread : 1);
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(p, &expected, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        expected = 0;
        os::YieldThread();
    }
}

static void SpinRelease(uint32_t lockPtr)
{
    __atomic_store_n(reinterpret_cast<uint32_t*>(HostPtr(lockPtr)), 0u, __ATOMIC_RELEASE);
}

GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(Arg(ctx, 0)); Ret(ctx, 0); /* old IRQL */ }
GUEST_IMPORT(KfReleaseSpinLock) { SpinRelease(Arg(ctx, 0)); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(Arg(ctx, 0)); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { SpinRelease(Arg(ctx, 0)); }

// Single-processor-style critical regions / L2 locking: nothing to do on the host.
GUEST_IMPORT(KeEnterCriticalRegion) {}
GUEST_IMPORT(KeLeaveCriticalRegion) {}
GUEST_IMPORT(KeLockL2) { Ret(ctx, 0); }
GUEST_IMPORT(KeUnlockL2) {}

// ---------------------------------------------------------------------------
// InterlockedPopEntrySList(SLIST_HEADER*)
// Header (big-endian u64): Next(32) | Depth(16) | Sequence(16). The game pushes
// inline with ldarx/stdcx., which the recompiler turns into a 64-bit CAS on the
// same memory, so a CAS loop here interoperates with it.
// ---------------------------------------------------------------------------
GUEST_IMPORT(InterlockedPopEntrySList)
{
    auto* p = reinterpret_cast<uint64_t*>(HostPtr(Arg(ctx, 0)));
    for (;;)
    {
        uint64_t raw = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        uint64_t v = __builtin_bswap64(raw);
        uint32_t first = uint32_t(v >> 32);
        if (!first)
            return Ret(ctx, 0);
        uint32_t next = Ld32(first);
        uint16_t depth = uint16_t(v >> 16), seq = uint16_t(v);
        uint64_t nv = (uint64_t(next) << 32) | (uint64_t(uint16_t(depth - 1)) << 16) | seq;
        uint64_t nraw = __builtin_bswap64(nv);
        if (__atomic_compare_exchange_n(p, &raw, nraw, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return Ret(ctx, first);
    }
}
