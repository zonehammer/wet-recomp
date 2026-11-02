// Native mouse look: with mouse aim on, the mouse turns the third-person
// camera (wetcamera::Cam3rd) by an angle, instead of being turned into a
// right-stick value. No stick dead zone, response curve, turn speed or slow
// motion scaling: one mouse count is always the same angle. Where the camera
// hooks don't run (menus, turrets, other cameras) the mouse goes back to the
// right stick (padaim.cpp / input.cpp) after kHandBack.
//
// Hook points and the camera layout are from nikolaygorb/WetRecomp (MIT):
// Cam3rd::Update (sub_829BA128) gathers its input in sub_829B8078: the right
// stick from its WetCameraInput (camera+488, device at +4) through
// WetCameraInputFilter::ModifyAndFilterRightStickValue (sub_829A64B8, filter at
// camera+492) with the in-game invert options (bytes +2/+3 of camera+496); no
// invert option set flips both axes. It sets "right stick touched" bits in the
// camera flags (0x2 raw, 0x80 filtered); only then does Update run the manual
// orbit step, which turns yaw then pitch with sub_829AB470 each:
//     delta = dt * speed * filtered stick; angle = sub_829A8168(angle, delta, limits)
// The mouse turn is added to delta just before sub_829A8168, with the signs the
// stick would get there, so the orbit limits still apply and an axis the camera
// data locks (speed 0) stays locked.
//
// WET_NO_NATIVE_LOOK=1 turns it off (direct aim then drives the stick as before).
#include "input.h"
#include "log.h"
#include "memory.h"
#include "options.h"
#include "ppc_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace
{
    constexpr uint32_t kCameraInput = 488, kCameraFilter = 492, kCameraOptions = 496, kInputDevice = 4;
    constexpr uint32_t kRightStickTouched = 0x2 | 0x80;
    // sub_829AB470's frame is 144 bytes: the caller's r1+84 (yaw) / r1+80 (pitch).
    constexpr uint32_t kYawResult = 144 + 84, kPitchResult = 144 + 80;
    // Mouse reads come at the host rate, camera updates at the game's: a frame
    // without motion in the middle of a sweep mustn't hand the camera back to
    // auto-align.
    constexpr double kTouchedSeconds = 0.1;
    // No single update turns more than this (a stalled frame, a cursor jump).
    constexpr double kMaxRadians = 1.5;

    struct PendingTurn
    {
        uint32_t camera = 0;
        double yaw = 0, pitch = 0;
        bool yawPending = false, pitchPending = false;
    };
    thread_local PendingTurn t_turn;

    double Now()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    std::atomic<double> s_lastMotion{ -1e9 };

    bool Disabled()
    {
        static bool off = [] {
            const char* v = std::getenv("WET_NO_NATIVE_LOOK");
            bool o = v && *v && *v != '0';
            if (o)
                wet::Log("Mouse: native camera look off (WET_NO_NATIVE_LOOK)");
            return o;
        }();
        return off;
    }

    uint32_t Load32(uint32_t address)
    {
        uint8_t* base = wet::g_mem.base;
        return PPC_LOAD_U32(address);
    }

    // Same check as sub_829A2FE8: no device, no stick.
    bool HasInput(uint32_t camera)
    {
        uint32_t input = Load32(camera + kCameraInput);
        return input && Load32(input + kInputDevice);
    }

    // The signs sub_829A64B8 gives the right stick: -1 per axis, +1 with its invert option.
    void FilterSigns(uint32_t camera, double& x, double& y)
    {
        x = y = 1.0;
        if (!Load32(camera + kCameraFilter))
            return;
        uint32_t options = Load32(camera + kCameraOptions);
        if (!options)
        {
            x = y = -1.0;
            return;
        }
        uint8_t* base = wet::g_mem.base;
        x = PPC_LOAD_U8(options + 2) ? 1.0 : -1.0;
        y = PPC_LOAD_U8(options + 3) ? 1.0 : -1.0;
    }

    void AddTurn(PPCRegister& delta, double turn, double stickSign, double speed)
    {
        if (speed == 0.0 || !std::isfinite(speed))
            return;
        double sign = speed > 0.0 ? stickSign : -stickSign;
        delta.f64 = double(float(delta.f64 + turn * sign));
    }
}

// 0x829B80EC in the camera input gather, before the right stick is read. r29 = camera.
void WetMouseLookGather(PPCRegister& r29)
{
    t_turn = {};
    if (Disabled() || !wet::options::MouseAim() || !HasInput(r29.u32))
        return;
    double dx, dy;
    if (!wet::TakeLookDelta(dx, dy) || (dx == 0 && dy == 0))
        return;
    s_lastMotion = Now();
    static std::atomic<bool> s_logged{ false };
    if (!s_logged.exchange(true))
        wet::Log("Mouse: native camera look in use (camera %08X, sensitivity %d)", r29.u32,
                 wet::options::MouseSensitivity());
    // Sensitivity 50 (the default) = 0.066 degrees per pixel (3x Source's 0.022),
    // 100 = 0.132, 200 = 0.264.
    double radiansPerCount = 0.066 * wet::options::MouseSensitivity() / 50.0 * 3.14159265358979 / 180.0;
    double yaw = dx * radiansPerCount;
    double pitch = -dy * radiansPerCount;   // the cursor's y grows downwards, the stick's upwards
    if (wet::options::MouseInvertY())
        pitch = -pitch;
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return;
    t_turn = { r29.u32, std::clamp(yaw, -kMaxRadians, kMaxRadians), std::clamp(pitch, -kMaxRadians, kMaxRadians),
               true, true };
}

// 0x829B82D0, the store of the camera flags: r10 = flags, r29 = camera. While the
// mouse turns, the "right stick touched" bits make Update run the manual orbit step.
void WetMouseLookTouched(PPCRegister& r10, PPCRegister& r29)
{
    if (Disabled() || !wet::options::MouseAim() || !HasInput(r29.u32))
        return;
    bool turning = t_turn.camera == r29.u32 && t_turn.yawPending;
    if (!turning && Now() - s_lastMotion.load() > kTouchedSeconds)
        return;
    r10.u64 = r10.u32 | kRightStickTouched;
}

// 0x829AB4DC in the yaw / pitch step, before sub_829A8168: f2 = delta (radians),
// f28 = turn speed, r30 = where the result goes, r31 = camera.
void WetMouseLookTurn(PPCRegister& r1, PPCRegister& r30, PPCRegister& r31, PPCRegister& f2, PPCRegister& f28)
{
    if (t_turn.camera != r31.u32)
        return;
    uint32_t result = r30.u32 - r1.u32;
    double sx, sy;
    if (result == kYawResult && t_turn.yawPending)
    {
        t_turn.yawPending = false;
        FilterSigns(r31.u32, sx, sy);
        AddTurn(f2, t_turn.yaw, sx, f28.f64);
    }
    else if (result == kPitchResult && t_turn.pitchPending)
    {
        t_turn.pitchPending = false;
        FilterSigns(r31.u32, sx, sy);
        AddTurn(f2, t_turn.pitch, sy, f28.f64);
    }
}
