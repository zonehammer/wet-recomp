// Photo mode: F5 (or L3 + R3) in third-person play.
//
// - Time: the world's per-frame update (sub_82C32BF8, options.cpp) gets a time
//   step of 0.1 ms, so everything stands still (a real 0 risks divisions by zero
//   in the game's physics) while the game keeps rendering.
// - Input: the game sees a controller at rest; the real controller, keyboard and
//   mouse fly the camera instead (input.cpp hands them over).
// - Camera: the game's "set camera" (sub_82C7E590, fov.cpp) gets our view matrix
//   instead of the game's for the main (wide perspective) camera. It starts where
//   the game's camera was; everything derived from the view (culling included)
//   follows it. Its own field of view zooms.
// - HUD: HudComponentImpl::OnHudVisibility (sub_828F1010, the handler of
//   wetMessage::HudVisibilityMsg: byte +4 = visible) is called with a message
//   made on the game thread's stack. The component is the `this` of its handler
//   registration (sub_828F2248), forgotten in its destructor (sub_828F35E0), and
//   checked against its vtable (0x822CDE74) before use. Its own flag is at +40.
// - Screenshots: the renderer saves the game's output (no overlay) as a PNG.
#include "photo.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "options.h"
#include "gpu/renderer.h"
#include "ppc_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace wet::photo
{
    namespace
    {
        constexpr uint32_t kHudVtable = 0x822CDE74;
        constexpr double kFrozenStep = 0.0001;
        constexpr double kPi = 3.14159265358979323846;

        using Clock = std::chrono::steady_clock;
        double Seconds()
        {
            static const auto start = Clock::now();
            return std::chrono::duration<double>(Clock::now() - start).count();
        }

        struct V3
        {
            double x = 0, y = 0, z = 0;
            V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
            V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
            V3 operator*(double s) const { return { x * s, y * s, z * s }; }
        };
        double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        V3 Normal(const V3& a)
        {
            double l = std::sqrt(Dot(a, a));
            return l > 1e-12 ? a * (1.0 / l) : a;
        }
        // Rotates v about the unit axis k (Rodrigues).
        V3 Rotate(const V3& v, const V3& k, double angle)
        {
            double c = std::cos(angle), s = std::sin(angle);
            return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1 - c));
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

        std::mutex s_lock;
        std::atomic<bool> s_active{ false }, s_toggle{ false }, s_paused{ false };
        std::atomic<double> s_lastThirdPerson{ -1e9 }, s_toggleAt{ -1e9 };

        // Camera (guarded by s_lock). Basis vectors and position in world space, in
        // the row-vector convention: view = inverse([right; up; axis; position]).
        bool s_haveCamera = false, s_wantCapture = false;
        bool s_transposed = false;   // the game's matrices are column-vector ones
        double s_forwardSign = 1;    // the view looks along +axis (D3D) or -axis
        V3 s_right, s_up, s_axis, s_pos, s_worldUp;
        V3 s_startRight, s_startUp, s_startAxis, s_startPos;
        double s_fov = 90, s_lastUpdate = 0;
        double s_mouseX = 0, s_mouseY = 0;
        uint16_t s_buttons = 0, s_lastButtons = 0;
        uint8_t s_lt = 0, s_rt = 0;
        int16_t s_lx = 0, s_ly = 0, s_rx = 0, s_ry = 0;
        bool s_fast = false;
        bool s_firstFrame = false;

        // HUD.
        std::atomic<uint32_t> s_hud{ 0 };
        std::atomic<bool> s_hudHidden{ false };
        bool s_hudWant = true;           // game thread: what the HUD should be
        bool s_hudToggleRequest = false; // Y while active (guarded by s_lock)
        uint8_t s_hudWas = 1;

        std::mutex s_toastLock;
        std::string s_toast;
        double s_toastUntil = 0;

        bool HudUsable(uint8_t* base)
        {
            uint32_t hud = s_hud.load();
            return hud && PPC_LOAD_U32(hud) == kHudVtable;
        }
    }

    extern "C" void __imp__sub_828F1010(PPCContext& ctx, uint8_t* base);

    namespace
    {
        // Game thread: the HUD's own visibility handler with a message on the stack.
        void SetHudVisible(PPCContext& ctx, uint8_t* base, bool visible)
        {
            if (!HudUsable(base))
                return;
            uint32_t hud = s_hud.load();
            PPCContext saved = ctx;
            uint32_t msg = (ctx.r1.u32 - 2048) & ~15u;
            for (uint32_t i = 0; i < 16; i += 4)
                PPC_STORE_U32(msg + i, 0);
            PPC_STORE_U8(msg + 4, visible ? 1 : 0);
            ctx.r1.u64 = msg - 256;
            ctx.r3.u64 = hud;
            ctx.r4.u64 = msg;
            __imp__sub_828F1010(ctx, base);
            ctx = saved;
        }
    }

    bool Active() { return s_active.load(); }
    void RequestToggle()
    {
        s_toggleAt = Seconds();
        s_toggle = true;
        // Nothing picks the request up outside play (menus, pause): say so.
        if (!s_active && (s_paused || Seconds() - s_lastThirdPerson.load() > 1.0))
        {
            s_toggle = false;
            SetToast("Photo mode works in third-person play (not in menus or cutscenes)");
        }
    }
    void NoteThirdPersonCamera() { s_lastThirdPerson = Seconds(); }
    void SetPauseMenu(bool open) { s_paused = open; }

    void NotePad(const HostPadState& pad)
    {
        std::lock_guard g(s_lock);
        s_buttons = pad.buttons;
        s_lt = pad.leftTrigger, s_rt = pad.rightTrigger;
        s_lx = pad.lx, s_ly = pad.ly, s_rx = pad.rx, s_ry = pad.ry;
    }

    void AddMouse(double dx, double dy)
    {
        std::lock_guard g(s_lock);
        s_mouseX += dx;
        s_mouseY += dy;
    }

    double Fov()
    {
        std::lock_guard g(s_lock);
        return s_fov;
    }

    Status GetStatus()
    {
        Status s;
        s.active = s_active.load();
        s.hudHidden = s_hudHidden.load();
        s.hudAvailable = s_hud.load() != 0;
        std::lock_guard g(s_lock);
        s.fov = s_fov;
        s.fast = s_fast;
        return s;
    }

    std::string Toast()
    {
        std::lock_guard g(s_toastLock);
        return Seconds() < s_toastUntil ? s_toast : std::string();
    }

    void SetToast(const std::string& text)
    {
        {
            std::lock_guard g(s_toastLock);
            s_toast = text;
            s_toastUntil = Seconds() + 3.0;
        }
        renderer::RefreshOverlay();
    }

    double GameTick(PPCContext& ctx, uint8_t* base, double step)
    {
        if (s_toggle.exchange(false) && Seconds() - s_toggleAt.load() < 0.5)
        {
            if (!s_active)
            {
                if (s_paused || Seconds() - s_lastThirdPerson.load() > 1.0)
                    SetToast("Photo mode works in third-person play (not in menus or cutscenes)");
                else
                {
                    {
                        std::lock_guard g(s_lock);
                        s_wantCapture = true;
                        s_haveCamera = false;
                        s_fov = options::Fov();
                        s_mouseX = s_mouseY = 0;
                        s_lastButtons = 0xFFFF;   // ignore buttons already held
                        s_hudToggleRequest = false;
                    }
                    s_active = true;
                    s_hudWant = false;
                    s_hudWas = HudUsable(base) ? PPC_LOAD_U8(s_hud.load() + 40) : 1;
                    Log("Photo mode on (HUD component %08X%s)", s_hud.load(),
                        HudUsable(base) ? "" : ", not found: the HUD stays");
                    renderer::RefreshOverlay();
                }
            }
            else
            {
                s_active = false;
                s_hudWant = true;
                Log("Photo mode off");
                renderer::RefreshOverlay();
            }
        }
        if (s_active)
        {
            std::lock_guard g(s_lock);
            if (s_hudToggleRequest)
            {
                s_hudToggleRequest = false;
                s_hudWant = !s_hudWant;
            }
        }
        if (!s_hudWant && !s_hudHidden && HudUsable(base))
        {
            SetHudVisible(ctx, base, false);
            s_hudHidden = true;
        }
        else if (s_hudWant && s_hudHidden)
        {
            SetHudVisible(ctx, base, s_hudWas != 0);   // as it was before
            s_hudHidden = false;
        }
        return s_active ? kFrozenStep : step;
    }

    bool OverrideView(uint8_t* base, uint32_t view, uint32_t proj, uint32_t out)
    {
        if (!s_active)
            return false;
        std::lock_guard g(s_lock);
        float m[16];
        for (int i = 0; i < 16; i++)
            m[i] = LoadF(base, view + i * 4);
        if (s_wantCapture)
        {
            s_wantCapture = false;
            // Row-vector matrices keep the translation in the last row.
            s_transposed = std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]) < 1e-6 &&
                           std::fabs(m[3]) + std::fabs(m[7]) + std::fabs(m[11]) > 1e-6;
            auto at = [&](int r, int c) { return double(s_transposed ? m[c * 4 + r] : m[r * 4 + c]); };
            // view = [R 0; t 1]: the camera's axes in world space are R's columns,
            // its position -t R^T.
            V3 col[3];
            for (int c = 0; c < 3; c++)
                col[c] = { at(0, c), at(1, c), at(2, c) };
            V3 t{ at(3, 0), at(3, 1), at(3, 2) };
            s_right = Normal(col[0]);
            s_up = Normal(col[1]);
            s_axis = Normal(col[2]);
            // t = -position R, so position = -t R^T = -(t.x col0 + t.y col1 + t.z col2).
            s_pos = (col[0] * t.x + col[1] * t.y + col[2] * t.z) * -1.0;
            double err = std::fabs(-Dot(s_pos, col[0]) - t.x) + std::fabs(-Dot(s_pos, col[1]) - t.y) +
                         std::fabs(-Dot(s_pos, col[2]) - t.z);
            if (err > 0.01)
                Log("Photo mode: the view matrix doesn't round-trip (error %.3f)", err);
            // The projection's w = +-z tells which way the view looks.
            float w1 = LoadF(base, proj + 44), w2 = LoadF(base, proj + 56);
            float w = std::fabs(w1) == 1.0f ? w1 : w2;
            s_forwardSign = w < 0 ? -1.0 : 1.0;
            // World up: the world axis closest to the camera's up.
            double ax[3] = { std::fabs(s_up.x), std::fabs(s_up.y), std::fabs(s_up.z) };
            int k = int(std::max_element(ax, ax + 3) - ax);
            s_worldUp = { k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0 };
            if (Dot(s_worldUp, s_up) < 0)
                s_worldUp = s_worldUp * -1.0;
            s_startRight = s_right, s_startUp = s_up, s_startAxis = s_axis, s_startPos = s_pos;
            s_haveCamera = true;
            s_firstFrame = true;
            s_lastUpdate = Seconds();
            Log("Photo mode camera at %.2f %.2f %.2f (%s matrices, looking along %saxis, up %c)", s_pos.x, s_pos.y,
                s_pos.z, s_transposed ? "column" : "row", s_forwardSign > 0 ? "+" : "-", "xyz"[k]);
        }
        if (!s_haveCamera)
            return false;

        // Input since the last update.
        double now = Seconds();
        double dt = std::clamp(now - s_lastUpdate, 0.0, 0.1);
        s_lastUpdate = now;
        uint16_t pressed = s_buttons & ~s_lastButtons;
        s_lastButtons = s_buttons;
        auto stick = [](int16_t v) {
            double f = v / 32767.0;
            return std::fabs(f) < 0.2 ? 0.0 : (f - (f > 0 ? 0.2 : -0.2)) / 0.8;
        };
        constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000, kLB = 0x0100, kRB = 0x0200,
                           kLThumb = 0x0040;
        s_fast = (s_buttons & kLThumb) != 0;

        // Look: mouse (same feel as the game's mouse look) and the right stick.
        double perPixel = 0.066 * options::MouseSensitivity() / 50.0 * kPi / 180.0 * (s_fov / 90.0);
        double yaw = s_mouseX * perPixel + stick(s_rx) * 2.0 * dt * (s_fov / 90.0);
        double pitch = -s_mouseY * perPixel + stick(s_ry) * 1.5 * dt * (s_fov / 90.0);
        if (options::MouseInvertY())
            pitch -= 2 * (-s_mouseY * perPixel);
        s_mouseX = s_mouseY = 0;
        V3 forward = s_axis * s_forwardSign;
        if (yaw != 0)
        {
            // Turn about world up, the way that swings the view towards its right.
            V3 k = s_worldUp;
            if (Dot(Cross(k, forward), s_right) < 0)
                k = k * -1.0;
            s_right = Rotate(s_right, k, yaw);
            s_up = Rotate(s_up, k, yaw);
            s_axis = Rotate(s_axis, k, yaw);
        }
        forward = s_axis * s_forwardSign;
        if (pitch != 0)
        {
            V3 k = s_right;
            if (Dot(Cross(k, forward), s_up) < 0)
                k = k * -1.0;
            V3 axis = Rotate(s_axis, k, pitch);
            // Not past straight up or down.
            if (std::fabs(Dot(axis * s_forwardSign, s_worldUp)) < 0.995)
            {
                s_axis = axis;
                s_up = Rotate(s_up, k, pitch);
            }
        }
        // Keep the axes square and level (no roll).
        forward = Normal(s_axis * s_forwardSign);
        V3 right = Cross(s_worldUp, forward);
        if (Dot(right, s_right) < 0)
            right = right * -1.0;
        s_right = Normal(right);
        V3 up = Cross(forward, s_right);
        if (Dot(up, s_worldUp) < 0)
            up = up * -1.0;
        s_up = Normal(up);
        s_axis = forward * s_forwardSign;

        // Move: left stick (WASD), up / down RB / LB (E / Q), faster with L3 (Shift).
        double speed = (s_fast ? 12.0 : 3.0) * dt;
        double vertical = ((s_buttons & kRB) ? 1.0 : 0.0) - ((s_buttons & kLB) ? 1.0 : 0.0);
        s_pos = s_pos + forward * (stick(s_ly) * speed) + s_right * (stick(s_lx) * speed) +
                s_worldUp * (vertical * speed);

        // Zoom: RT (left click) in, LT (right click) out.
        double zoom = (s_rt > 40 ? 1.0 : 0.0) - (s_lt > 40 ? 1.0 : 0.0);
        s_fov = std::clamp(s_fov - zoom * 45.0 * dt, 10.0, 130.0);

        if (pressed & kX)   // back to where photo mode started
        {
            s_right = s_startRight, s_up = s_startUp, s_axis = s_startAxis, s_pos = s_startPos;
            s_fov = options::Fov();
        }
        if (pressed & kY)
            s_hudToggleRequest = true;
        if (pressed & kA)
            renderer::SaveScreenshot();
        if (pressed & kB)
        {
            s_toggleAt = now;
            s_toggle = true;
        }

        // The new view: [R 0; t 1] with R's columns the axes, t = -position R.
        V3 cols[3] = { s_right, s_up, s_axis };
        double r[4][4] = {};
        for (int c = 0; c < 3; c++)
        {
            r[0][c] = cols[c].x;
            r[1][c] = cols[c].y;
            r[2][c] = cols[c].z;
            r[3][c] = -Dot(s_pos, cols[c]);
        }
        r[3][3] = 1;
        double diff = 0;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                uint32_t index = uint32_t(s_transposed ? j * 4 + i : i * 4 + j);
                diff = std::max(diff, std::fabs(double(m[index]) - r[i][j]));
                StoreF(base, out + index * 4, float(r[i][j]));
            }
        if (s_firstFrame)
        {
            // Right after the start nothing has moved: the view should be the game's.
            s_firstFrame = false;
            Log("Photo mode: first view differs from the game's by %.4f", diff);
        }
        return true;
    }
}

// HudComponentImpl's handler registration (this = the component) and destructor.
extern "C" void __imp__sub_828F2248(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_828F2248)
{
    // (Checked against the vtable when used: it may not be set yet here.)
    uint32_t hud = ctx.r3.u32;
    if (wet::photo::s_hud.exchange(hud) != hud)
        wet::Log("Photo: HUD component at %08X", hud);
    __imp__sub_828F2248(ctx, base);
}

extern "C" void __imp__sub_828F35E0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_828F35E0)
{
    uint32_t hud = ctx.r3.u32;
    wet::photo::s_hud.compare_exchange_strong(hud, 0u);
    __imp__sub_828F35E0(ctx, base);
}
