// Video (Vd*), audio (XAudio/XMA), network (NetDll) and crypto (XeCrypt/XeKeys) imports.
#include "kernel.h"
#include "threads.h"
#include "xma.h"
#include "os.h"

#include <algorithm>
#include <TinySHA1.hpp>
#include <atomic>
#include <mutex>

using namespace wet;

// ---------------------------------------------------------------------------
// Video. Mode: 1280x720p60 widescreen.
// The GPU-facing entry points (ring buffer, interrupts, swap) live in gpu.cpp.
// ---------------------------------------------------------------------------
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

// The audio "driver": the title's XAudio2 mixes in a callback the system calls
// once per 256-sample frame (48 kHz, 5.33 ms) and hands the result back with
// XAudioSubmitRenderDriverFrame. Nothing is played yet, but the callback has to
// run at the real rate - without it XAudio2 never processes its voices, so
// sounds never finish and the title's heap fills with them.
namespace
{
    struct AudioClient { uint32_t callback = 0, argPtr = 0; bool active = false; };
    constexpr int kMaxAudioClients = 8;
    AudioClient s_audioClients[kMaxAudioClients];
    std::mutex s_audioLock;
    std::once_flag s_audioThreadOnce;

    void AudioThreadMain(void*)
    {
        PPCContext& ctx = AdoptHostThread(0x10000);
        SetThreadName(0xFFFFFFFF, "Audio driver (host)");
        constexpr uint64_t kFrameNs = 256ull * 1000000000ull / 48000ull;
        uint64_t next = os::MonotonicNs();
        for (;;)
        {
            uint64_t now = os::MonotonicNs();
            if (now < next)
            {
                os::SleepNs(next - now);
                continue;
            }
            // Catch up at most a few frames after a stall, then resynchronise.
            next = (now - next > 8 * kFrameNs) ? now + kFrameNs : next + kFrameNs;
            for (int i = 0; i < kMaxAudioClients; i++)
            {
                AudioClient c;
                {
                    std::lock_guard lock(s_audioLock);
                    c = s_audioClients[i];
                }
                if (!c.active)
                    continue;
                ctx.r3.u64 = c.argPtr;
                CallGuest(ctx, c.callback);
            }
        }
    }
}

// XAudioRegisterRenderDriverClient(callback_info*, driver*)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t info = Arg(ctx, 0);
    uint32_t callback = info ? Ld32(info) : 0, arg = info ? Ld32(info + 4) : 0;
    int index = -1;
    {
        std::lock_guard lock(s_audioLock);
        for (int i = 0; i < kMaxAudioClients && index < 0; i++)
            if (!s_audioClients[i].active)
                index = i;
        if (index >= 0)
        {
            // Like Xenia, the callback gets a pointer to a word holding its argument.
            uint32_t argPtr = s_audioClients[index].argPtr ? s_audioClients[index].argPtr : SystemAlloc(4);
            St32(argPtr, arg);
            s_audioClients[index] = { callback, argPtr, true };
        }
    }
    if (index < 0)
        return Ret(ctx, 0xC0000017);    // STATUS_NO_MEMORY
    Log("XAudioRegisterRenderDriverClient(callback 0x%08X, arg 0x%08X) -> client %d (callbacks at 48 kHz/256, no sound output yet)",
        callback, arg, index);
    St32(Arg(ctx, 1), 0x41550000u | uint32_t(index));
    std::call_once(s_audioThreadOnce, [] { os::StartThread(AudioThreadMain, nullptr, 1 << 20); });
    Ret(ctx, ERROR_SUCCESS);
}

// XAudioUnregisterRenderDriverClient(driver)
GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    uint32_t index = Arg(ctx, 0) & 0xFFFF;
    if (index < kMaxAudioClients)
    {
        std::lock_guard lock(s_audioLock);
        s_audioClients[index].active = false;
    }
    Ret(ctx, ERROR_SUCCESS);
}
GUEST_IMPORT(XAudioSubmitRenderDriverFrame) { Ret(ctx, ERROR_SUCCESS); }

// XMACreateContext(u32* context)
GUEST_IMPORT(XMACreateContext)
{
    uint32_t c = XmaAllocateContext();
    St32(Arg(ctx, 0), c);
    Ret(ctx, c ? ERROR_SUCCESS : 0xC0000017 /* STATUS_NO_MEMORY */);
}

GUEST_IMPORT(XMAReleaseContext)
{
    XmaReleaseContext(Arg(ctx, 0));
    Ret(ctx, 0);
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
