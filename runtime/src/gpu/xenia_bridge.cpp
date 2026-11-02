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
#include "wet_version.h"
#include "gpu/renderer.h"
#include "options.h"
#include "os.h"
#include "input.h"
#include "achievements.h"
#include "keybinds.h"
#include "gpu_info.h"
#include "photo.h"
#include <map>
#include <ctime>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <cmath>
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
#include "xenia/gpu/d3d12/pipeline_cache.h"
#endif
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context_win.h"
#include "xenia/ui/window_win.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "third_party/imgui/imgui.h"
#endif

DECLARE_bool(log_to_stdout);
DECLARE_bool(host_present_from_non_ui_thread);
DECLARE_int32(log_level);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
#if WET_HAVE_D3D12
DECLARE_bool(d3d12_allow_variable_refresh_rate_and_tearing);
#endif
DECLARE_bool(vulkan_allow_present_mode_immediate);
DECLARE_uint32(texture_cache_memory_limit_soft);
DECLARE_uint32(texture_cache_memory_limit_hard);
DECLARE_uint32(texture_cache_memory_limit_render_to_texture);
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

    // Width / height of the main monitor (16:9 if unknown).
    float ScreenAspect()
    {
#if XE_PLATFORM_WIN32
        int x, y, w, h;
        if (!wet::os::GameDisplayRect(x, y, w, h))
            w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        if (w > 0 && h > 0)
            return float(w) / float(h);
#endif
        return 16.0f / 9.0f;
    }
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
                uint32_t height = s_resolutionScale >= 2 ? 1080 : 720;
                float aspect = wet::options::Widescreen() ? std::max(16.0f / 9.0f, ScreenAspect()) : 16.0f / 9.0f;
                self->window = xe::ui::Window::Create(*self->app, "WET (recompiled)", uint32_t(height * aspect + 0.5f),
                                                      height);
                ok = self->window && (self->window->AddListener(self), self->window->Open());
                // On the chosen display (wet.ini monitor): centred there, so
                // fullscreen (which uses the window's display) goes there too.
                int mx, my, mw, mh;
                if (ok && !wet::options::Monitor().empty() && wet::os::GameDisplayRect(mx, my, mw, mh))
                {
                    HWND hwnd = static_cast<xe::ui::Win32Window*>(self->window.get())->hwnd();
                    RECT r;
                    GetWindowRect(hwnd, &r);
                    int ww = std::min<int>(r.right - r.left, mw), wh = std::min<int>(r.bottom - r.top, mh);
                    SetWindowPos(hwnd, nullptr, mx + (mw - ww) / 2, my + (mh - wh) / 2, ww, wh,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
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
#if WET_HAVE_D3D12
                // Every 10 s: draws skipped while their shader was compiling.
                static auto lastReport = std::chrono::steady_clock::now();
                static uint64_t reported = 0;
                if (now - lastReport > std::chrono::seconds(10))
                {
                    lastReport = now;
                    uint64_t skipped = xe::gpu::d3d12::wet_skipped_draws.load();
                    if (skipped != reported)
                    {
                        char text[128];
                        std::snprintf(text, sizeof(text), "Renderer: %llu draws skipped while their shaders compiled (%llu in total)",
                                      (unsigned long long)(skipped - reported), (unsigned long long)skipped);
                        wet::renderer_host::Log(text);
                        reported = skipped;
                    }
                }
#endif
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

    // ---- Settings overlay (F1) and FPS counter, drawn with Xenia's Dear ImGui ----
    std::unique_ptr<xe::ui::ImmediateDrawer> s_immediateDrawer;
    std::unique_ptr<xe::ui::ImGuiDrawer> s_imgui;
    std::atomic<bool> s_overlayOpen{ false };
    std::atomic<float> s_quitHold{ -1.0f };   // pause menu quit hint (SetQuitHold)
    class OverlayDialog;
    OverlayDialog* s_overlay = nullptr;   // UI thread only
    bool s_nisUnsupported = false;        // UI thread only

    // Achievement pop-ups and icons (UI thread only).
    constexpr double kPopupSeconds = 6.0;
    bool s_popupActive = false;
    wet::achievements::Achievement s_popup;
    std::chrono::steady_clock::time_point s_popupStart;
    std::map<uint32_t, std::unique_ptr<xe::ui::ImmediateTexture>> s_icons;

    xe::ui::ImmediateTexture* AchievementIcon(uint32_t imageId)
    {
        auto it = s_icons.find(imageId);
        if (it != s_icons.end())
            return it->second.get();
        std::unique_ptr<xe::ui::ImmediateTexture> texture;
        auto png = wet::achievements::Image(imageId);
        int w = 0, h = 0, n = 0;
        if (!png.empty() && s_immediateDrawer)
            if (stbi_uc* rgba = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4))
            {
                texture = s_immediateDrawer->CreateTexture(uint32_t(w), uint32_t(h),
                                                           xe::ui::ImmediateTextureFilter::kLinear, false, rgba);
                stbi_image_free(rgba);
            }
        auto* raw = texture.get();
        s_icons[imageId] = std::move(texture);
        return raw;
    }

    bool PopupPending()
    {
        return s_popupActive || (wet::options::AchievementPopups() && wet::achievements::HasPopup());
    }

    // Post-processing from the options (UI thread).
    void ApplyPostProcessing()
    {
        xe::gpu::wet_film_grain = float(wet::options::FilmGrain()) / 100.0f;
#if WET_HAVE_D3D12
        xe::gpu::d3d12::wet_async_pipelines = wet::options::AsyncShaders();
#endif
        if (!s_graphics)
            return;
        if (auto* cp = s_graphics->command_processor())
            cp->SetDesiredSwapPostEffect(xe::gpu::CommandProcessor::SwapPostEffect(wet::options::Antialiasing()));
        if (auto* presenter = s_graphics->presenter())
        {
            auto config = presenter->GetGuestOutputPaintConfigFromUIThread();
            config.SetEffect(xe::ui::Presenter::GuestOutputPaintConfig::Effect(wet::options::Filter()));
            config.SetNisSharpness(float(wet::options::Sharpness()) / 100.0f);
            presenter->SetGuestOutputPaintConfigFromUIThread(config);
            s_nisUnsupported = !presenter->IsNisSupported();
        }
    }

    class OverlayDialog final : public xe::ui::ImGuiDialog
    {
    public:
        explicit OverlayDialog(xe::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

    protected:
        void OnClose() override { s_overlay = nullptr; }

        void OnDraw(ImGuiIO& io) override
        {
            // Readable at any window size (Xenia's font is 10 px).
            float scale = std::max(1.5f, io.DisplaySize.y / 540.0f);
            io.FontGlobalScale = scale;

            UpdateFps();
            if (wet::options::ShowFps())
            {
                ImGui::SetNextWindowPos(ImVec2(8 * scale, 8 * scale));
                ImGui::SetNextWindowBgAlpha(0.45f);
                ImGui::Begin("##fps", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav);
                ImGui::Text("%.0f FPS", fps_);
                ImGui::End();
            }

            DrawPopup(io, scale);

            if (s_overlayOpen)
                DrawSettings(io, scale);
            else
            {
                DrawQuitHint(io, scale);
                DrawPhoto(io, scale);
            }

            if (!s_overlayOpen && !wet::options::ShowFps() && !PopupPending() && s_quitHold < 0 &&
                !wet::photo::Active() && wet::photo::Toast().empty())
                Close();
        }

    private:
        void UpdateFps()
        {
            auto now = std::chrono::steady_clock::now();
            uint32_t frames = s_graphics && s_graphics->command_processor() ? s_graphics->command_processor()->wet_swap_count() : 0;
            double dt = std::chrono::duration<double>(now - fpsStart_).count();
            if (dt >= 0.5)
            {
                fps_ = (frames - fpsFrames_) / dt;
                fpsFrames_ = frames;
                fpsStart_ = now;
            }
        }

        void DrawPopup(ImGuiIO& io, float scale)
        {
            if (!s_popupActive)
            {
                if (!wet::options::AchievementPopups())
                {
                    wet::achievements::Achievement skip;
                    while (wet::achievements::PopPopup(skip)) {}
                    return;
                }
                if (!wet::achievements::PopPopup(s_popup))
                    return;
                s_popupActive = true;
                s_popupStart = std::chrono::steady_clock::now();
            }
            double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_popupStart).count();
            if (t >= kPopupSeconds)
            {
                s_popupActive = false;
                return;
            }
            float alpha = float(std::min({ t / 0.3, (kPopupSeconds - t) / 0.5, 1.0 }));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * scale);
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24 * scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##achievement", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoSavedSettings);
            if (auto* icon = AchievementIcon(s_popup.imageId))
            {
                float size = 40 * scale;
                ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(size, size));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.35f, 1.0f), "Achievement unlocked - %uG", s_popup.gamerscore);
            ImGui::TextUnformatted(s_popup.name.c_str());
            if (!s_popup.description.empty())
                ImGui::TextDisabled("%s", s_popup.description.c_str());
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopStyleVar(2);
        }

        void DrawAchievements(float scale)
        {
            auto list = wet::achievements::List();
            if (list.empty())
            {
                ImGui::TextDisabled("No achievement data found in default.xex");
                return;
            }
            uint32_t unlocked = 0, score = 0, total = 0;
            for (auto& a : list)
            {
                total += a.gamerscore;
                if (a.unlocked)
                {
                    unlocked++;
                    score += a.gamerscore;
                }
            }
            ImGui::Text("%u of %zu unlocked, %u / %uG", unlocked, list.size(), score, total);
            bool popups = wet::options::AchievementPopups();
            if (ImGui::Checkbox("Show pop-ups", &popups))
                wet::options::SetAchievementPopups(popups);
            if (!ImGui::CollapsingHeader("Achievement list", ImGuiTreeNodeFlags_DefaultOpen))
                return;
            ImGui::BeginChild("##achievements", ImVec2(560 * scale / 1.5f, 220 * scale / 1.5f), true);
            for (auto& a : list)
            {
                if (auto* icon = AchievementIcon(a.imageId))
                {
                    ImVec4 tint = a.unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.35f, 0.35f, 0.35f, 1);
                    ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(28 * scale, 28 * scale), ImVec2(0, 0),
                                 ImVec2(1, 1), tint);
                    ImGui::SameLine();
                }
                ImGui::BeginGroup();
                bool hide = !a.unlocked && a.secret;
                if (a.unlocked)
                    ImGui::Text("%s  (%uG)", a.name.c_str(), a.gamerscore);
                else
                    ImGui::TextDisabled("%s  (%uG)", hide ? "Secret achievement" : a.name.c_str(), a.gamerscore);
                const std::string& text = a.unlocked ? a.description : a.lockedDescription;
                if (!hide && !text.empty())
                    ImGui::TextDisabled("%s", text.c_str());
                if (a.unlocked && a.unlockTime)
                {
                    std::time_t t = std::time_t(a.unlockTime);
                    char when[32] = {};
                    if (std::tm* tm = std::localtime(&t))
                        std::strftime(when, sizeof(when), "%d %b %Y %H:%M", tm);
                    ImGui::TextDisabled("Unlocked %s", when);
                }
                ImGui::EndGroup();
            }
            ImGui::EndChild();
        }

        // Keyboard bindings: click an action, press a key. Right click clears it.
        static void DrawKeyboard()
        {
            namespace kb = wet::keybinds;
            static int capture = -1;
            static bool held[kb::kKeyCount];
            if (!ImGui::CollapsingHeader("Keyboard bindings"))
            {
                capture = -1;
                return;
            }
#if XE_PLATFORM_WIN32
            if (capture >= 0)
            {
                // The first key that goes down after the click (keys already held,
                // like the Enter that pressed the button, don't count).
                for (int k = 0; k < kb::kKeyCount; k++)
                {
                    bool down = (GetAsyncKeyState(kb::kKeys[k].vk) & 0x8000) != 0;
                    if (down && !held[k] && capture >= 0)
                    {
                        wet::options::SetKeyBinding(capture, kb::kKeys[k].name);
                        capture = -1;
                    }
                    held[k] = down;
                }
            }
#endif
            if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_SizingFixedFit))
            {
                for (int i = 0; i < kb::kActionCount; i++)
                {
                    if (i % 2 == 0)
                        ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kb::kActions[i].label);
                    ImGui::TableNextColumn();
                    std::string keys = wet::options::KeyBinding(i);
                    std::string text = capture == i ? "Press a key..." : keys.empty() ? "NONE" : keys;
                    text += "##key" + std::to_string(i);
                    if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetFontSize() * 9, 0)))
                    {
                        capture = capture == i ? -1 : i;
#if XE_PLATFORM_WIN32
                        for (int k = 0; k < kb::kKeyCount; k++)
                            held[k] = (GetAsyncKeyState(kb::kKeys[k].vk) & 0x8000) != 0;
#endif
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    {
                        wet::options::SetKeyBinding(i, "NONE");
                        capture = -1;
                    }
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("Reset keyboard to defaults"))
            {
                for (int i = 0; i < kb::kActionCount; i++)
                    wet::options::SetKeyBinding(i, kb::kActions[i].fallback);
                capture = -1;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Click, then press a key; right click clears. Two keys: wet.ini key_* = Q, F");
        }

        // Photo mode's controls (bottom left) and messages such as "Screenshot saved"
        // (top middle). Screenshots are taken from the game's output, without these.
        static void DrawPhoto(ImGuiIO& io, float scale)
        {
            std::string toast = wet::photo::Toast();
            auto st = wet::photo::GetStatus();
            if (!st.active && toast.empty())
                return;
            ApplyStyle(scale);
            float em = ImGui::GetFontSize();
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
            if (!toast.empty())
            {
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, em * 1.5f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
                ImGui::SetNextWindowBgAlpha(0.85f);
                ImGui::Begin("##toast", nullptr, flags);
                ImGui::TextUnformatted(toast.c_str());
                ImGui::End();
            }
            if (!st.active)
                return;
            ImGui::SetNextWindowPos(ImVec2(em, io.DisplaySize.y - em), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##photo", nullptr, flags);
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "PHOTO MODE");
            ImGui::SameLine();
            ImGui::TextDisabled("  FOV %.0f%s", st.fov, st.fast ? "  - fast" : "");
            ImGui::Spacing();
            auto row = [](const char* what, const char* keys, const char* pad) {
                ImGui::TextUnformatted(what);
                ImGui::SameLine(ImGui::GetFontSize() * 7);
                ImGui::TextDisabled("%s", keys);
                ImGui::SameLine(ImGui::GetFontSize() * 17);
                ImGui::TextDisabled("%s", pad);
            };
            row("Move", "WASD", "Left stick");
            row("Look", "Mouse", "Right stick");
            row("Up / down", "E / Q", "RB / LB");
            row("Faster", "Hold Shift", "Hold L3");
            row("Zoom in / out", "Left / right click", "RT / LT");
            row(st.hudHidden ? "Show HUD" : "Hide HUD", "Y", "Y");
            row("Screenshot", "Space or F12", "A");
            row("Reset camera", "X", "X");
            row("Exit", "Esc or F5", "B or L3 + R3");
            ImGui::End();
        }

        // Pause menu: "Hold Back to quit to desktop", with the hold's progress.
        static void DrawQuitHint(ImGuiIO& io, float scale)
        {
            float progress = s_quitHold;
            if (progress < 0)
                return;
            ApplyStyle(scale);
            float em = ImGui::GetFontSize();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - em * 2.5f), ImGuiCond_Always,
                                    ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(progress > 0 ? 0.9f : 0.6f);
            ImGui::Begin("##quit", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
            // A small "exit door" icon: a frame with an arrow out of it.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float s = em, t = std::max(1.0f, em * 0.09f);
            ImU32 col = ImGui::GetColorU32(progress > 0 ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.92f, 0.92f, 0.93f, 1.0f));
            draw->AddLine(ImVec2(p.x + s * 0.55f, p.y + s * 0.1f), ImVec2(p.x + s * 0.1f, p.y + s * 0.1f), col, t);
            draw->AddLine(ImVec2(p.x + s * 0.1f, p.y + s * 0.1f), ImVec2(p.x + s * 0.1f, p.y + s * 0.9f), col, t);
            draw->AddLine(ImVec2(p.x + s * 0.1f, p.y + s * 0.9f), ImVec2(p.x + s * 0.55f, p.y + s * 0.9f), col, t);
            draw->AddLine(ImVec2(p.x + s * 0.38f, p.y + s * 0.5f), ImVec2(p.x + s * 0.95f, p.y + s * 0.5f), col, t);
            draw->AddLine(ImVec2(p.x + s * 0.75f, p.y + s * 0.3f), ImVec2(p.x + s * 0.95f, p.y + s * 0.5f), col, t);
            draw->AddLine(ImVec2(p.x + s * 0.75f, p.y + s * 0.7f), ImVec2(p.x + s * 0.95f, p.y + s * 0.5f), col, t);
            ImGui::Dummy(ImVec2(s, s));
            ImGui::SameLine();
            auto pads = wet::ConnectedControllers();
            bool keyboard = std::all_of(pads.begin(), pads.end(), [](const std::string& n) { return n.empty(); });
            ImGui::Text("Hold %s to quit to desktop", keyboard ? "Backspace" : "Back");
            if (progress > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
                ImGui::ProgressBar(std::min(progress, 1.0f), ImVec2(-FLT_MIN, em * 0.3f), "");
                ImGui::PopStyleColor();
            }
            ImGui::End();
        }

        // ---- Settings window (F1) ----
        // Tabs of label / control rows; longer explanations are in (?) tooltips.

        // The overlay's look; sizes follow the font scale (the text is scaled
        // with FontGlobalScale, which leaves padding and rounding alone).
        static void ApplyStyle(float scale)
        {
            static float applied = 0;
            if (applied == scale)
                return;
            applied = scale;
            ImGuiStyle& st = ImGui::GetStyle();
            st = ImGuiStyle();
            st.WindowRounding = 10;
            st.ChildRounding = 8;
            st.FrameRounding = 6;
            st.PopupRounding = 8;
            st.GrabRounding = 6;
            st.TabRounding = 6;
            st.ScrollbarRounding = 8;
            st.WindowBorderSize = 0;
            st.FrameBorderSize = 0;
            st.WindowPadding = ImVec2(14, 12);
            st.FramePadding = ImVec2(8, 4);
            st.ItemSpacing = ImVec2(10, 6);
            st.ItemInnerSpacing = ImVec2(6, 4);
            st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
            ImVec4* c = st.Colors;
            const ImVec4 bg(0.075f, 0.08f, 0.095f, 0.97f), panel(0.13f, 0.14f, 0.16f, 1.0f),
                panelHi(0.19f, 0.2f, 0.23f, 1.0f), accent(0.78f, 0.16f, 0.17f, 1.0f), accentHi(0.9f, 0.24f, 0.24f, 1.0f),
                text(0.92f, 0.92f, 0.93f, 1.0f), dim(0.56f, 0.58f, 0.62f, 1.0f);
            c[ImGuiCol_Text] = text;
            c[ImGuiCol_TextDisabled] = dim;
            c[ImGuiCol_WindowBg] = bg;
            c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = ImVec4(0.1f, 0.105f, 0.12f, 0.98f);
            c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
            c[ImGuiCol_FrameBg] = panel;
            c[ImGuiCol_FrameBgHovered] = panelHi;
            c[ImGuiCol_FrameBgActive] = panelHi;
            c[ImGuiCol_TitleBg] = bg;
            c[ImGuiCol_TitleBgActive] = bg;
            c[ImGuiCol_TitleBgCollapsed] = bg;
            c[ImGuiCol_CheckMark] = accentHi;
            c[ImGuiCol_SliderGrab] = accent;
            c[ImGuiCol_SliderGrabActive] = accentHi;
            c[ImGuiCol_Button] = panel;
            c[ImGuiCol_ButtonHovered] = panelHi;
            c[ImGuiCol_ButtonActive] = accent;
            c[ImGuiCol_Header] = panel;
            c[ImGuiCol_HeaderHovered] = panelHi;
            c[ImGuiCol_HeaderActive] = accent;
            c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
            c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TabHovered] = panelHi;
            c[ImGuiCol_TabActive] = accent;
            c[ImGuiCol_TabUnfocused] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TabUnfocusedActive] = accent;
            c[ImGuiCol_PlotHistogram] = accent;
            c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_ScrollbarGrab] = panel;
            c[ImGuiCol_ScrollbarGrabHovered] = panelHi;
            c[ImGuiCol_ScrollbarGrabActive] = accent;
            c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
            st.ScaleAllSizes(scale / 1.5f);
        }

        static bool BeginRows(const char* id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
                return false;
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 15.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        // A row: the label (with an optional (?) tooltip), then the control.
        static void Row(const char* label, const char* help = nullptr)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (help)
            {
                ImGui::SameLine(0, 4);
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
                    ImGui::TextUnformatted(help);
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        static bool RowCheck(const char* label, bool* v, const char* help = nullptr)
        {
            Row(label, help);
            ImGui::PushID(label);
            bool changed = ImGui::Checkbox("##v", v);
            ImGui::PopID();
            return changed;
        }

        static bool RowCombo(const char* label, int* index, const char* const* items, int count, const char* help = nullptr)
        {
            Row(label, help);
            ImGui::PushID(label);
            bool changed = ImGui::Combo("##v", index, items, count);
            ImGui::PopID();
            return changed;
        }

        static void Note(const char* text)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s", text);
            ImGui::PopTextWrapPos();
        }

        void TabGeneral()
        {
            if (!BeginRows("general"))
                return;
            {
                static const int kIds[] = { 0, 1, 3, 4, 5, 6 };
                static const char* kNames[] = { "Automatic (Windows)", "English", "Deutsch", "Fran\xC3\xA7" "ais",
                                                "Espa\xC3\xB1" "ol", "Italiano" };
                int setting = wet::options::LanguageSetting(), pick = 0;
                for (int i = 0; i < 6; i++)
                    if (kIds[i] == setting)
                        pick = i;
                if (RowCombo("Language", &pick, kNames, 6, "Text and speech. Takes effect at the next start."))
                    wet::options::SetLanguage(kIds[pick]);
            }
            {
                static const char* kNames[] = { "Automatic", "Xbox", "PlayStation", "Keyboard / mouse" };
                int pick = wet::options::ButtonPrompts();
                if (RowCombo("Button prompts", &pick, kNames, 4,
                             "Automatic: keyboard and mouse with no controller connected at startup, PlayStation "
                             "symbols with a PlayStation controller, Xbox otherwise. Takes effect at the next start."))
                    wet::options::SetButtonPrompts(pick);
            }
            bool showFps = wet::options::ShowFps();
            if (RowCheck("FPS counter", &showFps))
                wet::options::SetShowFps(showFps);
            ImGui::EndTable();
        }

        void TabDisplay()
        {
            if (!BeginRows("display"))
                return;
            bool fullscreen = s_window && s_window->window && s_window->window->IsFullscreen();
            if (RowCheck("Fullscreen", &fullscreen, "Also F11."))
            {
                // Not while painting: switching resizes the swap chain the
                // presenter is drawing into (that crashed in the driver).
                s_window->app->CallInUIThreadDeferred([fullscreen] { s_window->window->SetFullscreen(fullscreen); });
                wet::options::SetFullscreen(fullscreen);
            }
            {
                static const char* kNames[] = { "720p (original)", "1440p", "2160p (4K)" };
                int pick = wet::options::ResolutionScale() - 1;
                if (RowCombo("Resolution", &pick, kNames, 3, "The game renders at this resolution; takes effect at the next start."))
                    wet::options::SetResolutionScale(pick + 1);
                if (wet::options::ResolutionScale() != wet::options::RunningResolutionScale())
                    Note("Restart the game to change the resolution.");
            }
            bool wide = wet::options::Widescreen();
            if (RowCheck("Widescreen / ultrawide", &wide, "Fills 21:9 and 32:9 screens with a wider view. Sharpest after a restart."))
                wet::options::SetWidescreen(wide);
            if (wide)
            {
                bool hud = wet::options::WidescreenHud();
                if (RowCheck("HUD in the middle", &hud, "Keeps the HUD and menus 16:9 in the middle of a wider screen."))
                    wet::options::SetWidescreenHud(hud);
            }
            {
                int fov = wet::options::Fov(), pick = fov;
                Row("Field of view", "Horizontal, at 16:9. 90 is the game's own.");
                ImGui::SliderInt("##fov", &pick, 60, 130, "%d\xC2\xB0");
                if (pick != fov)
                    wet::options::SetFov(pick);
            }
            bool vsync = wet::options::Vsync();
            if (RowCheck("V-Sync", &vsync, "No tearing. Turn off for G-Sync / FreeSync. Takes effect at the next start."))
                wet::options::SetVsync(vsync);
            ImGui::EndTable();
        }

        void TabGraphics()
        {
            if (!BeginRows("graphics"))
                return;
            int aa = wet::options::Antialiasing(), aaPick = aa;
            int filter = wet::options::Filter(), filterPick = filter;
            int sharp = wet::options::Sharpness(), sharpPick = sharp;
            {
                static const char* kNames[] = { "Off", "FXAA", "FXAA (extreme)", "SMAA (sharper)" };
                RowCombo("Anti-aliasing", &aaPick, kNames, 4);
            }
            {
                static const char* kNames[] = { "Bilinear", "AMD CAS (sharpen)", "AMD FSR (upscale)", "NVIDIA NIS (upscale)" };
                RowCombo("Scaling", &filterPick, kNames, 4, "How the image is scaled to the window or screen.");
            }
            if (filterPick == 3)
            {
                Row("NIS sharpness");
                ImGui::SliderInt("##nis", &sharpPick, 0, 100, "%d%%");
                if (s_nisUnsupported)
                    Note("NIS needs the Direct3D 12 renderer; AMD FSR is used instead.");
            }
            if (aaPick != aa || filterPick != filter || sharpPick != sharp)
            {
                wet::options::SetAntialiasing(aaPick);
                wet::options::SetFilter(filterPick);
                wet::options::SetSharpness(sharpPick);
                ApplyPostProcessing();
            }
            {
                int grain = wet::options::FilmGrain(), pick = grain;
                Row("Film grain");
                ImGui::SliderInt("##grain", &pick, 0, 100, "%d%%");
                if (pick != grain)
                {
                    wet::options::SetFilmGrain(pick);
                    ApplyPostProcessing();
                }
            }
            bool blur = wet::options::MotionBlur(), dof = wet::options::DepthOfField(), shake = wet::options::CameraShake();
            if (RowCheck("Motion blur", &blur, "Off is also a little faster on slower graphics cards."))
                wet::options::SetMotionBlur(blur);
            if (RowCheck("Depth of field", &dof, "Off is also a little faster on slower graphics cards."))
                wet::options::SetDepthOfField(dof);
            if (RowCheck("Camera shake", &shake, "Including the shaky film effect that goes with it."))
                wet::options::SetCameraShake(shake);
            bool borders = wet::options::HideBorders();
            if (RowCheck("Hide screen-edge frame", &borders, "The dark vignette / cinematic frame at the edges of the screen."))
                wet::options::SetHideBorders(borders);
            ImGui::EndTable();
        }

        void TabPerformance()
        {
            if (!BeginRows("performance"))
                return;
            {
                // Choices, then "Custom" with a slider for any other rate.
                static const int kRates[] = { 30, 60, 120, 144, 165, 240, 360 };
                int fps = wet::options::ConfiguredFps();
                char monitor[40];
                std::snprintf(monitor, sizeof(monitor), "Match monitor (%d Hz)", wet::options::MonitorRefreshRate());
                const char* names[] = { monitor, "30 (original)", "60", "120", "144", "165", "240", "360", "Custom" };
                static bool custom = false;
                int index = 8;
                if (fps == 0)
                    index = 0;
                for (int i = 0; i < 7; i++)
                    if (fps == kRates[i])
                        index = i + 1;
                if (custom)
                    index = 8;
                if (RowCombo("Frame rate", &index, names, 9, "The game's own speed is 30 FPS; higher rates keep game time correct."))
                {
                    custom = index == 8;
                    if (index == 0)
                        wet::options::SetFps(0);
                    else if (index <= 7)
                        wet::options::SetFps(kRates[index - 1]);
                }
                if (index == 8)
                {
                    int rate = fps ? fps : wet::options::MonitorRefreshRate(), pick = rate;
                    Row("Custom rate");
                    ImGui::SliderInt("##rate", &pick, 30, 360, "%d FPS");
                    if (pick != rate)
                        wet::options::SetFps(pick);
                }
                int running = wet::options::Fps(), wanted = fps ? fps : wet::options::MonitorRefreshRate();
                if (running != wanted)
                {
                    char text[80];
                    std::snprintf(text, sizeof(text), "A video or cutscene is playing: %d FPS until it ends.", running);
                    Note(text);
                }
            }
            bool videoMode = wet::options::VideoMode();
            if (RowCheck("Videos at 30 FPS", &videoMode, "Avoids flicker in the game's videos."))
                wet::options::SetVideoMode(videoMode);
            bool cutsceneFps = wet::options::CutsceneFps30();
            if (RowCheck("Cutscenes at 30 FPS", &cutsceneFps, "In-game cutscenes as on the console, which keeps speech in sync."))
                wet::options::SetCutsceneFps30(cutsceneFps);
            bool asyncShaders = wet::options::AsyncShaders();
            if (RowCheck("Don't wait for shaders", &asyncShaders,
                         "Less stutter the first time something is drawn; an object may appear a moment late instead."))
            {
                wet::options::SetAsyncShaders(asyncShaders);
                ApplyPostProcessing();
            }
            bool adaptive = wet::options::AdaptiveWatch();
            if (RowCheck("Fast memory tracking", &adaptive, "Turn off if textures or models glitch."))
                wet::options::SetAdaptiveWatchOption(adaptive);
            bool gpuWait = wet::options::GpuWait();
            if (RowCheck("Low-CPU GPU wait", &gpuWait, "The render thread sleeps while it waits for the GPU instead of spinning."))
                wet::options::SetGpuWait(gpuWait);
            ImGui::EndTable();
        }

        void TabControls()
        {
            auto pads = wet::ConnectedControllers();
            bool any = false;
            for (size_t i = 0; i < pads.size(); i++)
                if (!pads[i].empty())
                {
                    ImGui::TextDisabled("Player %zu", i + 1);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(pads[i].c_str());
                    any = true;
                }
            if (!any)
                ImGui::TextDisabled("No controller connected - keyboard and mouse");
            ImGui::Spacing();
            if (BeginRows("controls"))
            {
                bool vibration = wet::options::Vibration();
                if (RowCheck("Vibration", &vibration))
                    wet::options::SetVibration(vibration);
                bool aim = wet::options::MouseAim();
                if (RowCheck("Mouse aim", &aim, "The mouse turns the camera and aims. F1 or Alt+Tab frees the cursor."))
                    wet::options::SetMouseAim(aim);
                int sens = wet::options::MouseSensitivity(), pick = sens;
                Row("Mouse sensitivity");
                ImGui::SliderInt("##sens", &pick, 1, 200);
                if (pick != sens)
                    wet::options::SetMouseSensitivity(pick);
                bool invert = wet::options::MouseInvertY();
                if (RowCheck("Invert mouse Y", &invert))
                    wet::options::SetMouseInvertY(invert);
                bool direct = wet::options::MouseDirect();
                if (RowCheck("Direct mouse aim", &direct, "Outside the normal third-person camera (turrets, vehicles, menus): turns by how far the mouse moves, with no stick speed limit. Off: the mouse acts as a stick there."))
                    wet::options::SetMouseDirect(direct);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            DrawKeyboard();
        }

        void DrawSettings(ImGuiIO& io, float scale)
        {
            ApplyStyle(scale);
            float em = ImGui::GetFontSize();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
            float width = std::min(em * 44, io.DisplaySize.x * 0.95f);
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, io.DisplaySize.y * 0.9f));
            bool open = true;
            if (!ImGui::Begin("WET Recompiled " WET_VERSION "###settings", &open,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::End();
                return;
            }
            if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyResizeDown))
            {
                if (ImGui::BeginTabItem("General")) { ImGui::Spacing(); TabGeneral(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Display")) { ImGui::Spacing(); TabDisplay(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Graphics")) { ImGui::Spacing(); TabGraphics(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Performance")) { ImGui::Spacing(); TabPerformance(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controls")) { ImGui::Spacing(); TabControls(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Achievements")) { ImGui::Spacing(); DrawAchievements(scale); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Saved automatically  \xC2\xB7  F1 closes");
            float quitWidth = ImGui::CalcTextSize("Quit to desktop").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - quitWidth);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.17f, 0.18f, 1.0f));
            if (ImGui::Button("Quit to desktop"))
                ImGui::OpenPopup("Quit WET?");
            ImGui::PopStyleColor(2);
            if (ImGui::BeginPopupModal("Quit WET?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted("Progress since the last checkpoint will be lost.");
                ImGui::Spacing();
                if (ImGui::Button("Quit", ImVec2(em * 7, 0)))
                {
                    wet::renderer_host::Log("Quit to desktop (settings)");
                    wet::renderer_host::ExitProcess();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(em * 7, 0)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::End();
            if (!open)
                s_overlayOpen = false;
        }

        double fps_ = 0;
        uint32_t fpsFrames_ = 0;
        std::chrono::steady_clock::time_point fpsStart_ = std::chrono::steady_clock::now();
    };

    // UI thread: make sure the overlay dialog exists if anything needs drawing.
    void UpdateOverlay()
    {
        if (!s_imgui)
            return;
        if (!s_overlay && (s_overlayOpen || wet::options::ShowFps() || PopupPending() || s_quitHold >= 0 ||
                           wet::photo::Active() || !wet::photo::Toast().empty()))
            s_overlay = new OverlayDialog(s_imgui.get());
    }

    // UI thread, after the presenter is attached.
    void SetUpOverlay()
    {
        auto* presenter = s_graphics->presenter();
        s_immediateDrawer = s_graphics->provider()->CreateImmediateDrawer();
        if (!s_immediateDrawer)
        {
            wet::renderer_host::Log("Renderer: no immediate drawer - settings overlay unavailable");
            return;
        }
        s_immediateDrawer->SetPresenter(presenter);
        s_imgui = std::make_unique<xe::ui::ImGuiDrawer>(s_window->window.get(), 64);
        s_imgui->SetPresenterAndImmediateDrawer(presenter, s_immediateDrawer.get());
        ApplyPostProcessing();
        if (wet::options::Fullscreen())
            s_window->app->CallInUIThreadDeferred([] { s_window->window->SetFullscreen(true); });
        UpdateOverlay();
    }
#endif

    // F8 burst capture: every newly presented frame for the next 48 frames, kept in
    // memory while capturing (so saving doesn't slow the capture), then written as
    // frames/burst_<run>_<index>_swap<count>.png.
    std::atomic<bool> s_burstRunning{ false };

    void BurstThread(void*)
    {
        static int run = 0;
        run++;
        struct Shot { uint32_t swap; xe::ui::RawImage image; };
        wet::renderer_host::StartWatchTrace();
        std::vector<Shot> shots;
        uint32_t last = 0xFFFFFFFF;
        auto start = std::chrono::steady_clock::now();
        while (shots.size() < 48 && std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
        {
            auto* cp = s_graphics ? s_graphics->command_processor() : nullptr;
            xe::ui::Presenter* presenter = s_graphics ? s_graphics->presenter() : nullptr;
            uint32_t swap = cp ? cp->wet_swap_count() : 0;
            if (!presenter || swap == last)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
            last = swap;
            wet::renderer_host::TraceEvent('C', swap, uint32_t(shots.size()));
            Shot shot{ swap, {} };
            if (presenter->CaptureGuestOutput(shot.image) && shot.image.width)
                shots.push_back(std::move(shot));
        }
        std::error_code ec;
        std::filesystem::create_directories("frames", ec);
        {
            char traceName[64];
            std::snprintf(traceName, sizeof(traceName), "frames/burst_%d_trace.txt", run);
            wet::renderer_host::StopWatchTrace(traceName);
        }
        for (size_t i = 0; i < shots.size(); i++)
        {
            auto& img = shots[i].image;
            // RawImage is RGBA (alpha ignored); stb wants tightly packed RGB.
            std::vector<uint8_t> rgb(size_t(img.width) * img.height * 3);
            for (uint32_t y = 0; y < img.height; y++)
                for (uint32_t x = 0; x < img.width; x++)
                    std::memcpy(&rgb[(size_t(y) * img.width + x) * 3], img.data.data() + y * img.stride + x * 4, 3);
            char name[96];
            std::snprintf(name, sizeof(name), "frames/burst_%d_%02zu_swap%u.png", run, i, shots[i].swap);
            stbi_write_png(name, int(img.width), int(img.height), 3, rgb.data(), int(img.width) * 3);
        }
        char text[128];
        std::snprintf(text, sizeof(text), "Renderer: F8 burst %d: saved %zu frames to frames\\burst_%d_*.png", run, shots.size(), run);
        wet::renderer_host::Log(text);
        s_burstRunning = false;
    }

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

    std::string s_shaderCacheRoot;
    void SetShaderCacheRoot(const std::string& path) { s_shaderCacheRoot = path; }

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
        cvars::texture_cache_memory_limit_render_to_texture = 64;
        // V-Sync option: no tearing (otherwise frames are shown at once, for VRR).
#if WET_HAVE_D3D12
        cvars::d3d12_allow_variable_refresh_rate_and_tearing = !wet::options::Vsync();
#endif
        cvars::vulkan_allow_present_mode_immediate = !wet::options::Vsync();
        // Widescreen: the 16:9 frame is shown stretched across the wider screen
        // (with the game's view widened to match), so render more columns to keep
        // it sharp: X scale = Y scale x how much wider than 16:9 the screen is.
        if (float wide = ScreenAspect() / (16.0f / 9.0f); wet::options::Widescreen() && wide > 1.01f)
        {
            int x = int(std::ceil(float(s_resolutionScale) * wide - 0.1f));
            cvars::draw_resolution_scale_x = std::clamp(x, int(s_resolutionScale), 6);
            char text[128];
            std::snprintf(text, sizeof(text), "Renderer: widescreen %.2f:1 - rendering at %dx%d", ScreenAspect(),
                          1280 * cvars::draw_resolution_scale_x, 720 * cvars::draw_resolution_scale_y);
            wet::renderer_host::Log(text);
        }
        // Texture memory: Xenia's defaults (384 MB soft, 768 MB hard) are for 720p;
        // at 1440p / 4K the render-to-texture targets grow with the scale squared
        // and textures got destroyed while still being drawn (surfaces flashing
        // black). Up to 2 GB soft / 4 GB hard, but within about half of the
        // card's video memory, so a 4 GB card doesn't spill into system memory
        // (which shows up as stutter).
        {
            gpu_info::Info gpu = gpu_info::Query();
            uint32_t soft, hard;
            gpu_info::TextureCacheLimits(gpu, cvars::draw_resolution_scale_x, cvars::draw_resolution_scale_y,
                                         uint32_t(cvars::texture_cache_memory_limit_render_to_texture), soft, hard);
            cvars::texture_cache_memory_limit_soft = soft;
            cvars::texture_cache_memory_limit_hard = hard;
            char text[256];
            std::snprintf(text, sizeof(text),
                          "Renderer: %s, %llu MB video memory (%llu MB available to the game); texture cache %u / %u MB",
                          gpu.found ? gpu.name.c_str() : "graphics card unknown", (unsigned long long)gpu.dedicatedMB,
                          (unsigned long long)gpu.budgetMB, soft, hard);
            wet::renderer_host::Log(text);
        }
        if (const char* level = std::getenv("WET_GPU_LOG_LEVEL"))
            cvars::log_level = std::atoi(level);
        xe::InitializeLogging("xenia_gpu");

        uint8_t* physical = renderer_host::PhysicalHostView();
        if (!physical)
            return false;
        s_memory = std::make_unique<xe::Memory>(membase, physical);
        renderer_host::SetPhysicalWriteCallback(OnPhysicalWrite);
        // Diagnostics: which shader / constant the film grain slider scales.
        if (const char* v = std::getenv("WET_GRAIN_PS"))
            xe::gpu::wet_grain_ps_hash = std::strtoull(v, nullptr, 16);
        if (const char* v = std::getenv("WET_GRAIN_CONST"))   // "<constant><component>", e.g. 14x
        {
            uint32_t c = uint32_t(std::atoi(v));
            const char* comp = v;
            while (*comp >= '0' && *comp <= '9')
                comp++;
            uint32_t k = *comp == 'y' ? 1 : *comp == 'z' ? 2 : *comp == 'w' ? 3 : 0;
            xe::gpu::wet_grain_const = (256 + c) * 4 + k;
        }
        xe::gpu::wet_film_grain = float(options::FilmGrain()) / 100.0f;
#if WET_HAVE_D3D12
        xe::gpu::d3d12::wet_async_pipelines = options::AsyncShaders();
        xe::gpu::d3d12::wet_preload_pipelines = options::ShaderPreload();
#endif
        xe::gpu::wet_gpu_progress = renderer_host::GpuProgress;
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
            appContext->CallInUIThreadSynchronous([] {
                s_window->window->SetPresenter(s_graphics->presenter());
                SetUpOverlay();
            });
            renderer_host::StartThread(&GameWindow::RepaintThread, s_window, "Repaint");
        }
#endif
        s_running = true;
        char text[160];
        std::snprintf(text, sizeof(text), "Renderer: Xenia GPU emulation on %s started (%s)", s_backend, s_graphics->name().c_str());
        renderer_host::Log(text);
        // Shader / pipeline cache: everything the game has used is saved and, on
        // later runs, rebuilt before the first frame instead of when first drawn.
        if (!s_shaderCacheRoot.empty())
        {
            auto started = std::chrono::steady_clock::now();
            s_graphics->InitializeShaderStorage(std::filesystem::path(s_shaderCacheRoot), titleId, true);
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            bool background = false;
#if WET_HAVE_D3D12
            background = xe::gpu::d3d12::wet_async_pipelines && !xe::gpu::d3d12::wet_preload_pipelines;
#endif
            std::snprintf(text, sizeof(text), "Renderer: shader cache in %s loaded in %.1f s%s", s_shaderCacheRoot.c_str(), seconds,
                          background ? " (pipelines finish building in the background: see xenia_gpu.log)" : "");
            renderer_host::Log(text);
        }
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

    void SetVsyncRate(uint32_t hz)
    {
        xe::gpu::wet_vsync_hz = hz;
        // One present per vblank (above 30 FPS): a late frame goes out at once
        // instead of waiting a whole extra vblank. WET_NO_ADAPTIVE_VBLANK=1 turns it off.
        static const bool off = std::getenv("WET_NO_ADAPTIVE_VBLANK") != nullptr;
        xe::gpu::wet_adaptive_vblank = !off && wet::options::Fps() > 30;
    }

    uint32_t EarlyVblanks() { return xe::gpu::wet_early_vblanks.load(std::memory_order_relaxed); }

    bool VideoMemory(uint64_t& usedMB, uint64_t& budgetMB)
    {
#ifdef _WIN32
        static IDXGIAdapter3* adapter = [] {
            IDXGIAdapter3* a3 = nullptr;
            if (IDXGIAdapter1* a = gpu_info::HighPerformanceAdapter())
            {
                a->QueryInterface(IID_PPV_ARGS(&a3));
                a->Release();
            }
            return a3;
        }();
        DXGI_QUERY_VIDEO_MEMORY_INFO memory;
        if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
        {
            usedMB = memory.CurrentUsage >> 20;
            budgetMB = memory.Budget >> 20;
            return true;
        }
#endif
        return false;
    }

    bool GpuTimes(uint64_t us[8], uint64_t& frames)
    {
        frames = xe::gpu::wet_gpu_frames.load(std::memory_order_relaxed);
        for (int i = 0; i < 8; i++)
            us[i] = xe::gpu::wet_gpu_time_us[i].load(std::memory_order_relaxed);
        return frames != 0;
    }

    void SetResolutionScale(uint32_t scale) { s_resolutionScale = std::clamp<uint32_t>(scale, 1, 3); }

    float DisplayAspect()
    {
        constexpr float k169 = 16.0f / 9.0f;
        uint32_t w = 1280, h = 720;
#if XE_PLATFORM_WIN32
        if (wet::options::Widescreen() && s_window && s_window->window)
        {
            uint32_t ww = s_window->window->GetActualPhysicalWidth(), wh = s_window->window->GetActualPhysicalHeight();
            if (ww && wh && float(ww) / float(wh) > k169 + 0.01f)
            {
                w = ww;
                h = wh;
                if (float(w) / float(h) > 4.0f)
                    w = h * 4;   // wider than 36:9 isn't worth it
            }
        }
#endif
        xe::gpu::wet_display_aspect_x.store(w, std::memory_order_relaxed);
        xe::gpu::wet_display_aspect_y.store(h, std::memory_order_relaxed);
        // The HUD (drawn after the final composite) stays 16:9 in the middle.
        xe::gpu::wet_hud_squeeze.store(wet::options::WidescreenHud() ? (16.0f / 9.0f) * float(h) / float(w) : 1.0f,
                                       std::memory_order_relaxed);
        xe::gpu::wet_hide_borders.store(wet::options::HideBorders(), std::memory_order_relaxed);
        return float(w) / float(h);
    }

    void ToggleFullscreen()
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_window->window)
            s_window->app->CallInUIThreadDeferred([] {
                bool on = !s_window->window->IsFullscreen();
                s_window->window->SetFullscreen(on);
                options::SetFullscreen(on);
            });
#endif
    }

    void SetCursorHidden(bool hidden)
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_window->window)
            s_window->app->CallInUIThread([hidden] {
                s_window->window->SetCursorVisibility(hidden ? xe::ui::Window::CursorVisibility::kHidden
                                                             : xe::ui::Window::CursorVisibility::kVisible);
            });
#endif
    }

    void RequestBurstCapture()
    {
        if (!s_graphics || s_burstRunning.exchange(true))
            return;
        wet::renderer_host::Log("Renderer: F8 - capturing the next 48 frames");
        wet::renderer_host::StartThread(BurstThread, nullptr, "Burst capture");
    }

    void RefreshOverlay()
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_imgui)
            s_window->app->CallInUIThread([] { UpdateOverlay(); });
#endif
    }

    // F12 / photo mode: the next presented frame as a PNG in screenshots\.
    std::atomic<bool> s_screenshotRunning{ false };

    void ScreenshotThread(void*)
    {
        auto* cp = s_graphics ? s_graphics->command_processor() : nullptr;
        xe::ui::Presenter* presenter = s_graphics ? s_graphics->presenter() : nullptr;
        xe::ui::RawImage img;
        bool ok = false;
        if (cp && presenter)
        {
            // A frame presented after the key press (the overlay isn't in it).
            uint32_t swap = cp->wet_swap_count();
            auto start = std::chrono::steady_clock::now();
            while (cp->wet_swap_count() == swap && std::chrono::steady_clock::now() - start < std::chrono::seconds(1))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ok = presenter->CaptureGuestOutput(img) && img.width;
        }
        if (ok)
        {
            std::error_code ec;
            std::filesystem::create_directories("screenshots", ec);
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char name[96];
            std::strftime(name, sizeof(name), "screenshots/WET_%Y-%m-%d_%H-%M-%S", &tm);
            std::string path = name;
            for (int n = 2; std::filesystem::exists(path + ".png", ec); n++)
                path = std::string(name) + "_" + std::to_string(n);
            path += ".png";
            std::vector<uint8_t> rgb(size_t(img.width) * img.height * 3);
            for (uint32_t y = 0; y < img.height; y++)
                for (uint32_t x = 0; x < img.width; x++)
                    std::memcpy(&rgb[(size_t(y) * img.width + x) * 3], img.data.data() + y * img.stride + x * 4, 3);
            ok = stbi_write_png(path.c_str(), int(img.width), int(img.height), 3, rgb.data(), int(img.width) * 3) != 0;
            std::string shown = path;
            std::replace(shown.begin(), shown.end(), '/', '\\');
            wet::renderer_host::Log(((ok ? "Screenshot saved: " : "Screenshot failed: ") + shown).c_str());
            wet::photo::SetToast(ok ? "Screenshot saved: " + shown + " (" + std::to_string(img.width) + "x" +
                                          std::to_string(img.height) + ")"
                                    : "Couldn't save the screenshot");
        }
        else
            wet::photo::SetToast("Couldn't capture the screenshot");
        s_screenshotRunning = false;
    }

    void SaveScreenshot()
    {
        if (!s_graphics || s_screenshotRunning.exchange(true))
            return;
        wet::renderer_host::StartThread(ScreenshotThread, nullptr, "Screenshot");
    }

    void SetQuitHold(float progress)
    {
#if XE_PLATFORM_WIN32
        float was = s_quitHold.exchange(progress);
        if (was < 0 && progress >= 0 && s_window && s_window->app && s_imgui)
            s_window->app->CallInUIThread([] { UpdateOverlay(); });
#else
        (void)progress;
#endif
    }

    void AchievementUnlocked()
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_imgui)
            s_window->app->CallInUIThread([] { UpdateOverlay(); });
#endif
    }

    void ToggleOverlay()
    {
#if XE_PLATFORM_WIN32
        if (s_window && s_window->app && s_imgui)
        {
            s_overlayOpen = !s_overlayOpen;
            s_window->app->CallInUIThread([] { UpdateOverlay(); });
        }
#endif
    }

    bool OverlayOpen()
    {
#if XE_PLATFORM_WIN32
        return s_overlayOpen;
#else
        return false;
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
