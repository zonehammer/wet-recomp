// The game's pad update (sub_82BF2AA0, a method of its pad object, called once per
// game frame per controller). It reads XInputGetState and stores the sticks as
// plain linear floats (raw / 32768, no dead zone or curve yet) in the pad object:
//   +8 left X, +12 left Y, +16 right X, +20 right Y, +56/+60 triggers.
// The camera reads the right stick from there, so this is the place for direct
// mouse aim: the floats aren't limited to the stick's -1..1.
//
// Diagnostics for now: WET_RSTICK_SCALE=<n> multiplies the right stick after the
// read (does the camera clamp it?), and the pad's address is kept for the pad
// reader watch (Shift+F7, camscan.cpp).
#include "padaim.h"
#include "log.h"
#include "ppc_context.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace wet::padaim
{
    namespace
    {
        std::atomic<uint32_t> s_pad{ 0 };

        float Scale()
        {
            static float scale = [] {
                const char* env = std::getenv("WET_RSTICK_SCALE");
                float s = env ? float(std::atof(env)) : 1.0f;
                if (s != 1.0f)
                    Log("Pad: right stick scaled x%.2f (WET_RSTICK_SCALE)", s);
                return s;
            }();
            return scale;
        }

        float LoadF(uint8_t* base, uint32_t a)
        {
            uint32_t u = PPC_LOAD_U32(a);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        void StoreF(uint8_t* base, uint32_t a, float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            PPC_STORE_U32(a, u);
        }
    }

    uint32_t PadAddress() { return s_pad.load(); }
}

extern "C" void __imp__sub_82BF2AA0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82BF2AA0)
{
    uint32_t pad = ctx.r3.u32;
    uint32_t user = PPC_LOAD_U32(pad + 5156);
    __imp__sub_82BF2AA0(ctx, base);
    if (user != 0)
        return;
    if (wet::padaim::s_pad.exchange(pad) != pad)
        wet::Log("Pad: user 0's pad object is at %08X", pad);
    float scale = wet::padaim::Scale();
    if (scale != 1.0f)
    {
        wet::padaim::StoreF(base, pad + 16, wet::padaim::LoadF(base, pad + 16) * scale);
        wet::padaim::StoreF(base, pad + 20, wet::padaim::LoadF(base, pad + 20) * scale);
    }
}
