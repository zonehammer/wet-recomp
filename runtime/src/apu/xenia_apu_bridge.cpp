// Hosts Xenia's XMA decoder contexts and XAudio2 output for the runtime (see
// apu.h). Xenia is BSD-licensed; its XMA decoding uses FFmpeg (LGPL).
#include "apu/apu.h"
#include "gpu/renderer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/apu/xma_context.h"
#include "xenia/base/threading.h"
#include "xenia/memory.h"
#if XE_PLATFORM_WIN32
#include "xenia/apu/xaudio2/xaudio2_audio_driver.h"
#endif

namespace wet::apu
{
    namespace
    {
        std::unique_ptr<xe::Memory> s_memory;
        std::vector<std::unique_ptr<xe::apu::XmaContext>> s_contexts;
        std::mutex s_workLock;
        std::condition_variable s_workCv;
        bool s_workPending = false;

        // Like Xenia's XmaDecoder worker: kicks enable contexts, this decodes them.
        void Worker()
        {
            for (;;)
            {
                {
                    std::unique_lock lock(s_workLock);
                    s_workCv.wait(lock, [] { return s_workPending; });
                    s_workPending = false;
                }
                for (auto& c : s_contexts)
                    if (c->is_allocated() && c->is_enabled())
                        c->Work();
            }
        }
    }

    bool XmaStart(uint8_t* membase, uint8_t* physicalView, uint32_t firstContextGuest, uint32_t count)
    {
        s_memory = std::make_unique<xe::Memory>(membase, physicalView);
        s_contexts.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            s_contexts[i] = std::make_unique<xe::apu::XmaContext>();
            if (s_contexts[i]->Setup(i, s_memory.get(), firstContextGuest + i * 64))
            {
                s_contexts.clear();
                renderer_host::Log("XMA: FFmpeg's XMA decoder is unavailable; decoding to silence");
                return false;
            }
        }
        std::thread(Worker).detach();
        return true;
    }

    void XmaAllocate(uint32_t id)
    {
        if (id < s_contexts.size())
            s_contexts[id]->set_is_allocated(true);
    }

    void XmaRelease(uint32_t id)
    {
        if (id < s_contexts.size() && s_contexts[id]->is_allocated())
            s_contexts[id]->Release();
    }

    void XmaKick(uint32_t id)
    {
        if (id >= s_contexts.size())
            return;
        s_contexts[id]->Enable();
        {
            std::lock_guard lock(s_workLock);
            s_workPending = true;
        }
        s_workCv.notify_one();
    }

    void XmaLock(uint32_t id)
    {
        if (id < s_contexts.size())
            s_contexts[id]->Disable();
    }

    void XmaClear(uint32_t id)
    {
        if (id < s_contexts.size())
            s_contexts[id]->Clear();
    }

#if XE_PLATFORM_WIN32
    struct Output
    {
        std::unique_ptr<xe::threading::Semaphore> semaphore;
        std::unique_ptr<xe::apu::xaudio2::XAudio2AudioDriver> driver;
    };

    Output* OpenOutput()
    {
        if (!s_memory)
            return nullptr;
        auto out = new Output();
        // Xenia's AudioSystem: up to 64 frames queued; the driver releases one
        // per frame played.
        constexpr int kMaxQueuedFrames = 64;
        // Start with 16 queued (85 ms) rather than 64, for less latency.
        out->semaphore = xe::threading::Semaphore::Create(16, kMaxQueuedFrames);
        out->driver = std::make_unique<xe::apu::xaudio2::XAudio2AudioDriver>(s_memory.get(), out->semaphore.get());
        if (!out->driver->Initialize())
        {
            renderer_host::Log("Audio: XAudio2 could not be initialised (see xenia_gpu.log); no sound");
            out->driver.reset();
            delete out;
            return nullptr;
        }
        return out;
    }

    bool WaitForFrameSlot(Output* out, uint32_t timeoutMs)
    {
        return xe::threading::Wait(out->semaphore.get(), false, std::chrono::milliseconds(timeoutMs)) ==
               xe::threading::WaitResult::kSuccess;
    }

    void SubmitFrame(Output* out, uint32_t guest) { out->driver->SubmitFrame(guest); }
#else
    struct Output {};
    Output* OpenOutput() { return nullptr; }
    bool WaitForFrameSlot(Output*, uint32_t) { return false; }
    void SubmitFrame(Output*, uint32_t) {}
#endif
}
