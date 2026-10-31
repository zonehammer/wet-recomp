// The game's pad update (sub_82BF2AA0, a method of its pad object, called once per
// game frame per controller). It reads XInputGetState and stores the sticks as
// plain linear floats (raw / 32768, no dead zone or curve yet) in the pad object:
//   +8 left X, +12 left Y, +16 right X, +20 right Y, +56/+60 triggers.
// The camera reads the right stick from there, so this is the place for direct
// mouse aim: the floats aren't limited to the stick's -1..1.
//
// Direct mouse aim (mouse_direct): each update, the mouse movement since the last
// one becomes a right-stick value proportional to the mouse's speed, past the
// camera's dead zone and without the -1..1 limit (the camera doesn't clamp it:
// WET_RSTICK_SCALE=3 turned three times as fast). So a fast flick turns as far as
// the mouse moved instead of topping out at the stick's turn speed.
//
// Diagnostics: WET_RSTICK_SCALE=<n> multiplies the right stick after the read, and
// the pad's address is kept for the pad reader watch (Shift+F7, camscan.cpp).
#include "padaim.h"
#include "input.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

        // Mouse movement since the last update -> right stick (x right, y up).
        bool MouseStick(float& sx, float& sy)
        {
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            double dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.001, 0.1);
            last = now;
            double dx, dy;
            if (!TakeMouseDelta(dx, dy) || (dx == 0 && dy == 0))
                return false;
            // Same scale as the stick mode at low speed: sensitivity 50 = 1200 px/s
            // for a full deflection, 100 = 600 px/s - but nothing caps it at 1.
            double k = options::MouseSensitivity() / 100.0 / 600.0;
            double tx = dx / dt * k, ty = -dy / dt * k;
            if (options::MouseInvertY())
                ty = -ty;
            double mag = std::sqrt(tx * tx + ty * ty);
            // Past the camera's dead zone (~24%) so slow movements still turn.
            constexpr double kDeadZone = 0.24;
            double out = kDeadZone + (1.0 - kDeadZone) * mag;
            sx = float(tx / mag * out);
            sy = float(ty / mag * out);
            return true;
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
    float mx, my;
    if (wet::padaim::MouseStick(mx, my))
    {
        wet::padaim::StoreF(base, pad + 16, mx);
        wet::padaim::StoreF(base, pad + 20, my);
    }
    float scale = wet::padaim::Scale();
    if (scale != 1.0f)
    {
        wet::padaim::StoreF(base, pad + 16, wet::padaim::LoadF(base, pad + 16) * scale);
        wet::padaim::StoreF(base, pad + 20, wet::padaim::LoadF(base, pad + 20) * scale);
    }
}
