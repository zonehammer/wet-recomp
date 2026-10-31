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
#include <map>
#include <mutex>
#include <utility>

namespace wet::padaim
{
    namespace
    {
        std::atomic<uint32_t> s_pad{ 0 };
        // The controller's own right stick, and what we last wrote over it: the
        // game only refreshes the pad when the XInput packet changes, so after
        // mouse movement the controller value has to be put back by us.
        float s_ctrlX = 0, s_ctrlY = 0, s_wroteX = 0, s_wroteY = 0;
        bool s_wrote = false;

        // Axis reader log (Shift+F7): (caller lr, axis) -> count.
        std::atomic<bool> s_logAxes{ false };
        std::mutex s_axisLock;
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> s_axisCalls;

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

    void LogAxisReaders(bool on)
    {
        if (on)
        {
            std::lock_guard lock(s_axisLock);
            s_axisCalls.clear();
        }
        s_logAxes = on;
    }

    std::vector<AxisCall> AxisReaders()
    {
        std::lock_guard lock(s_axisLock);
        std::vector<AxisCall> out;
        for (auto& [key, count] : s_axisCalls)
            out.push_back({ key.first, key.second, count });
        return out;
    }

    void NoteAxisCall(uint32_t lr, uint32_t axis)
    {
        std::lock_guard lock(s_axisLock);
        s_axisCalls[{ lr, axis }]++;
    }
}

namespace
{
    thread_local uint32_t t_actionLr = 0;   // caller of the input action being read
}

// An input action's value (sub_82B0ADD0): reads the pad axes bound to it through
// GetAxis. Its caller is the game code that uses the stick (the camera).
extern "C" void __imp__sub_82B0ADD0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82B0ADD0)
{
    uint32_t outer = t_actionLr;
    t_actionLr = uint32_t(ctx.lr);
    __imp__sub_82B0ADD0(ctx, base);
    t_actionLr = outer;
}

// The pad object's GetAxis(id) (a virtual): +8 + id*4, so 2 = right X, 3 = right Y.
// Logged by the caller of the action when read through one.
extern "C" void __imp__sub_82BF1CC0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82BF1CC0)
{
    if (wet::padaim::s_logAxes.load(std::memory_order_relaxed))
    {
        uint32_t lr = uint32_t(ctx.lr);
        if ((lr == 0x82B0AE28 || lr == 0x82B0AE68) && t_actionLr)
            lr = t_actionLr;
        wet::padaim::NoteAxisCall(lr, ctx.r4.u32);
    }
    __imp__sub_82BF1CC0(ctx, base);
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
    using namespace wet::padaim;
    float curX = LoadF(base, pad + 16), curY = LoadF(base, pad + 20);
    if (!s_wrote || curX != s_wroteX || curY != s_wroteY)
    {
        s_ctrlX = curX;   // the game stored a fresh controller reading
        s_ctrlY = curY;
    }
    float mx, my;
    if (MouseStick(mx, my))
    {
        StoreF(base, pad + 16, mx);
        StoreF(base, pad + 20, my);
        s_wroteX = mx;
        s_wroteY = my;
        s_wrote = true;
    }
    else if (s_wrote)
    {
        // Mouse stopped: back to the controller's stick.
        StoreF(base, pad + 16, s_ctrlX);
        StoreF(base, pad + 20, s_ctrlY);
        s_wroteX = s_ctrlX;
        s_wroteY = s_ctrlY;
    }
    float scale = wet::padaim::Scale();
    if (scale != 1.0f)
    {
        wet::padaim::StoreF(base, pad + 16, wet::padaim::LoadF(base, pad + 16) * scale);
        wet::padaim::StoreF(base, pad + 20, wet::padaim::LoadF(base, pad + 20) * scale);
    }
}
