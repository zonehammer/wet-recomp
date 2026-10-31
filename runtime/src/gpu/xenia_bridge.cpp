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
#include "input.h"
#include "achievements.h"
#include "keybinds.h"
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

    // "A, B" (hex) -> hashes (skip_passes).
    static std::vector<uint64_t> ParseHashes(const std::string& text)
    {
        std::vector<uint64_t> out;
        size_t i = 0;
        while (i < text.size())
        {
            size_t j = text.find(',', i);
            if (j == std::string::npos)
                j = text.size();
            std::string item = text.substr(i, j - i);
            item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return std::isspace((unsigned char)c); }),
                       item.end());
            if (!item.empty())
                out.push_back(std::strtoull(item.c_str(), nullptr, 16));
            i = j + 1;
        }
        return out;
    }

    // Width / height of the main monitor (16:9 if unknown).
    float ScreenAspect()
    {
#if XE_PLATFORM_WIN32
        int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
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

            if (!s_overlayOpen && !wet::options::ShowFps() && !PopupPending())
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
            if (!ImGui::CollapsingHeader("Achievement list"))
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

        static void Section(const char* title)
        {
            ImGui::Spacing();
            ImGui::TextUnformatted(title);
            ImGui::Separator();
        }

        // Post effect passes: every full-screen pass the game drew recently, in
        // frame order, each with a checkbox (skipped when off). For finding which
        // pass is motion blur, bloom and so on.
        static void DrawPasses()
        {
            if (!ImGui::CollapsingHeader("Post effect passes (experimental)"))
                return;
            ImGui::TextDisabled("Full-screen passes from the last few seconds, in drawing order. Untick one to");
            ImGui::TextDisabled("switch it off and see what changes (some may break the picture - tick it again).");
            std::vector<uint64_t> skipped = ParseHashes(wet::options::SkippedPasses());
            auto passes = xe::gpu::WetPassList();
            uint32_t now = s_graphics && s_graphics->command_processor() ? s_graphics->command_processor()->wet_swap_count() : 0;
            std::vector<xe::gpu::WetPassInfo> recent;
            for (auto& p : passes)
                if (now - p.last_frame < 600 || std::find(skipped.begin(), skipped.end(), p.hash) != skipped.end())
                    recent.push_back(p);
            std::sort(recent.begin(), recent.end(), [](auto& a, auto& b) { return a.order < b.order; });
            bool changed = false;
            for (auto& p : recent)
            {
                bool on = std::find(skipped.begin(), skipped.end(), p.hash) == skipped.end();
                char label[96];
                std::snprintf(label, sizeof(label), "#%u  %016llX%s%s", p.order, (unsigned long long)p.hash,
                              p.hash == xe::gpu::wet_grain_ps_hash ? "  (final composite + film grain)" : "",
                              now - p.last_frame >= 600 ? "  (not seen lately)" : "");
                if (ImGui::Checkbox(label, &on))
                {
                    changed = true;
                    if (on)
                        skipped.erase(std::remove(skipped.begin(), skipped.end(), p.hash), skipped.end());
                    else
                        skipped.push_back(p.hash);
                }
            }
            if (recent.empty())
                ImGui::TextDisabled("(none yet - play for a moment)");
            if (!skipped.empty() && ImGui::Button("Switch all back on"))
            {
                skipped.clear();
                changed = true;
            }
            if (changed)
            {
                std::string text;
                for (uint64_t h : skipped)
                {
                    char hex[24];
                    std::snprintf(hex, sizeof(hex), "%s%016llX", text.empty() ? "" : ", ", (unsigned long long)h);
                    text += hex;
                }
                wet::options::SetSkippedPasses(text);
                xe::gpu::WetSetSkippedPasses(skipped);
            }
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

        static bool Radio(const char* label, int* value, int choice)
        {
            return ImGui::RadioButton(label, value, choice);
        }

        void DrawSettings(ImGuiIO& io, float scale)
        {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowBgAlpha(0.88f);
            bool open = true;
            if (!ImGui::Begin("WET Recomp " WET_VERSION " - settings (F1 to close)###settings", &open,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::End();
                return;
            }

            Section("Frame rate");
            int fps = wet::options::ConfiguredFps();
            int pick = fps;
            static constexpr int kChoices[] = { 30, 60, 120, 144, 165, 240 };
            bool listed = fps == 0;
            for (int c : kChoices)
            {
                char label[24];
                std::snprintf(label, sizeof(label), c == 30 ? "%d (original)" : "%d", c);
                Radio(label, &pick, c);
                ImGui::SameLine();
                listed |= fps == c;
            }
            char monitor[40];
            std::snprintf(monitor, sizeof(monitor), "Monitor (%d Hz)", wet::options::MonitorRefreshRate());
            Radio(monitor, &pick, 0);
            // Any other rate: the slider (also shows a custom rate from wet.ini).
            int custom = pick == 0 ? wet::options::MonitorRefreshRate() : pick;
            if (ImGui::SliderInt(listed ? "Other rate" : "Other rate (in use)", &custom, 30, 360, "%d FPS"))
                pick = custom;
            if (pick != fps)
                wet::options::SetFps(pick);
            bool asyncShaders = wet::options::AsyncShaders();
            if (ImGui::Checkbox("Don't wait for new shaders (less stutter; objects may pop in)", &asyncShaders))
            {
                wet::options::SetAsyncShaders(asyncShaders);
                ApplyPostProcessing();
            }
            bool videoMode = wet::options::VideoMode();
            if (ImGui::Checkbox("Videos at 30 FPS (avoids video flicker)", &videoMode))
                wet::options::SetVideoMode(videoMode);
            if (wet::options::Fps() != (wet::options::ConfiguredFps() ? wet::options::ConfiguredFps() : wet::options::MonitorRefreshRate()))
                ImGui::TextDisabled("A video is playing: %d FPS until it ends", wet::options::Fps());
            bool showFps = wet::options::ShowFps();
            if (ImGui::Checkbox("Show FPS counter", &showFps))
                wet::options::SetShowFps(showFps);
            bool adaptive = wet::options::AdaptiveWatch();
            if (ImGui::Checkbox("Fast memory tracking (turn off if textures or models glitch)", &adaptive))
                wet::options::SetAdaptiveWatchOption(adaptive);
            bool gpuWait = wet::options::GpuWait();
            if (ImGui::Checkbox("Low-CPU GPU wait (render thread sleeps instead of spinning)", &gpuWait))
                wet::options::SetGpuWait(gpuWait);

            Section("Achievements");
            DrawAchievements(scale);

            Section("Controllers");
            {
                auto pads = wet::ConnectedControllers();
                bool any = false;
                for (size_t i = 0; i < pads.size(); i++)
                    if (!pads[i].empty())
                    {
                        ImGui::Text("Player %zu: %s", i + 1, pads[i].c_str());
                        any = true;
                    }
                if (!any)
                    ImGui::TextDisabled("No controller connected (keyboard: WASD, arrows, Space, Esc, X, Y, Q, E, Z, C, Enter)");
                bool vibration = wet::options::Vibration();
                if (ImGui::Checkbox("Vibration", &vibration))
                    wet::options::SetVibration(vibration);
            }

            Section("Mouse");
            {
                bool aim = wet::options::MouseAim();
                if (ImGui::Checkbox("Mouse aim (camera / aiming; F1 or Alt+Tab frees the cursor)", &aim))
                    wet::options::SetMouseAim(aim);
                int sens = wet::options::MouseSensitivity(), sensPick = sens;
                ImGui::SliderInt("Mouse sensitivity", &sensPick, 1, 100);
                if (sensPick != sens)
                    wet::options::SetMouseSensitivity(sensPick);
                bool invert = wet::options::MouseInvertY();
                if (ImGui::Checkbox("Invert mouse Y", &invert))
                    wet::options::SetMouseInvertY(invert);
                bool direct = wet::options::MouseDirect();
                if (ImGui::Checkbox("Direct mouse aim (turn by how far the mouse moves, no stick speed limit)", &direct))
                    wet::options::SetMouseDirect(direct);
                ImGui::TextDisabled("Left click RT, right click LT (other buttons: wet.ini mouse_*)");
            }

            DrawKeyboard();
            DrawPasses();

            Section("Resolution");
            int res = wet::options::ResolutionScale();
            int resPick = res;
            Radio("720p (original)", &resPick, 1); ImGui::SameLine();
            Radio("1440p", &resPick, 2); ImGui::SameLine();
            Radio("2160p / 4K", &resPick, 3);
            if (resPick != res)
                wet::options::SetResolutionScale(resPick);
            if (wet::options::ResolutionScale() != wet::options::RunningResolutionScale())
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart the game to change the resolution.");

            Section("Display");
            bool fullscreen = s_window && s_window->window && s_window->window->IsFullscreen();
            if (ImGui::Checkbox("Fullscreen (F11)", &fullscreen))
            {
                // Not while painting: switching resizes the swap chain the
                // presenter is drawing into (that crashed in the driver).
                s_window->app->CallInUIThreadDeferred([fullscreen] {
                    s_window->window->SetFullscreen(fullscreen);
                });
                wet::options::SetFullscreen(fullscreen);
            }

            int grain = wet::options::FilmGrain(), grainPick = grain;
            ImGui::SliderInt("Film grain", &grainPick, 0, 100, "%d%%");
            if (grainPick != grain)
            {
                wet::options::SetFilmGrain(grainPick);
                ApplyPostProcessing();
            }

            bool wide = wet::options::Widescreen();
            if (ImGui::Checkbox("Widescreen / ultrawide (fill wider screens; sharpest after a restart)", &wide))
                wet::options::SetWidescreen(wide);

            int fov = wet::options::Fov(), fovPick = fov;
            ImGui::SliderInt("Field of view (90 = original)", &fovPick, 60, 130, "%d deg");
            if (fovPick != fov)
                wet::options::SetFov(fovPick);

            int aa = wet::options::Antialiasing(), aaPick = aa;
            ImGui::TextUnformatted("Anti-aliasing:");
            Radio("Off", &aaPick, 0); ImGui::SameLine();
            Radio("FXAA", &aaPick, 1); ImGui::SameLine();
            Radio("FXAA (extreme)", &aaPick, 2);
            int filter = wet::options::Filter(), filterPick = filter;
            ImGui::TextUnformatted("Scaling to the window:");
            Radio("Bilinear", &filterPick, 0); ImGui::SameLine();
            Radio("AMD CAS (sharpen)", &filterPick, 1); ImGui::SameLine();
            Radio("AMD FSR (upscale)", &filterPick, 2); ImGui::SameLine();
            Radio("NVIDIA NIS (upscale)", &filterPick, 3);
            int sharp = wet::options::Sharpness(), sharpPick = sharp;
            if (filter == 3)
            {
                if (s_nisUnsupported)
                    ImGui::TextDisabled("NIS needs the D3D12 renderer; AMD FSR is used instead.");
                ImGui::SliderInt("NIS sharpness", &sharpPick, 0, 100, "%d%%");
            }
            if (aaPick != aa || filterPick != filter || sharpPick != sharp)
            {
                wet::options::SetAntialiasing(aaPick);
                wet::options::SetFilter(filterPick);
                wet::options::SetSharpness(sharpPick);
                ApplyPostProcessing();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Settings are saved in wet.ini. While this is open, the keyboard doesn't control the game.");
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
        if (!s_overlay && (s_overlayOpen || wet::options::ShowFps() || PopupPending()))
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
        xe::gpu::WetSetSkippedPasses(ParseHashes(options::SkippedPasses()));
#if WET_HAVE_D3D12
        xe::gpu::d3d12::wet_async_pipelines = options::AsyncShaders();
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
            std::snprintf(text, sizeof(text), "Renderer: shader cache in %s loaded in %.1f s", s_shaderCacheRoot.c_str(), seconds);
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

    void SetVsyncRate(uint32_t hz) { xe::gpu::wet_vsync_hz = hz; }

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
        xe::gpu::wet_hud_squeeze.store((16.0f / 9.0f) * float(h) / float(w), std::memory_order_relaxed);
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
