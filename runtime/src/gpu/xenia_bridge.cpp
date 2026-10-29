// Hosts Xenia's GPU emulation (Vulkan backend) inside the runtime.
//
// Xenia's command processor, shader translator, texture / render-target caches
// and presenter are used unchanged. This file provides the few emulator
// services they expect (see runtime/xenia_shim): guest memory, host threads
// that can run the title's interrupt callback, and the "processor" that runs it.
//
// The renderer reads and writes physical memory through its own mapping, which is
// never protected. Pages it caches are write-watched in the title's views: the
// first CPU write invalidates the GPU copy (Xenia's scheme, see xe::Memory).
#include "gpu/renderer.h"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context_win.h"
#endif

DECLARE_bool(log_to_stdout);
DECLARE_path(log_file);

// ---------------------------------------------------------------------------
// Shim implementations
// ---------------------------------------------------------------------------
namespace
{
    struct Watch
    {
        xe::Memory::PhysicalMemoryInvalidationCallback callback;
        void* context;
    };
    std::mutex s_watchLock;
    std::vector<std::unique_ptr<Watch>> s_watches;

    thread_local xe::kernel::XThread* t_xthread = nullptr;

    void OnPhysicalWrite(uint32_t physStart, uint32_t length)
    {
        std::lock_guard lock(s_watchLock);
        for (auto& w : s_watches)
            w->callback(w->context, physStart, length, false);
    }
}

namespace xe
{
    void* Memory::RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                             void* callback_context)
    {
        std::lock_guard lock(s_watchLock);
        s_watches.push_back(std::make_unique<Watch>(Watch{ callback, callback_context }));
        return s_watches.back().get();
    }

    void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle)
    {
        std::lock_guard lock(s_watchLock);
        for (auto it = s_watches.begin(); it != s_watches.end(); ++it)
            if (it->get() == callback_handle)
            {
                s_watches.erase(it);
                return;
            }
    }

    void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                     bool enable_invalidation_notifications, bool)
    {
        if (enable_invalidation_notifications)
            wet::renderer_host::WatchPhysical(physical_address, length);
    }

    // No native message boxes: log and carry on (FatalError aborts afterwards).
    void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message)
    {
        std::string text = "Xenia GPU: ";
        text.append(message);
        wet::renderer_host::Log(text.c_str());
    }
    void LaunchWebBrowser(const std::string_view) {}
    void LaunchFileExplorer(const std::filesystem::path&) {}

    namespace cpu
    {
        uint64_t Processor::ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                                             size_t arg_count)
        {
            return wet::renderer_host::CallInterrupt(thread_state, address, args, arg_count);
        }
    }

    namespace kernel
    {
        XThread* XThread::GetCurrentThread() { return t_xthread; }

        void XThread::SetActiveCpu(uint32_t cpu)
        {
            if (thread_state_)
                wet::renderer_host::SetActiveCpu(thread_state_, cpu);
        }

        struct XHostThread::Impl
        {
            std::mutex lock;
            std::condition_variable cv;
            bool done = false;
        };

        XHostThread::~XHostThread() = default;

        void XHostThread::Entry(void* p)
        {
            auto* self = static_cast<XHostThread*>(p);
            auto impl = self->impl_;
            self->thread_state_ = static_cast<cpu::ThreadState*>(wet::renderer_host::AdoptThread(self->name_.c_str()));
            t_xthread = self;
            self->host_fn_();
            t_xthread = nullptr;
            std::lock_guard lock(impl->lock);
            impl->done = true;
            impl->cv.notify_all();
        }

        X_STATUS XHostThread::Create()
        {
            impl_ = std::make_shared<Impl>();
            wet::renderer_host::StartThread(&XHostThread::Entry, this, name_.c_str());
            return X_STATUS_SUCCESS;
        }

        X_STATUS XHostThread::Wait(uint32_t, uint32_t, uint32_t, uint64_t*)
        {
            if (!impl_)
                return X_STATUS_SUCCESS;
            std::unique_lock lock(impl_->lock);
            impl_->cv.wait(lock, [&] { return impl_->done; });
            return X_STATUS_SUCCESS;
        }
    }
}

// ---------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------
namespace
{
    std::unique_ptr<xe::Memory> s_memory;
    std::unique_ptr<xe::cpu::Processor> s_processor;
    std::unique_ptr<xe::kernel::KernelState> s_kernel;
    std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> s_graphics;
    std::atomic<bool> s_running{ false };

#if XE_PLATFORM_WIN32
    // The game window: Xenia's Win32 window and app context, on their own UI thread.
    struct GameWindow : xe::ui::WindowListener
    {
        std::unique_ptr<xe::ui::Win32WindowedAppContext> app;
        std::unique_ptr<xe::ui::Window> window;
        std::mutex lock;
        std::condition_variable cv;
        bool ready = false, ok = false;

        void OnClosing(xe::ui::UIEvent&) override
        {
            wet::renderer_host::Log("Window closed: exiting");
            wet::renderer_host::ExitProcess();
        }

        static void Thread(void* p)
        {
            auto* self = static_cast<GameWindow*>(p);
            self->app = std::make_unique<xe::ui::Win32WindowedAppContext>(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
            bool ok = self->app->Initialize();
            if (ok)
            {
                self->window = xe::ui::Window::Create(*self->app, "WET (recompiled)", 1280, 720);
                ok = self->window && (self->window->AddListener(self), self->window->Open());
            }
            {
                std::lock_guard l(self->lock);
                self->ready = true;
                self->ok = ok;
                self->cv.notify_all();
            }
            if (ok)
                self->app->RunMainMessageLoop();
            wet::renderer_host::ExitProcess();
        }

        bool Open()
        {
            wet::renderer_host::StartThread(&GameWindow::Thread, this, "UI");
            std::unique_lock l(lock);
            cv.wait(l, [&] { return ready; });
            return ok;
        }
    };
    GameWindow* s_window = nullptr;
#endif

    // Frame dumps for headless testing: WET_DUMP_FRAMES=<folder> writes the guest
    // output as PPM images, one every WET_DUMP_INTERVAL ms (default 1000).
    void DumpThread(void*)
    {
        const char* dir = std::getenv("WET_DUMP_FRAMES");
        int interval = std::getenv("WET_DUMP_INTERVAL") ? std::atoi(std::getenv("WET_DUMP_INTERVAL")) : 1000;
        std::filesystem::create_directories(dir);
        for (int n = 0;; n++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            xe::ui::Presenter* presenter = s_graphics ? s_graphics->presenter() : nullptr;
            xe::ui::RawImage image;
            if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width)
                continue;
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%05d.ppm", n);
            FILE* f = std::fopen((std::filesystem::path(dir) / name).string().c_str(), "wb");
            if (!f)
                continue;
            std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
            std::vector<uint8_t> row(size_t(image.width) * 3);
            for (uint32_t y = 0; y < image.height; y++)
            {
                const uint8_t* src = image.data.data() + y * image.stride;
                for (uint32_t x = 0; x < image.width; x++)
                    std::memcpy(&row[x * 3], src + x * 4, 3);
                std::fwrite(row.data(), 1, row.size(), f);
            }
            std::fclose(f);
        }
    }
}

namespace wet::renderer
{
    bool Enabled()
    {
        const char* e = std::getenv("WET_RENDERER");
        return !(e && e[0] == '0');
    }

    bool Running() { return s_running.load(); }

    bool Start(uint8_t* membase, uint32_t titleId)
    {
        if (s_running)
            return true;
        cvars::log_to_stdout = false;
        cvars::log_file = "xenia_gpu.log";
        xe::InitializeLogging("xenia_gpu");

        uint8_t* physical = renderer_host::PhysicalHostView();
        if (!physical)
            return false;
        s_memory = std::make_unique<xe::Memory>(membase, physical);
        renderer_host::SetPhysicalWriteCallback(OnPhysicalWrite);
        s_processor = std::make_unique<xe::cpu::Processor>(s_memory.get());
        s_kernel = std::make_unique<xe::kernel::KernelState>(titleId);
        s_graphics = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
        xe::ui::WindowedAppContext* appContext = nullptr;
#if XE_PLATFORM_WIN32
        if (!std::getenv("WET_HEADLESS"))
        {
            s_window = new GameWindow();
            if (s_window->Open())
                appContext = s_window->app.get();
            else
                renderer_host::Log("Renderer: could not open the game window; running without one");
        }
#endif
        xe::X_STATUS status = s_graphics->Setup(s_processor.get(), s_kernel.get(), appContext, true);
        if (XFAILED(status))
        {
            renderer_host::Log("Renderer: Vulkan initialisation failed (see xenia_gpu.log); using the stand-in GPU");
            s_graphics.reset();
            return false;
        }
#if XE_PLATFORM_WIN32
        if (appContext && s_graphics->presenter())
            appContext->CallInUIThreadSynchronous([] { s_window->window->SetPresenter(s_graphics->presenter()); });
#endif
        s_running = true;
        char text[160];
        std::snprintf(text, sizeof(text), "Renderer: Xenia GPU emulation on Vulkan started (%s)", s_graphics->name().c_str());
        renderer_host::Log(text);
        if (std::getenv("WET_DUMP_FRAMES"))
            renderer_host::StartThread(DumpThread, nullptr, "Frame dump");
        return true;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        if (s_graphics)
            s_graphics->SetInterruptCallback(callback, userData);
    }

    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2)
    {
        if (s_graphics)
            s_graphics->InitializeRingBuffer(physicalAddress, sizeLog2);
    }

    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2)
    {
        if (s_graphics)
            s_graphics->EnableReadPointerWriteBack(physicalAddress, blockSizeLog2);
    }

    void UpdateWritePointer(uint32_t value)
    {
        if (s_graphics)
        {
            s_graphics->command_processor()->UpdateWritePointer(value);
        }
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (!s_graphics || index >= xe::gpu::RegisterFile::kRegisterCount)
            return;
        auto& regs = *s_graphics->register_file();
        regs.values[index] = value;
        // Scratch registers written by the CPU are written back like the CP's own.
        if (index >= xe::gpu::XE_GPU_REG_SCRATCH_REG0 && index <= xe::gpu::XE_GPU_REG_SCRATCH_REG7)
        {
            uint32_t n = index - xe::gpu::XE_GPU_REG_SCRATCH_REG0;
            if ((1u << n) & regs.values[xe::gpu::XE_GPU_REG_SCRATCH_UMSK])
                xe::store_and_swap<uint32_t>(
                    s_memory->TranslatePhysical(regs.values[xe::gpu::XE_GPU_REG_SCRATCH_ADDR] + n * 4), value);
        }
    }

    uint32_t ReadRegister(uint32_t index)
    {
        if (!s_graphics || index >= xe::gpu::RegisterFile::kRegisterCount)
            return 0;
        return s_graphics->register_file()->values[index];
    }
}
