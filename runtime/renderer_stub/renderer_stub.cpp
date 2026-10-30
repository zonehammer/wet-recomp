// Built instead of the renderer when WET_RENDERER is OFF.
#include "gpu/renderer.h"

namespace wet::renderer
{
    bool Enabled() { return false; }
    bool Start(uint8_t*, uint32_t) { return false; }
    bool Running() { return false; }
    void SetInterruptCallback(uint32_t, uint32_t) {}
    void InitializeRingBuffer(uint32_t, uint32_t) {}
    void EnableReadPointerWriteBack(uint32_t, uint32_t) {}
    void UpdateWritePointer(uint32_t) {}
    void WriteRegister(uint32_t, uint32_t) {}
    void RequestDrawTrace(int) {}
    void AchievementUnlocked() {}
    void SetVsyncRate(uint32_t) {}
    void SetResolutionScale(uint32_t) {}
    void ToggleFullscreen() {}
    void ToggleOverlay() {}
    bool OverlayOpen() { return false; }
    uint32_t ReadRegister(uint32_t) { return 0; }
}
