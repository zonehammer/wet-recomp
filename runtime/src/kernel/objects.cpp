#include "objects.h"
#include "kernel.h"
#include "threads.h"
#include "../log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <unordered_map>

namespace wet
{
    // ------------------------------------------------------------------
    // Dispatcher
    // ------------------------------------------------------------------
    static std::mutex s_dispatchLock;

    struct Waiter
    {
        std::condition_variable cv;
        std::vector<KObject*> objs;
        KObject* thread = nullptr;
        bool notified = false;
        bool anyLate = false;       // waits on an object with lateWake
    };
    static std::vector<Waiter*> s_waiters;   // threads blocked right now (dispatcher lock)
    static thread_local Waiter t_waiter;

    std::mutex& DispatchLock() { return s_dispatchLock; }

    // WET_BROADCAST_WAKE=1: wake every waiter on every change, like before
    // per-object wakes (diagnostic).
    static bool BroadcastWake()
    {
        static const bool on = [] {
        const char* v = std::getenv("WET_BROADCAST_WAKE");
            bool b = v && *v == '1';
            if (b)
                Log("Kernel: WET_BROADCAST_WAKE=1 - every change wakes every waiting thread");
            return b;
        }();
        return on;
    }

    void DispatchNotifyLocked(KObject* obj)
    {
        if (BroadcastWake())
            obj = nullptr;
        for (Waiter* w : s_waiters)
        {
            bool match = !obj || w->thread == obj || w->anyLate;
            for (size_t i = 0; !match && i < w->objs.size(); i++)
                match = w->objs[i] == obj;
            if (match && !w->notified)
            {
                w->notified = true;
                w->cv.notify_one();
            }
        }
    }

    void DispatchNotify(KObject* obj)
    {
        std::lock_guard lock(s_dispatchLock);
        DispatchNotifyLocked(obj);
    }

    bool DispatchWait(std::unique_lock<std::mutex>& lock, KObject* const* objs, size_t count,
                      std::optional<std::chrono::steady_clock::time_point> until)
    {
        Waiter& w = t_waiter;
        w.objs.assign(objs, objs + count);
        w.thread = CurrentThread();
        w.notified = false;
        w.anyLate = false;
        for (size_t i = 0; i < count; i++)
            w.anyLate = w.anyLate || objs[i]->lateWake;
        s_waiters.push_back(&w);
        bool timedOut = false;
        while (!w.notified)
        {
            if (until)
            {
                if (w.cv.wait_until(lock, *until) == std::cv_status::timeout)
                {
                    timedOut = !w.notified;
                    break;
                }
            }
            else
                w.cv.wait(lock);
        }
        for (size_t i = 0; i < s_waiters.size(); i++)
            if (s_waiters[i] == &w)
            {
                s_waiters[i] = s_waiters.back();
                s_waiters.pop_back();
                break;
            }
        return !timedOut;
    }

    void KEvent::Set(bool s)
    {
        signaled = s;
        if (guestPtr)
        {
            St32(guestPtr + 4, s ? 1 : 0);
            lastWritten = s ? 1 : 0;
        }
    }

    void KEvent::SyncFromGuest()
    {
        if (!guestOwned || !guestPtr)
            return;
        uint32_t v = Ld32(guestPtr + 4);
        if (v != lastWritten)
        {
            // The game (re)initialised or signalled the event without the kernel.
            signaled = v != 0;
            manualReset = Ld8(guestPtr) == kDispNotificationEvent;
            lastWritten = v;
        }
    }

    void KSemaphore::SetCount(int32_t c)
    {
        count = c;
        if (guestPtr)
        {
            St32(guestPtr + 4, uint32_t(c));
            lastWritten = uint32_t(c);
        }
    }

    void KSemaphore::SyncFromGuest()
    {
        if (!guestOwned || !guestPtr)
            return;
        uint32_t v = Ld32(guestPtr + 4);
        if (v != lastWritten)
        {
            count = int32_t(v);
            max = int32_t(Ld32(guestPtr + 0x10));
            lastWritten = v;
        }
    }

    std::optional<std::chrono::steady_clock::time_point> DeadlineFromTimeout(uint32_t timeoutPtr)
    {
        if (!timeoutPtr)
            return std::nullopt;
        auto now = std::chrono::steady_clock::now();
        int64_t v = int64_t(Ld64(timeoutPtr));
        if (v < 0)
            return now + std::chrono::nanoseconds(-v * 100);
        if (v == 0)
            return now;
        int64_t delta = v - int64_t(CurrentFileTime());
        return now + std::chrono::nanoseconds(delta > 0 ? delta * 100 : 0);
    }

    uint32_t WaitForObjects(const std::vector<ObjectPtr>& objects, bool waitAll,
                            std::optional<std::chrono::steady_clock::time_point> deadline,
                            bool alertable)
    {
        GuestThread* self = CurrentThread();
        std::optional<WaitScope> scope;
        std::unique_lock lock(s_dispatchLock);
        bool afterTick = false;     // the last wait ended at the periodic re-check
        auto late = [&](KObject* o) {
            if (!afterTick || o->lateWake)
                return;
            o->lateWake = true;
            static int logged = 0;
            if (logged++ < 40)
            {
                static const char* kTypes[] = { "event", "semaphore", "mutant", "thread", "iocompletion", "file", "notify", "object" };
                std::string name;
                if (self)
                {
                    std::lock_guard g(self->statusLock);
                    name = self->name;
                }
                Log("Kernel: %s@%08X became signalled without a wake-up (thread %u %s); its waiters now wake on every change",
                    kTypes[int(o->type)], o->guestPtr, self ? self->id : 0, name.c_str());
            }
        };
        for (bool first = true;; first = false)
        {
            for (auto& o : objects)
                o->SyncFromGuest();

            if (waitAll)
            {
                bool all = true;
                for (auto& o : objects)
                    all = all && o->IsSignaled(self);
                if (all)
                {
                    for (auto& o : objects)
                    {
                        late(o.get());
                        o->Acquire(self);
                    }
                    return STATUS_SUCCESS;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(self))
                    {
                        late(objects[i].get());
                        objects[i]->Acquire(self);
                        return uint32_t(i);
                    }
                }
            }

            if (alertable && self && !self->apcs.empty())
            {
                lock.unlock();
                DeliverApcs();
                return STATUS_USER_APC;
            }

            auto now = std::chrono::steady_clock::now();
            if (deadline && now >= *deadline)
                return STATUS_TIMEOUT;

            if (first)
            {
                static const char* kNames[] = { "event", "semaphore", "mutant", "thread", "iocompletion", "file", "notify", "object" };
                std::string what = objects.empty() ? "alertable sleep" : waitAll ? "all of " : (objects.size() > 1 ? "any of " : "");
                for (size_t i = 0; i < objects.size() && i < 4; i++)
                {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "%s%s@%08X", i ? ", " : "", kNames[int(objects[i]->type)], objects[i]->guestPtr);
                    what += buf;
                }
                if (deadline)
                    what += " (with timeout)";
                lock.unlock();
                scope.emplace(std::move(what));
                lock.lock();
                continue;
            }

            // Wake periodically too: guest-owned objects can be changed by game
            // code writing memory directly, which never notifies us.
            bool anyLate = false;
            for (auto& o : objects)
                anyLate = anyLate || o->lateWake;
            auto next = now + (anyLate ? std::chrono::milliseconds(1) : std::chrono::milliseconds(5));
            bool tick = true;
            if (deadline && *deadline < next)
            {
                next = *deadline;
                tick = false;
            }
            KObject* raw[8];
            size_t n = std::min<size_t>(objects.size(), 8);
            for (size_t i = 0; i < n; i++)
                raw[i] = objects[i].get();
            if (objects.size() > 8)
                n = 0;   // unusual: any notification wakes it (DispatchWait with no objects
                         // still matches nullptr notifies; the 5 ms recheck covers the rest)
            afterTick = !DispatchWait(lock, raw, n, next) && tick;
        }
    }

    // ------------------------------------------------------------------
    // Handles
    // ------------------------------------------------------------------
    static std::mutex s_handleLock;
    static std::unordered_map<uint32_t, ObjectPtr> s_handles;
    static uint32_t s_nextHandle = 0x10000;

    uint32_t CreateHandle(const ObjectPtr& obj)
    {
        std::lock_guard lock(s_handleLock);
        uint32_t h = s_nextHandle;
        s_nextHandle += 4;
        s_handles[h] = obj;
        return h;
    }

    ObjectPtr LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThreadPtr();
        std::lock_guard lock(s_handleLock);
        auto it = s_handles.find(handle);
        return it == s_handles.end() ? nullptr : it->second;
    }

    bool CloseHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle || handle == kCurrentProcessHandle)
            return true;
        std::lock_guard lock(s_handleLock);
        return s_handles.erase(handle) != 0;
    }

    // ------------------------------------------------------------------
    // Guest-pointer objects
    // ------------------------------------------------------------------
    static std::mutex s_guestObjLock;
    static std::unordered_map<uint32_t, ObjectPtr> s_guestObjects;

    void RegisterGuestObject(uint32_t guestPtr, const ObjectPtr& obj)
    {
        std::lock_guard lock(s_guestObjLock);
        obj->guestPtr = guestPtr;
        s_guestObjects[guestPtr] = obj;
    }

    ObjectPtr ObjectFromGuest(uint32_t guestPtr)
    {
        if (!guestPtr)
            return nullptr;
        {
            std::lock_guard lock(s_guestObjLock);
            auto it = s_guestObjects.find(guestPtr);
            if (it != s_guestObjects.end())
                return it->second;
        }

        uint8_t type = Ld8(guestPtr);
        uint32_t signal = Ld32(guestPtr + 4);
        ObjectPtr obj;
        switch (type)
        {
        case kDispNotificationEvent:
        case kDispSynchronizationEvent:
        {
            auto e = std::make_shared<KEvent>(type == kDispNotificationEvent, signal != 0);
            e->lastWritten = signal;
            obj = e;
            break;
        }
        case kDispSemaphore:
        {
            auto s = std::make_shared<KSemaphore>(int32_t(signal), int32_t(Ld32(guestPtr + 0x10)));
            s->lastWritten = signal;
            obj = s;
            break;
        }
        case kDispMutant:
            obj = std::make_shared<KMutant>();
            break;
        case kDispNotificationTimer:
        case kDispSynchronizationTimer:
        {
            Log("ObjectFromGuest: timer object at 0x%08X treated as an event (timers not implemented)", guestPtr);
            auto e = std::make_shared<KEvent>(type == kDispNotificationTimer, signal != 0);
            e->lastWritten = signal;
            obj = e;
            break;
        }
        default:
            Log("ObjectFromGuest: unknown dispatcher type %u at 0x%08X; treating as a never-signalled event", type, guestPtr);
            obj = std::make_shared<KEvent>(true, false);
            break;
        }
        obj->guestOwned = true;

        std::lock_guard lock(s_guestObjLock);
        auto [it, inserted] = s_guestObjects.emplace(guestPtr, obj);
        it->second->guestPtr = guestPtr;
        return it->second;
    }

    uint32_t EnsureGuestObject(const ObjectPtr& obj)
    {
        if (obj->guestPtr)
            return obj->guestPtr;

        uint32_t p = SystemAlloc(0x20);
        switch (obj->type)
        {
        case ObjType::Event:
        {
            auto* e = static_cast<KEvent*>(obj.get());
            St8(p, e->manualReset ? kDispNotificationEvent : kDispSynchronizationEvent);
            St32(p + 4, e->signaled);
            e->lastWritten = e->signaled;
            break;
        }
        case ObjType::Semaphore:
        {
            auto* s = static_cast<KSemaphore*>(obj.get());
            St8(p, kDispSemaphore);
            St32(p + 4, uint32_t(s->count));
            St32(p + 0x10, uint32_t(s->max));
            s->lastWritten = uint32_t(s->count);
            break;
        }
        case ObjType::Mutant:
            St8(p, kDispMutant);
            break;
        default:
            St8(p, 0xFF);
            break;
        }
        St8(p + 2, 0x20 / 4);
        RegisterGuestObject(p, obj);
        return p;
    }
}
