#pragma once
#include "objects.h"

#include <atomic>
#include <ppc_config.h>
#include <ppc_context.h>

namespace wet
{
    // One guest thread = one host thread running recompiled code.
    // Guest-side state (stack, PCR, KTHREAD, TLS) lives in the system heap.
    struct GuestThread : KObject
    {
        GuestThread() : KObject(ObjType::Thread) {}

        uint32_t id = 0;
        uint32_t stackSize = 0;
        uint32_t stackLimit = 0;    // lowest address
        uint32_t stackBase = 0;     // highest address (initial r1 is just below)
        uint32_t pcr = 0;           // r13
        uint32_t kthread = 0;       // X_KTHREAD (also the thread's guest dispatcher object)
        uint32_t tlsStatic = 0;     // __declspec(thread) data, PCR->tls_ptr
        uint32_t tlsSlots = 0;      // KeTls* slot array

        uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;

        int32_t suspendCount = 0;   // dispatcher lock
        bool exited = false;        // dispatcher lock
        uint32_t exitCode = 0;
        int32_t priority = 0;
        uint32_t affinity = 0x3F;

        PPCContext* ctx = nullptr;
        const char* currentImport = nullptr;

        bool IsSignaled(GuestThread*) override { return exited; }
    };

    GuestThread* CurrentThread();
    std::shared_ptr<GuestThread> CurrentThreadPtr();

    // Creates guest memory for a thread and starts it on a new host thread.
    std::shared_ptr<GuestThread> StartGuestThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
                                                  uint32_t xapiStartup, bool suspended);

    // Runs the title's entry point on a new host thread; returns its object.
    std::shared_ptr<GuestThread> StartMainThread(uint32_t entryPoint, uint32_t stackSize);

    [[noreturn]] void ExitGuestThread(uint32_t exitCode);

    // Gives the calling host thread its own guest stack/PCR/TLS so it can call guest
    // code (used for interrupt delivery). Returns the thread's context.
    PPCContext& AdoptHostThread(uint32_t stackSize);

    void InitThreading(uint32_t tlsSlotCount, uint32_t tlsRawAddr, uint32_t tlsDataSize, uint32_t tlsRawSize);
}
