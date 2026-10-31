// Interface between the kernel's Vd* imports (kernel/gpu.cpp) and the renderer
// (gpu/xenia_bridge.cpp, Xenia's GPU emulation on Vulkan). Kept free of both the
// kernel's and Xenia's headers so neither leaks into the other.
#pragma once
#include <string>
#include <cstddef>
#include <cstdint>

namespace wet::renderer
{
    // True if the renderer was built in and not disabled (WET_RENDERER=0).
    bool Enabled();

    // Creates the Vulkan device, Xenia's graphics system and its command processor.
    // Returns false (after logging why) if no usable Vulkan device exists; the
    // kernel then falls back to its stand-in command processor.
    bool Start(uint8_t* membase, uint32_t titleId);
    // Folder for the shader / pipeline cache (before Start); empty = no cache.
    void SetShaderCacheRoot(const std::string& path);
    bool Running();

    void SetInterruptCallback(uint32_t callback, uint32_t userData);
    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2);
    // The title wrote CP_RB_WPTR (MMIO is plain memory; the kernel polls it).
    void UpdateWritePointer(uint32_t value);
    // The title wrote a GPU register through MMIO.
    void WriteRegister(uint32_t index, uint32_t value);
    uint32_t ReadRegister(uint32_t index);
    // Logs every draw of the next `frames` frames (diagnostics, F9).
    void RequestDrawTrace(int frames);
    // An achievement pop-up is waiting (achievements.cpp); any thread.
    void AchievementUnlocked();
    // F8: saves the next 48 presented frames to frames/burst_*.png (diagnostics).
    void RequestBurstCapture();
    // Hides the cursor over the game window while the mouse aims (any thread).
    void SetCursorHidden(bool hidden);
    // Guest vblank rate (60 Hz normally).
    void SetVsyncRate(uint32_t hz);
    // Rendering resolution scale (1 = 720p, 2 = 1440p, 3 = 2160p); before Start.
    void SetResolutionScale(uint32_t scale);
    void ToggleFullscreen();
    // Width / height the game is shown at: 16:9, or the window's shape with the
    // widescreen option (never narrower than 16:9). Passes it on to the presenter.
    float DisplayAspect();
    // F1 settings overlay; while it's open the keyboard doesn't drive the game.
    void ToggleOverlay();
    bool OverlayOpen();
}

// Provided by the kernel for the renderer's threads.
namespace wet::renderer_host
{
    // The GPU wrote a fence or similar (gpu_wait.cpp wakes the render thread).
    void GpuProgress();
    // Write-watch trace around an F8 burst capture (memory.h).
    void StartWatchTrace();
    void StopWatchTrace(const char* path);
    void TraceEvent(char type, uint32_t a, uint32_t b);
    // Starts a host thread that can call guest code.
    void StartThread(void (*entry)(void*), void* arg, const char* name);
    // Gives the calling host thread a guest context (stack, PCR); returns it (PPCContext*).
    void* AdoptThread(const char* name);
    // Runs a guest function as an interrupt on `ctx`'s thread (registers preserved).
    uint64_t CallInterrupt(void* ctx, uint32_t address, const uint64_t* args, size_t count);
    // PCR "current processor" byte, which interrupt handlers read.
    void SetActiveCpu(void* ctx, uint32_t cpu);
    void Log(const char* text);
    [[noreturn]] void ExitProcess();
    // Physical memory: an unwatched mapping, and write watches on guest views.
    uint8_t* PhysicalHostView();
    void WatchPhysical(uint32_t physStart, uint32_t length);
    // Routes write-watch hits to `callback` (the renderer's invalidation).
    void SetPhysicalWriteCallback(void (*callback)(uint32_t physStart, uint32_t length));
    void UnwatchPhysical(uint32_t physStart, uint32_t length);
    // Asks the GPU thread to mirror the title's register writes into the renderer now.
    void RequestRegisterSync();
}
