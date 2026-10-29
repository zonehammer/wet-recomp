// Kernel objects, the handle table and waiting.
//
// All dispatcher state (event signalled, semaphore count, mutant owner, thread
// exited, ...) is guarded by one global lock and one condition variable. That is
// slow under heavy contention but simple and obviously correct, which is what a
// first boot needs.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace wet
{
    struct GuestThread;

    enum class ObjType
    {
        Event,
        Semaphore,
        Mutant,
        Thread,
        IoCompletion,
        File,
        NotifyListener,
        Other,
    };

    // DISPATCHER_HEADER types as written by the game into guest memory
    enum : uint8_t
    {
        kDispNotificationEvent = 0,
        kDispSynchronizationEvent = 1,
        kDispMutant = 2,
        kDispSemaphore = 5,
        kDispThread = 6,
        kDispNotificationTimer = 8,
        kDispSynchronizationTimer = 9,
    };

    struct KObject
    {
        explicit KObject(ObjType t) : type(t) {}
        virtual ~KObject() = default;

        ObjType type;
        uint32_t guestPtr = 0;      // guest object (DISPATCHER_HEADER first), 0 if none yet
        bool guestOwned = false;    // lives in game memory (e.g. a KEVENT inside a game struct)

        // Called with the dispatcher lock held.
        virtual bool IsSignaled(GuestThread* waiter) { return false; }
        virtual void Acquire(GuestThread* waiter) {}
        // Re-read state from guest memory if the game rewrote the object behind our back.
        virtual void SyncFromGuest() {}
    };

    struct KEvent : KObject
    {
        KEvent(bool manual, bool initial) : KObject(ObjType::Event), manualReset(manual), signaled(initial) {}
        bool manualReset;
        bool signaled;
        uint32_t lastWritten = 0xFFFFFFFF;

        bool IsSignaled(GuestThread*) override { return signaled; }
        void Acquire(GuestThread*) override { if (!manualReset) Set(false); }
        void Set(bool s);           // updates guest SignalState too
        void SyncFromGuest() override;
    };

    struct KSemaphore : KObject
    {
        KSemaphore(int32_t initial, int32_t maximum) : KObject(ObjType::Semaphore), count(initial), max(maximum) {}
        int32_t count, max;
        uint32_t lastWritten = 0xFFFFFFFF;

        bool IsSignaled(GuestThread*) override { return count > 0; }
        void Acquire(GuestThread*) override { SetCount(count - 1); }
        void SetCount(int32_t c);
        void SyncFromGuest() override;
    };

    struct KMutant : KObject
    {
        KMutant() : KObject(ObjType::Mutant) {}
        GuestThread* owner = nullptr;
        int32_t recursion = 0;

        bool IsSignaled(GuestThread* t) override { return owner == nullptr || owner == t; }
        void Acquire(GuestThread* t) override { owner = t; recursion++; }
    };

    struct KIoCompletion : KObject
    {
        KIoCompletion() : KObject(ObjType::IoCompletion) {}
        struct Entry { uint32_t key, apcContext, status, information; };
        std::deque<Entry> queue;
        bool IsSignaled(GuestThread*) override { return !queue.empty(); }
    };

    struct KNotifyListener : KObject
    {
        KNotifyListener(uint64_t m) : KObject(ObjType::NotifyListener), mask(m) {}
        uint64_t mask;
        struct Note { uint32_t id, param; };
        std::deque<Note> queue;
        bool IsSignaled(GuestThread*) override { return !queue.empty(); }
    };

    using ObjectPtr = std::shared_ptr<KObject>;

    // ---- Dispatcher ----
    std::mutex& DispatchLock();
    void DispatchNotify();      // wake all waiters (call after changing state)
    std::condition_variable& DispatchCv();

    // Deadline from an Xbox LARGE_INTEGER timeout pointer (0 = wait forever).
    std::optional<std::chrono::steady_clock::time_point> DeadlineFromTimeout(uint32_t timeoutPtr);

    // Returns STATUS_WAIT_0 + index, STATUS_TIMEOUT, or (waitAll) STATUS_SUCCESS.
    // An alertable wait returns STATUS_USER_APC after running queued APCs. With no
    // objects it is a plain (possibly alertable) sleep that ends in STATUS_TIMEOUT.
    uint32_t WaitForObjects(const std::vector<ObjectPtr>& objects, bool waitAll,
                            std::optional<std::chrono::steady_clock::time_point> deadline,
                            bool alertable = false);

    // ---- Handles ----
    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;

    uint32_t CreateHandle(const ObjectPtr& obj);
    ObjectPtr LookupHandle(uint32_t handle);    // handles kCurrentThreadHandle
    bool CloseHandle(uint32_t handle);

    template <typename T>
    std::shared_ptr<T> LookupHandleAs(uint32_t handle, ObjType type)
    {
        auto obj = LookupHandle(handle);
        if (!obj || obj->type != type)
            return nullptr;
        return std::static_pointer_cast<T>(obj);
    }

    // ---- Guest-pointer objects (KEVENT etc. embedded in game memory) ----
    // Finds the object for a guest dispatcher pointer, creating it from the
    // DISPATCHER_HEADER the game wrote if this is the first time we see it.
    ObjectPtr ObjectFromGuest(uint32_t guestPtr);
    // Gives obj a guest-side header (for ObReferenceObjectByHandle) and returns it.
    uint32_t EnsureGuestObject(const ObjectPtr& obj);
    void RegisterGuestObject(uint32_t guestPtr, const ObjectPtr& obj);
}
