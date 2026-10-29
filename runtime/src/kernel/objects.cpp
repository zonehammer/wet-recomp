#include "objects.h"
#include "kernel.h"
#include "threads.h"

#include <cstdio>

#include <unordered_map>

namespace wet
{
    // ------------------------------------------------------------------
    // Dispatcher
    // ------------------------------------------------------------------
    static std::mutex s_dispatchLock;
    static std::condition_variable s_dispatchCv;

    std::mutex& DispatchLock() { return s_dispatchLock; }
    void DispatchNotify() { s_dispatchCv.notify_all(); }
    std::condition_variable& DispatchCv() { return s_dispatchCv; }

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
                            std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        GuestThread* self = CurrentThread();
        std::optional<WaitScope> scope;
        std::unique_lock lock(s_dispatchLock);
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
                        o->Acquire(self);
                    return STATUS_SUCCESS;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(self))
                    {
                        objects[i]->Acquire(self);
                        return uint32_t(i);
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (deadline && now >= *deadline)
                return STATUS_TIMEOUT;

            if (first)
            {
                static const char* kNames[] = { "event", "semaphore", "mutant", "thread", "iocompletion", "file", "notify", "object" };
                std::string what = waitAll ? "all of " : (objects.size() > 1 ? "any of " : "");
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
            auto next = now + std::chrono::milliseconds(5);
            if (deadline && *deadline < next)
                next = *deadline;
            s_dispatchCv.wait_until(lock, next);
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
