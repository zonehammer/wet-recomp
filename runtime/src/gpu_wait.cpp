// Proper wait for GPU fences.
//
// The console D3D's fence wait (sub_8305E9E0) spins until the GPU has passed a
// fence, calling sub_83066340 each time round (a short delay loop plus hang
// detection). On the 360 the GPU catches up within microseconds; here Xenia's
// command processor can be a whole frame behind, so the render thread burned a
// core spinning. sub_83066340, when called from that loop, now blocks until the
// command processor writes something that could be the fence (it signals after
// MEM_WRITE / EVENT_WRITE* / scratch registers / the read pointer write-back), with
// a 2 ms limit so nothing can hang on a signal we don't send.
#include "gpu/renderer.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace
{
    std::atomic<uint64_t> s_generation{ 0 };
    std::atomic<int> s_waiting{ 0 };
    std::mutex s_lock;
    std::condition_variable s_cv;
    std::atomic<uint64_t> s_waits{ 0 }, s_timeouts{ 0 };

    constexpr uint32_t kFenceLoopReturn = 0x8305EA78;   // lr of the call in sub_8305E9E0

    // sub_8305E9E0's loop condition: the fence hasn't been reached yet.
    bool FencePending(uint8_t* base, uint32_t device, uint32_t fence)
    {
        uint32_t top = PPC_LOAD_U32(device + 10908);
        uint32_t cur = PPC_LOAD_U32(PPC_LOAD_U32(device + 10896));
        return top - fence < top - cur;
    }

    void WaitForFence(uint8_t* base, uint32_t device, uint32_t fence)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        bool waited = false;
        for (;;)
        {
            uint64_t gen = s_generation.load();
            if (!FencePending(base, device, fence))
                break;
            waited = true;
            s_waiting.fetch_add(1);
            bool timedOut;
            {
                std::unique_lock lock(s_lock);
                timedOut = !s_cv.wait_until(lock, deadline, [&] { return s_generation.load() != gen; });
            }
            s_waiting.fetch_sub(1);
            if (timedOut)
            {
                s_timeouts.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        if (waited)
        {
            uint64_t n = s_waits.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0 && n >= 1024)
                wet::Log("GPU fence waits: %llu, %llu reached the 2 ms limit", (unsigned long long)n,
                         (unsigned long long)s_timeouts.load());
        }
    }
}

namespace wet::renderer_host
{
    void GpuProgress()
    {
        s_generation.fetch_add(1);
        if (s_waiting.load())
        {
            std::lock_guard lock(s_lock);
            s_cv.notify_all();
        }
    }
}

extern "C" void __imp__sub_83066340(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_83066340)
{
    // Called with sub_8305E9E0's registers still live: r31 = device, r30 = fence.
    if (ctx.lr == kFenceLoopReturn && wet::options::GpuWait())
        WaitForFence(base, ctx.r31.u32, ctx.r30.u32);
    __imp__sub_83066340(ctx, base);
}
