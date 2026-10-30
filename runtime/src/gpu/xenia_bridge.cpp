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

#include <algorithm>
#include <atomic>
#include <cctype>
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
#if WET_HAVE_D3D12
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif
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
DECLARE_bool(host_present_from_non_ui_thread);
DECLARE_int32(log_level);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_path(dump_shaders);
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
        // Like Xenia's PhysicalHeap::TriggerCallbacks: the callbacks may invalidate
        // a wider range than the written page (up to 256 KB around it); unwatching
        // what all of them invalidated saves a fault per neighbouring page.
        uint64_t first = 0, last = UINT32_MAX;
        {
            std::lock_guard lock(s_watchLock);
            if (s_watches.empty())
                return;
            for (auto& w : s_watches)
            {
                auto r = w->callback(w->context, physStart, length, false);
                first = std::max<uint64_t>(first, r.first);
                last = std::min<uint64_t>(last, uint64_t(r.first) + std::max<uint32_t>(r.second, 1) - 1);
            }
        }
        uint64_t reqLast = uint64_t(physStart) + length - 1;
        first = std::min<uint64_t>(first, physStart);
        last = std::max<uint64_t>(last, reqLast);
        constexpr uint64_t kMaxExcess = 4 * 1024 * 1024;
        first = std::max<uint64_t>(first, physStart & ~(kMaxExcess - 1));
        last = std::min<uint64_t>(last, reqLast | (kMaxExcess - 1));
        if (first < physStart || last > reqLast)
            wet::renderer_host::UnwatchPhysical(uint32_t(first), uint32_t(last - first + 1));
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
    std::unique_ptr<xe::gpu::GraphicsSystem> s_graphics;
    uint32_t s_resolutionScale = 1;   // rendering resolution = 1280x720 x this
    const char* s_backend = "Vulkan";
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
                self->window = xe::ui::Window::Create(*self->app, "WET (recompiled)", s_resolutionScale >= 2 ? 1920 : 1280,
                                                      s_resolutionScale >= 2 ? 1080 : 720);
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

        // Repaints the window at 60 Hz from its own thread (WM_PAINT), so presentation
        // never depends on a single paint request getting through.
        static void RepaintThread(void* p)
        {
            auto* self = static_cast<GameWindow*>(p);
            // Repaint at (a little above) the guest vblank rate, so the window
            // shows every frame at 120/144 FPS too (it was a fixed 16 ms).
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                uint32_t hz = std::max<uint32_t>(xe::gpu::wet_vsync_hz.load(), 60);
                next += std::chrono::microseconds(1000000 / (hz + hz / 8));
                auto now = std::chrono::steady_clock::now();
                if (next < now)
                    next = now;
                else
                    std::this_thread::sleep_until(next);
                if (self->window)
                    self->window->RequestPaint();
            }
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

    // Snapshots of the guest output (what the GPU produced, before the window):
    // by default one every 5 s for the first 2 minutes into "frames/", each logged
    // with its average brightness, to tell rendering problems from presentation
    // problems. WET_DUMP_FRAMES=<folder> / WET_DUMP_INTERVAL=<ms> dump continuously.
    void DumpThread(void*)
    {
        const char* env = std::getenv("WET_DUMP_FRAMES");
        std::filesystem::path dir = env ? env : "frames";
        int interval = std::getenv("WET_DUMP_INTERVAL") ? std::atoi(std::getenv("WET_DUMP_INTERVAL")) : (env ? 1000 : 5000);
        int limit = env ? 1 << 30 : 24;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (int n = 0; n < limit; n++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            xe::ui::Presenter* presenter = s_graphics ? s_graphics->presenter() : nullptr;
            xe::ui::RawImage image;
            if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "Renderer: snapshot %d: no guest output yet", n);
                wet::renderer_host::Log(text);
                continue;
            }
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%05d.ppm", n);
            FILE* f = std::fopen((dir / name).string().c_str(), "wb");
            uint64_t sum = 0;
            std::vector<uint8_t> row(size_t(image.width) * 3);
            if (f)
                std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
            for (uint32_t y = 0; y < image.height; y++)
            {
                const uint8_t* src = image.data.data() + y * image.stride;
                for (uint32_t x = 0; x < image.width; x++)
                {
                    std::memcpy(&row[x * 3], src + x * 4, 3);
                    sum += src[x * 4] + src[x * 4 + 1] + src[x * 4 + 2];
                }
                if (f)
                    std::fwrite(row.data(), 1, row.size(), f);
            }
            if (f)
                std::fclose(f);
            if (const char* probe = std::getenv("WET_PROBE_PHYS"))  // "addr:len,addr:len" (hex)
            {
                std::string spec(probe);
                for (size_t p = 0; p < spec.size();)
                {
                    size_t e = spec.find(',', p);
                    if (e == std::string::npos) e = spec.size();
                    unsigned a = 0, l = 0;
                    std::sscanf(spec.substr(p, e - p).c_str(), "%x:%x", &a, &l);
                    uint64_t t = 0; unsigned nz = 0;
                    for (unsigned i = 0; i < l; i++) { uint8_t b = s_memory->TranslatePhysical(a)[i]; t += b; nz += b != 0; }
                    if (std::getenv("WET_PROBE_SAVE"))
                    {
                        char fn[64];
                        std::snprintf(fn, sizeof(fn), "probe_%08X_%d.bin", a, n);
                        if (FILE* pf = std::fopen((dir / fn).string().c_str(), "wb"))
                        {
                            std::fwrite(s_memory->TranslatePhysical(a), 1, l, pf);
                            std::fclose(pf);
                        }
                    }
                    char line[160];
                    std::snprintf(line, sizeof(line), "Renderer: probe %08X+%X: sum %llu, nonzero bytes %u", a, l, (unsigned long long)t, nz);
                    wet::renderer_host::Log(line);
                    p = e + 1;
                }
            }
            if (std::getenv("WET_PROBE_FETCH"))
            {
                auto& r = s_graphics->register_file()->values;
                char line[200];
                std::snprintf(line, sizeof(line), "Renderer: RB_COLOR_MASK %08X RB_COLOR_INFO %08X RB_BLENDCONTROL0 %08X RB_COLORCONTROL %08X RB_MODECONTROL %08X SQ_PS_CONST %08X",
                              r[0x2104], r[0x2001], r[0x2201], r[0x2202], r[0x2208], r[0x2181]);
                wet::renderer_host::Log(line);
            }
            if (std::getenv("WET_PROBE_FETCH"))
                for (uint32_t c : { 0u, 1u, 2u, 3u, 254u, 255u })
                {
                    const float* v = reinterpret_cast<const float*>(&s_graphics->register_file()->values[0x4000 + (256 + c) * 4]);
                    char line[160];
                    std::snprintf(line, sizeof(line), "Renderer: ps c%u = %g %g %g %g", c, v[0], v[1], v[2], v[3]);
                    wet::renderer_host::Log(line);
                }
            if (std::getenv("WET_PROBE_FETCH"))
                for (uint32_t t = 0; t < 4; t++)
                {
                    const uint32_t* v = &s_graphics->register_file()->values[0x4800 + t * 6];
                    char line[160];
                    std::snprintf(line, sizeof(line), "Renderer: tf%u = %08X %08X %08X %08X %08X %08X", t, v[0], v[1], v[2], v[3], v[4], v[5]);
                    wet::renderer_host::Log(line);
                }
            char text[160];
            std::snprintf(text, sizeof(text), "Renderer: snapshot %d: %ux%u guest output, average brightness %.1f/255 (%s)", n,
                          image.width, image.height, double(sum) / (3.0 * image.width * image.height),
                          (dir / name).string().c_str());
            wet::renderer_host::Log(text);
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
        // Present from the window's thread on WM_PAINT (Xenia's most conservative mode).
        cvars::host_present_from_non_ui_thread = false;
        if (const char* dir = std::getenv("WET_DUMP_SHADERS"))
            cvars::dump_shaders = dir;
        // Warnings and errors only by default: at the info level Xenia logs every
        // texture upload and shader, megabytes a minute.
        cvars::log_level = 1;
        // Rendering resolution: the game's 1280x720 times the option's scale.
        cvars::draw_resolution_scale_x = int32_t(s_resolutionScale);
        cvars::draw_resolution_scale_y = int32_t(s_resolutionScale);
        if (const char* level = std::getenv("WET_GPU_LOG_LEVEL"))
            cvars::log_level = std::atoi(level);
        xe::InitializeLogging("xenia_gpu");

        uint8_t* physical = renderer_host::PhysicalHostView();
        if (!physical)
            return false;
        s_memory = std::make_unique<xe::Memory>(membase, physical);
        renderer_host::SetPhysicalWriteCallback(OnPhysicalWrite);
        xe::gpu::wet_register_sync_hook = [] { renderer_host::RequestRegisterSync(); };
        s_processor = std::make_unique<xe::cpu::Processor>(s_memory.get());
        s_kernel = std::make_unique<xe::kernel::KernelState>(titleId);
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
        // WET_GPU=d3d12 | vulkan. Direct3D 12 is Xenia's most complete backend and
        // the default on Windows; Vulkan is the fallback (and the only one on Linux).
        std::string want = std::getenv("WET_GPU") ? std::getenv("WET_GPU") : "";
        for (auto& ch : want) ch = char(std::tolower((unsigned char)ch));
        xe::X_STATUS status = xe::X_STATUS(0xC0000001);
#if WET_HAVE_D3D12
        if (want != "vulkan")
        {
            s_backend = "Direct3D 12";
            s_graphics = std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
            status = s_graphics->Setup(s_processor.get(), s_kernel.get(), appContext, true);
            if (XFAILED(status))
            {
                renderer_host::Log("Renderer: Direct3D 12 initialisation failed (see xenia_gpu.log); trying Vulkan");
                s_graphics.reset();
            }
        }
#endif
        if (!s_graphics)
        {
            s_backend = "Vulkan";
            s_graphics = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
            status = s_graphics->Setup(s_processor.get(), s_kernel.get(), appContext, true);
        }
        if (XFAILED(status))
        {
            renderer_host::Log("Renderer: Vulkan initialisation failed (see xenia_gpu.log); using the stand-in GPU");
            s_graphics.reset();
            return false;
        }
#if XE_PLATFORM_WIN32
        if (appContext && s_graphics->presenter())
        {
            appContext->CallInUIThreadSynchronous([] { s_window->window->SetPresenter(s_graphics->presenter()); });
            renderer_host::StartThread(&GameWindow::RepaintThread, s_window, "Repaint");
        }
#endif
        s_running = true;
        char text[160];
        std::snprintf(text, sizeof(text), "Renderer: Xenia GPU emulation on %s started (%s)", s_backend, s_graphics->name().c_str());
        renderer_host::Log(text);
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
            if (std::getenv("WET_GPU_INVALIDATE_ALL"))   // diagnostic: bypass write watches
                OnPhysicalWrite(0, 0x20000000);
            // WET_DRAW_TRACE_AT=<seconds>: trace two frames' draws at that time.
            static const double traceAt = [] {
                const char* v = std::getenv("WET_DRAW_TRACE_AT");
                return v ? std::atof(v) : -1.0;
            }();
            static const auto t0 = std::chrono::steady_clock::now();
            static bool traced = false;
            if (traceAt >= 0 && !traced &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= traceAt)
            {
                traced = true;
                RequestDrawTrace(2);
            }
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

    void SetVsyncRate(uint32_t hz) { xe::gpu::wet_vsync_hz = hz; }

    void SetResolutionScale(uint32_t scale) { s_resolutionScale = std::clamp<uint32_t>(scale, 1, 3); }

    void ToggleFullscreen()
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_window->window)
            s_window->app->CallInUIThread([] {
                s_window->window->SetFullscreen(!s_window->window->IsFullscreen());
            });
#endif
    }

    void RequestDrawTrace(int frames)
    {
        xe::gpu::wet_draw_trace_frames = frames;
    }

    uint32_t ReadRegister(uint32_t index)
    {
        if (!s_graphics || index >= xe::gpu::RegisterFile::kRegisterCount)
            return 0;
        return s_graphics->register_file()->values[index];
    }
}
