// Video (Vd*), audio (XAudio/XMA), network (NetDll) and crypto (XeCrypt/XeKeys) imports.
#include "kernel.h"

#include <TinySHA1.hpp>
#include <atomic>

using namespace wet;

// ---------------------------------------------------------------------------
// Video. Mode: 1280x720p60 widescreen.
// The ring buffer / interrupt / swap entry points are where the GPU work starts;
// they only log for now.
// ---------------------------------------------------------------------------
static uint32_t s_gfxInterruptCallback = 0, s_gfxInterruptUser = 0;

// VdQueryVideoMode(X_VIDEO_MODE*)
GUEST_IMPORT(VdQueryVideoMode)
{
    uint32_t m = Arg(ctx, 0);
    float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, 4);
    Zero(m, 0x30);
    St32(m + 0x00, 1280);       // display_width
    St32(m + 0x04, 720);        // display_height
    St32(m + 0x08, 0);          // is_interlaced
    St32(m + 0x0C, 1);          // is_widescreen
    St32(m + 0x10, 1);          // is_hi_def
    St32(m + 0x14, refreshBits);// refresh_rate (float)
    St32(m + 0x18, 1);          // video_standard (NTSC)
    St32(m + 0x1C, 0x4A);       // unknown_0x8a
    St32(m + 0x20, 0x01);       // unknown_0x01
}

GUEST_IMPORT(VdQueryVideoFlags) { Ret(ctx, 0x3); } // widescreen | hi-def

// VdGetCurrentDisplayGamma(u32* type, float* power)
GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    float power = 2.22222233f;
    uint32_t bits;
    std::memcpy(&bits, &power, 4);
    St32(Arg(ctx, 0), 1);
    St32(Arg(ctx, 1), bits);
}

// VdGetCurrentDisplayInformation(X_DISPLAY_INFO*): only the front-buffer size is filled.
GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    uint32_t p = Arg(ctx, 0);
    Zero(p, 0x58);
    St16(p + 0, 1280);
    St16(p + 2, 720);
    TraceCall("VdGetCurrentDisplayInformation", ctx, "partially filled");
}

GUEST_IMPORT(VdInitializeEngines) { TraceCall("VdInitializeEngines", ctx); Ret(ctx, 1); }
GUEST_IMPORT(VdShutdownEngines) {}
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { Ret(ctx, 1); }
GUEST_IMPORT(VdRetrainEDRAM) { Ret(ctx, 0); }
GUEST_IMPORT(VdRetrainEDRAMWorker) { Ret(ctx, 0); }
GUEST_IMPORT(VdEnableDisableClockGating) { Ret(ctx, 0); }

// VdSetGraphicsInterruptCallback(callback, user_data)
GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    s_gfxInterruptCallback = Arg(ctx, 0);
    s_gfxInterruptUser = Arg(ctx, 1);
    Log("VdSetGraphicsInterruptCallback(0x%08X, 0x%08X) - GPU interrupts not emulated yet",
        s_gfxInterruptCallback, s_gfxInterruptUser);
}

// VdInitializeRingBuffer(ptr, size_log2)
GUEST_IMPORT(VdInitializeRingBuffer)
{
    Log("VdInitializeRingBuffer(0x%08X, log2 size %u) - GPU command processor not implemented yet", Arg(ctx, 0), Arg(ctx, 1));
}

// VdEnableRingBufferRPtrWriteBack(ptr, block_log2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    Log("VdEnableRingBufferRPtrWriteBack(0x%08X, %u)", Arg(ctx, 0), Arg(ctx, 1));
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    Log("VdSetSystemCommandBufferGpuIdentifierAddress(0x%08X)", Arg(ctx, 0));
}

// VdSwap(...) - the title presented a frame.
GUEST_IMPORT(VdSwap)
{
    static std::atomic<uint32_t> frames{ 0 };
    uint32_t n = ++frames;
    if (n <= 3 || (n & (n - 1)) == 0)
        Log("VdSwap: frame %u (buffer 0x%08X)", n, Arg(ctx, 0));
    Ret(ctx, 0);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    St32(Arg(ctx, 0), 0x00010001);
    Ret(ctx, ERROR_SUCCESS);
}

// XAudioGetVoiceCategoryVolume(category, float* volume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    float v = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    St32(Arg(ctx, 1), bits);
    Ret(ctx, ERROR_SUCCESS);
}

// XAudioRegisterRenderDriverClient(callback_info*, driver*)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t info = Arg(ctx, 0);
    Log("XAudioRegisterRenderDriverClient(callback 0x%08X, arg 0x%08X) - audio output not implemented yet",
        info ? Ld32(info) : 0, info ? Ld32(info + 4) : 0);
    St32(Arg(ctx, 1), 0x41550000);
    Ret(ctx, ERROR_SUCCESS);
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient) { Ret(ctx, ERROR_SUCCESS); }
GUEST_IMPORT(XAudioSubmitRenderDriverFrame) { Ret(ctx, ERROR_SUCCESS); }

// XMACreateContext(u32* context)
GUEST_IMPORT(XMACreateContext)
{
    St32(Arg(ctx, 0), SystemAlloc(0x40));
    Ret(ctx, ERROR_SUCCESS);
}

GUEST_IMPORT(XMAReleaseContext)
{
    SystemFree(Arg(ctx, 0));
    Ret(ctx, ERROR_SUCCESS);
}

// ---------------------------------------------------------------------------
// Network: report "no network" cleanly.
// ---------------------------------------------------------------------------
constexpr uint32_t WSAENETDOWN = 10050;

GUEST_IMPORT(NetDll_WSAStartup)
{
    if (Arg(ctx, 2))
        Zero(Arg(ctx, 2), 0x190);
    Ret(ctx, 0);
}
GUEST_IMPORT(NetDll_WSACleanup) { Ret(ctx, 0); }
GUEST_IMPORT(NetDll_XNetStartup) { Ret(ctx, 0); }
GUEST_IMPORT(NetDll_XNetCleanup) { Ret(ctx, 0); }
GUEST_IMPORT(NetDll_WSAGetLastError) { Ret(ctx, WSAENETDOWN); }
GUEST_IMPORT(NetDll_socket) { Ret(ctx, 0xFFFFFFFF); } // INVALID_SOCKET

// ---------------------------------------------------------------------------
// Crypto
// ---------------------------------------------------------------------------

// XeCryptSha(in1, len1, in2, len2, in3, len3, out, out_len)
GUEST_IMPORT(XeCryptSha)
{
    sha1::SHA1 sha;
    for (int i = 0; i < 3; i++)
    {
        uint32_t p = Arg(ctx, i * 2), n = Arg(ctx, i * 2 + 1);
        if (p && n)
            sha.processBytes(HostPtr(p), n);
    }
    sha1::SHA1::digest8_t digest;
    sha.finalize(digest);
    uint32_t out = Arg(ctx, 6), outLen = std::min<uint32_t>(Arg(ctx, 7), 20);
    std::memcpy(HostPtr(out), digest, outLen);
}

// Signature checks on content/saves: report success.
GUEST_IMPORT(XeCryptBnQwBeSigVerify) { Ret(ctx, 1); }

// XeKeysConsoleSignatureVerification(hash, signature, BOOL* result)
GUEST_IMPORT(XeKeysConsoleSignatureVerification)
{
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), 1);
    Ret(ctx, STATUS_SUCCESS);
}

// XeKeysConsolePrivateKeySign(hash, signature_out[0x228])
GUEST_IMPORT(XeKeysConsolePrivateKeySign)
{
    Zero(Arg(ctx, 1), 0x228);
    Ret(ctx, 1);
}
