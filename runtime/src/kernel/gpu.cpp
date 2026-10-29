// Stand-in GPU: just enough of the Xenos interface for the game to get past
// graphics initialisation and into its frame loop. Nothing is drawn.
//
// - GPU registers live at 0x7FC80000 (plain memory, see memory.h).
// - The command processor "consumes" everything instantly: whenever the game
//   advances CP_RB_WPTR, the read pointer is set to match, both in CP_RB_RPTR and
//   at the write-back address the game polls.
// - A 60 Hz vertical-blank interrupt is delivered to the game's graphics
//   interrupt callback (source 0), on a dedicated thread with its own guest stack.
//
// TODO (real GPU work): parse the PM4 packets in the ring buffer (register writes,
// MEM_WRITE / EVENT_WRITE fences, INDIRECT_BUFFER, draws), raise command-processor
// interrupts (source 1), and translate rendering to D3D12/Vulkan.
#include "kernel.h"
#include "os.h"
#include "threads.h"

#include <atomic>
#include <mutex>

using namespace wet;

namespace
{
    constexpr uint32_t kRegCpRbRptr = 0x01C4;
    constexpr uint32_t kRegCpRbWptr = 0x01C5;

    std::atomic<uint32_t> s_interruptCallback{ 0 }, s_interruptUser{ 0 };
    std::atomic<uint32_t> s_ringBase{ 0 }, s_ringSizeLog2{ 0 }, s_rptrWriteBack{ 0 };
    std::atomic<uint64_t> s_vblanks{ 0 }, s_swaps{ 0 };
    std::once_flag s_gpuStarted;

    uint32_t Reg(uint32_t index) { return Ld32(kGpuRegisterBase + index * 4); }
    void SetReg(uint32_t index, uint32_t v) { St32(kGpuRegisterBase + index * 4, v); }

    void GpuThreadMain(void*)
    {
        PPCContext& ctx = AdoptHostThread(0x40000);
        GuestThread* self = CurrentThread();
        Log("GPU stand-in started (guest thread %u): instant command consumption + 60 Hz vblank", self->id);

        constexpr uint64_t kVblankNs = 16666667;
        uint64_t nextVblank = os::MonotonicNs() + kVblankNs;
        uint32_t lastRptr = 0;
        uint32_t reportedWrites = 0;

        for (;;)
        {
            os::SleepNs(1000000);

            // Command processor: consume everything that was submitted.
            uint32_t wptr = Reg(kRegCpRbWptr);
            if (wptr != lastRptr)
            {
                lastRptr = wptr;
                SetReg(kRegCpRbRptr, wptr);
                uint32_t wb = s_rptrWriteBack.load();
                uint32_t wbGuest = wb ? PhysicalToGuest(wb) : 0;
                if (wbGuest)
                    St32(wbGuest, wptr);
                if (reportedWrites < 3 || (++reportedWrites & 0xFFF) == 0)
                {
                    if (reportedWrites < 3) reportedWrites++;
                    Log("GPU: ring write pointer -> 0x%X (consumed; write-back %s)", wptr,
                        wbGuest ? "updated" : (wb ? "address not mapped" : "not set"));
                }
            }

            // Vertical blank.
            uint64_t now = os::MonotonicNs();
            if (now >= nextVblank)
            {
                nextVblank += kVblankNs;
                if (now > nextVblank + kVblankNs * 4)
                    nextVblank = now + kVblankNs; // don't try to catch up after a stall
                uint64_t n = ++s_vblanks;
                uint32_t cb = s_interruptCallback.load();
                if (cb)
                {
                    ctx.r1.u64 = self->stackBase - 0x80;
                    ctx.r3.u64 = 0;                     // source: vblank
                    ctx.r4.u64 = s_interruptUser.load();
                    SetCurrentImport("(vblank interrupt)");
                    CallGuest(ctx, cb);
                    if (n == 1 || n == 60 || (n % 3600) == 0)
                        Log("GPU: vblank %llu delivered (%llu frames presented so far)",
                            (unsigned long long)n, (unsigned long long)s_swaps.load());
                }
            }
        }
    }

    void EnsureGpu()
    {
        std::call_once(s_gpuStarted, [] {
            if (!os::StartThread(GpuThreadMain, nullptr, 16 << 20))
                LogFatal("Could not start the GPU thread");
        });
    }
}

// VdSetGraphicsInterruptCallback(callback, user_data)
GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    s_interruptCallback = Arg(ctx, 0);
    s_interruptUser = Arg(ctx, 1);
    Log("VdSetGraphicsInterruptCallback(0x%08X, 0x%08X)", Arg(ctx, 0), Arg(ctx, 1));
    EnsureGpu();
}

// VdInitializeRingBuffer(physical_address, size_log2)
GUEST_IMPORT(VdInitializeRingBuffer)
{
    s_ringBase = Arg(ctx, 0);
    s_ringSizeLog2 = Arg(ctx, 1);
    Log("VdInitializeRingBuffer(0x%08X -> guest 0x%08X, %u bytes)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)),
        1u << (Arg(ctx, 1) + 3));
    EnsureGpu();
}

// VdEnableRingBufferRPtrWriteBack(physical_address, block_size_log2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    s_rptrWriteBack = Arg(ctx, 0);
    Log("VdEnableRingBufferRPtrWriteBack(0x%08X -> guest 0x%08X)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)));
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    Log("VdSetSystemCommandBufferGpuIdentifierAddress(0x%08X)", Arg(ctx, 0));
}

// VdSwap(...) - the title presented a frame.
GUEST_IMPORT(VdSwap)
{
    uint64_t n = ++s_swaps;
    if (n <= 3 || (n & (n - 1)) == 0)
        Log("VdSwap: frame %llu presented", (unsigned long long)n);
    Ret(ctx, 0);
}
