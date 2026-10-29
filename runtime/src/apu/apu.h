// Interface between the kernel (kernel/xma.cpp, kernel/misc.cpp) and Xenia's
// audio code (apu/xenia_apu_bridge.cpp): the XMA decoder contexts and the
// audio output. Free of both sides' headers.
#pragma once
#include <cstdint>

namespace wet::apu
{
    // XMA decoding with Xenia's XmaContext (FFmpeg). False if not built in, in
    // which case the kernel's silent stand-in decoder is used.
    bool XmaStart(uint8_t* membase, uint8_t* physicalView, uint32_t firstContextGuest, uint32_t count);
    void XmaAllocate(uint32_t id);
    void XmaRelease(uint32_t id);
    void XmaKick(uint32_t id);
    void XmaLock(uint32_t id);
    void XmaClear(uint32_t id);

    // Audio output (XAudio2 on Windows); null if there is none.
    struct Output;
    Output* OpenOutput();
    // Waits until the output can take another 256-sample frame.
    bool WaitForFrameSlot(Output* out, uint32_t timeoutMs);
    // Queues the title's frame: 6 channels x 256 big-endian floats at `guest`.
    void SubmitFrame(Output* out, uint32_t guest);
}
