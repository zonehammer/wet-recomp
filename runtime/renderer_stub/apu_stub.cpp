// Without Xenia (WET_RENDERER=OFF): silent XMA decoding, no audio output.
#include "apu/apu.h"

namespace wet::apu
{
    bool XmaStart(uint8_t*, uint8_t*, uint32_t, uint32_t) { return false; }
    void XmaAllocate(uint32_t) {}
    void XmaRelease(uint32_t) {}
    void XmaKick(uint32_t) {}
    void XmaLock(uint32_t) {}
    void XmaClear(uint32_t) {}
    struct Output {};
    Output* OpenOutput() { return nullptr; }
    bool WaitForFrameSlot(Output*, uint32_t) { return false; }
    void SubmitFrame(Output*, uint32_t) {}
}
