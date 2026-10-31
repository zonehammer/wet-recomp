#include "input.h"
#include "log.h"
#include "gpu/renderer.h"
#include "profiler.h"
#include "options.h"
#include "camscan.h"

#include <atomic>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif
#if WET_HAVE_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <thread>
#endif

// Controllers: with SDL2 (Windows builds), any pad SDL knows - PlayStation 4/5,
// Xbox, Switch Pro and others - through one input thread that owns SDL, with
// hot-plugging and rumble; pads take guest users 0-3 in the order they connect.
// Without SDL, or if it fails to start, XInput pads (Xbox-compatible only).
//
// Pad 0 is the union of: the first controller, the keyboard
// (Windows, see kKeyMap) and WET_AUTOPRESS (any platform), so the game can be
// driven with no controller attached and in automated test runs.
//
// WET_AUTOPRESS="START,A,A" taps each button in turn, one every 2 seconds
// (held for 0.2 s), starting WET_AUTOPRESS_DELAY seconds after launch (default 50,
// i.e. after the intro videos), repeating the list forever.

namespace wet
{
    namespace
    {
        constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008,
                           kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080,
                           kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

        struct NamedButton { const char* name; uint16_t bit; };
        constexpr NamedButton kNames[] = {
            { "UP", kUp }, { "DOWN", kDown }, { "LEFT", kLeft }, { "RIGHT", kRight },
            { "START", kStart }, { "BACK", kBack }, { "LTHUMB", kLThumb }, { "RTHUMB", kRThumb },
            { "LB", kLB }, { "RB", kRB }, { "A", kA }, { "B", kB }, { "X", kX }, { "Y", kY },
        };

        const auto s_start = std::chrono::steady_clock::now();

        struct AutoPress
        {
            std::vector<uint16_t> sequence;
            double delay = 50.0;

            AutoPress()
            {
                const char* env = std::getenv("WET_AUTOPRESS");
                if (!env || !*env)
                    return;
                if (const char* d = std::getenv("WET_AUTOPRESS_DELAY"))
                    delay = std::atof(d);
                std::string s(env);
                size_t p = 0;
                while (p <= s.size())
                {
                    size_t e = s.find(',', p);
                    if (e == std::string::npos) e = s.size();
                    std::string tok = s.substr(p, e - p);
                    for (auto& c : tok) c = char(std::toupper((unsigned char)c));
                    for (auto& n : kNames)
                        if (tok == n.name)
                            sequence.push_back(n.bit);
                    p = e + 1;
                }
                Log("Input: WET_AUTOPRESS with %zu buttons, starting after %.0f s", sequence.size(), delay);
            }

            uint16_t Buttons() const
            {
                if (sequence.empty())
                    return 0;
                double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count() - delay;
                if (t < 0)
                    return 0;
                double slot = t / 2.0;
                size_t i = size_t(slot);
                return (slot - double(i)) < 0.1 ? sequence[i % sequence.size()] : 0;
            }
        };
        const AutoPress& Auto() { static AutoPress a; return a; }

#ifdef _WIN32
        struct KeyBinding { int vk; uint16_t bit; };
        constexpr KeyBinding kKeyMap[] = {
            { VK_UP, kUp }, { VK_DOWN, kDown }, { VK_LEFT, kLeft }, { VK_RIGHT, kRight },
            { VK_RETURN, kStart }, { VK_BACK, kBack }, { VK_SPACE, kA }, { VK_ESCAPE, kB },
            { 'X', kX }, { 'Y', kY }, { 'Q', kLB }, { 'E', kRB },
        };

        // The keyboard only counts while this process's console (or, later, window)
        // is in the foreground, so typing elsewhere doesn't drive the game.
        bool OurWindowFocused()
        {
            HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            if (fg == GetConsoleWindow())
                return true;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            return pid == GetCurrentProcessId();
        }

        // ---- Mouse aim: mouse movement drives the right stick ----
        // While the game window is in front (and the F1 overlay closed) the cursor
        // is hidden, kept in the window and re-centred after every read; the
        // movement since the last read, as a speed, becomes a stick deflection.
        struct MouseState
        {
            bool captured = false;
            POINT center{};
            double accX = 0, accY = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
            double stickX = 0, stickY = 0;
        };
        MouseState s_mouse;
        std::mutex s_mouseLock;

        uint16_t BindingBits(const std::string& name, uint8_t& lt, uint8_t& rt)
        {
            if (name == "LT") { lt = 255; return 0; }
            if (name == "RT") { rt = 255; return 0; }
            for (auto& n : kNames)
                if (name == n.name)
                    return n.bit;
            return 0;
        }

        void ReleaseMouse()
        {
            if (!s_mouse.captured)
                return;
            ClipCursor(nullptr);
            renderer::SetCursorHidden(false);
            s_mouse.captured = false;
            s_mouse.stickX = s_mouse.stickY = 0;
        }

        // Fills the right stick and mouse-button presses; false if not aiming.
        bool MouseAim(int16_t& rx, int16_t& ry, uint16_t& buttons, uint8_t& lt, uint8_t& rt)
        {
            std::lock_guard lock(s_mouseLock);
            HWND fg = GetForegroundWindow();
            bool game = fg && fg != GetConsoleWindow() && options::MouseAim() && !renderer::OverlayOpen();
            if (game)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(fg, &pid);
                game = pid == GetCurrentProcessId();
            }
            if (!game)
            {
                ReleaseMouse();
                return false;
            }
            RECT rc;
            GetClientRect(fg, &rc);
            POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
            ClientToScreen(fg, &tl);
            ClientToScreen(fg, &br);
            RECT clip{ tl.x, tl.y, br.x, br.y };
            POINT center{ (tl.x + br.x) / 2, (tl.y + br.y) / 2 };
            if (!s_mouse.captured)
            {
                s_mouse.captured = true;
                renderer::SetCursorHidden(true);
                SetCursorPos(center.x, center.y);
                s_mouse.center = center;
                s_mouse.accX = s_mouse.accY = 0;
                s_mouse.last = std::chrono::steady_clock::now();
            }
            ClipCursor(&clip);
            POINT p;
            if (GetCursorPos(&p))
            {
                s_mouse.accX += p.x - s_mouse.center.x;
                s_mouse.accY += p.y - s_mouse.center.y;
            }
            SetCursorPos(center.x, center.y);
            s_mouse.center = center;
            // Turn the movement into a speed every few ms, so several reads in one
            // frame don't see zero and make the stick jitter.
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - s_mouse.last).count();
            if (dt >= 0.004)
            {
                dt = std::min(dt, 0.05);
                double vx = s_mouse.accX / dt, vy = s_mouse.accY / dt;   // pixels per second
                s_mouse.accX = s_mouse.accY = 0;
                s_mouse.last = now;
                // Sensitivity 50: 1200 px/s is a full deflection; 100: 600 px/s.
                double k = options::MouseSensitivity() / 100.0 / 600.0;
                double tx = vx * k, ty = -vy * k;
                if (options::MouseInvertY())
                    ty = -ty;
                double mag = std::sqrt(tx * tx + ty * ty);
                if (mag > 0.002)
                {
                    // Past the game's dead zone (~24%) so slow movements still turn.
                    constexpr double kDeadZone = 0.24;
                    double out = std::min(1.0, kDeadZone + (1.0 - kDeadZone) * mag);
                    tx = tx / mag * out;
                    ty = ty / mag * out;
                }
                else
                    tx = ty = 0;
                s_mouse.stickX = 0.5 * s_mouse.stickX + 0.5 * tx;
                s_mouse.stickY = 0.5 * s_mouse.stickY + 0.5 * ty;
                if (std::abs(s_mouse.stickX) < 0.01) s_mouse.stickX = 0;
                if (std::abs(s_mouse.stickY) < 0.01) s_mouse.stickY = 0;
            }
            rx = int16_t(std::clamp(s_mouse.stickX, -1.0, 1.0) * 32767);
            ry = int16_t(std::clamp(s_mouse.stickY, -1.0, 1.0) * 32767);
            static constexpr int kButtons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
            for (int i = 0; i < 5; i++)
                if (GetAsyncKeyState(kButtons[i]) & 0x8000)
                    buttons |= BindingBits(options::MouseBinding(i), lt, rt);
            return true;
        }

        uint16_t KeyboardButtons(uint8_t& lt, uint8_t& rt, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry)
        {
            if (!OurWindowFocused())
            {
                std::lock_guard lock(s_mouseLock);
                ReleaseMouse();
                return 0;
            }
            // F1: settings overlay.
            static bool f1 = false;
            bool f1Now = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
            if (f1Now && !f1)
                renderer::ToggleOverlay();
            f1 = f1Now;
            if (renderer::OverlayOpen())
            {
                std::lock_guard lock(s_mouseLock);
                ReleaseMouse();
                return 0;
            }
            uint16_t b = 0;
            for (auto& k : kKeyMap)
                if (GetAsyncKeyState(k.vk) & 0x8000)
                    b |= k.bit;
            if (GetAsyncKeyState('Z') & 0x8000) lt = 255;
            if (GetAsyncKeyState('C') & 0x8000) rt = 255;
            // WASD: left stick.
            int mx = ((GetAsyncKeyState('D') & 0x8000) ? 1 : 0) - ((GetAsyncKeyState('A') & 0x8000) ? 1 : 0);
            int my = ((GetAsyncKeyState('W') & 0x8000) ? 1 : 0) - ((GetAsyncKeyState('S') & 0x8000) ? 1 : 0);
            if (mx || my)
            {
                double scale = (mx && my) ? 0.7071 : 1.0;
                lx = int16_t(mx * 32767 * scale);
                ly = int16_t(my * 32767 * scale);
            }
            MouseAim(rx, ry, b, lt, rt);
            // F9: trace the next two frames' draws to xenia_gpu.log.
            static bool f9 = false;
            bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (f9Now && !f9)
            {
                Log("Input: F9 - tracing the next 2 frames' draws to xenia_gpu.log");
                renderer::RequestDrawTrace(2);
            }
            f9 = f9Now;
            // F6: 30 / 60 / 120 / 144 FPS.
            static bool f6 = false;
            bool f6Now = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            if (f6Now && !f6)
                options::CycleFps();
            f6 = f6Now;
            // F11: fullscreen.
            static bool f11 = false;
            bool f11Now = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (f11Now && !f11)
                renderer::ToggleFullscreen();
            f11 = f11Now;
            // F7: camera finder (camscan.txt) for direct mouse aim.
            static bool f7 = false;
            bool f7Now = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (f7Now && !f7)
            {
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                    camscan::StartPadReaders();
                else
                    camscan::Start();
            }
            f7 = f7Now;
            // F8: save the next 48 frames (frames/burst_*.png) for diagnosing glitches.
            static bool f8 = false;
            bool f8Now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8Now && !f8)
                renderer::RequestBurstCapture();
            f8 = f8Now;
            // F10: start / stop the sampling profiler (profile.txt).
            static bool f10 = false;
            bool f10Now = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
            if (f10Now && !f10)
                profiler::Toggle();
            f10 = f10Now;
            return b;
        }
#endif

        void LogButtons(uint16_t buttons)
        {
            static std::mutex lock;
            static uint16_t last = 0;
            static int logged = 0;
            std::lock_guard g(lock);
            if (buttons == last)
                return;
            uint16_t pressed = buttons & ~last;
            last = buttons;
            if (!pressed || logged >= 300)
                return;
            logged++;
            std::string names;
            for (auto& n : kNames)
                if (pressed & n.bit)
                    names += std::string(names.empty() ? "" : "+") + n.name;
            Log("Input: pressed %s%s", names.c_str(), logged == 300 ? " (further presses not logged)" : "");
        }

#if WET_HAVE_SDL
        // ---- SDL backend: one thread owns SDL (init, events, reads, rumble) ----
        struct SdlSlot
        {
            SDL_GameController* pad = nullptr;
            SDL_JoystickID id = -1;
            std::string name;
            HostPadState state;
            uint16_t wantLow = 0, wantHigh = 0;     // rumble the game asked for
            uint16_t sentLow = 0, sentHigh = 0;
            uint64_t sentAt = 0;
        };
        std::mutex s_sdlLock;
        SdlSlot s_sdl[4];
        std::atomic<int> s_sdlState{ 0 };   // 0 not started, 1 starting, 2 running, 3 failed

        uint16_t SdlButtons(SDL_GameController* c)
        {
            struct Map { SDL_GameControllerButton b; uint16_t bit; };
            static constexpr Map kMap[] = {
                { SDL_CONTROLLER_BUTTON_DPAD_UP, kUp }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown },
                { SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight },
                { SDL_CONTROLLER_BUTTON_START, kStart }, { SDL_CONTROLLER_BUTTON_BACK, kBack },
                { SDL_CONTROLLER_BUTTON_TOUCHPAD, kBack },      // PlayStation touchpad click
                { SDL_CONTROLLER_BUTTON_LEFTSTICK, kLThumb }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, kRThumb },
                { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kLB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kRB },
                // Positional: A = bottom (Cross), B = right (Circle), X = left (Square), Y = top (Triangle).
                { SDL_CONTROLLER_BUTTON_A, kA }, { SDL_CONTROLLER_BUTTON_B, kB },
                { SDL_CONTROLLER_BUTTON_X, kX }, { SDL_CONTROLLER_BUTTON_Y, kY },
            };
            uint16_t b = 0;
            for (auto& m : kMap)
                if (SDL_GameControllerGetButton(c, m.b))
                    b |= m.bit;
            return b;
        }

        int16_t InvertAxis(int16_t v) { return v == -32768 ? 32767 : int16_t(-v); }
        uint8_t TriggerByte(int16_t v) { return uint8_t(v <= 0 ? 0 : (int32_t(v) * 255 + 16383) / 32767); }

        void SdlOpen(int deviceIndex)
        {
            if (!SDL_IsGameController(deviceIndex))
                return;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
            std::lock_guard lock(s_sdlLock);
            for (auto& slot : s_sdl)
                if (slot.pad && slot.id == id)
                    return;
            for (int i = 0; i < 4; i++)
            {
                SdlSlot& slot = s_sdl[i];
                if (slot.pad)
                    continue;
                SDL_GameController* c = SDL_GameControllerOpen(deviceIndex);
                if (!c)
                    return;
                slot = SdlSlot{};
                slot.pad = c;
                slot.id = id;
                const char* name = SDL_GameControllerName(c);
                slot.name = name ? name : "Controller";
                SDL_GameControllerSetPlayerIndex(c, i);
                if (SDL_GameControllerHasLED(c))
                    SDL_GameControllerSetLED(c, 0xE0, 0x30, 0x10);   // light bar: WET orange-red
                Log("Input: controller %d connected: %s", i + 1, slot.name.c_str());
                return;
            }
            Log("Input: a 5th controller was ignored (the game supports 4)");
        }

        void SdlClose(SDL_JoystickID id)
        {
            std::lock_guard lock(s_sdlLock);
            for (int i = 0; i < 4; i++)
                if (s_sdl[i].pad && s_sdl[i].id == id)
                {
                    Log("Input: controller %d disconnected: %s", i + 1, s_sdl[i].name.c_str());
                    SDL_GameControllerClose(s_sdl[i].pad);
                    s_sdl[i] = SdlSlot{};
                }
        }

        void SdlThread()
        {
            SDL_SetMainReady();
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");   // device notifications on SDL's own thread
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
            if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
            {
                Log("Input: SDL could not start (%s); using XInput (Xbox controllers only)", SDL_GetError());
                s_sdlState = 3;
                return;
            }
            SDL_version v;
            SDL_GetVersion(&v);
            Log("Input: SDL %d.%d.%d controllers (PlayStation, Xbox and others)", v.major, v.minor, v.patch);
            s_sdlState = 2;
            for (;;)
            {
                // Messages for SDL's hidden windows created on this thread.
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                SDL_Event e;
                while (SDL_PollEvent(&e))
                {
                    if (e.type == SDL_CONTROLLERDEVICEADDED)
                        SdlOpen(e.cdevice.which);
                    else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                        SdlClose(e.cdevice.which);
                }
                uint64_t now = uint64_t(SDL_GetTicks64());
                {
                    std::lock_guard lock(s_sdlLock);
                    for (auto& slot : s_sdl)
                    {
                        if (!slot.pad)
                            continue;
                        HostPadState st;
                        st.buttons = SdlButtons(slot.pad);
                        st.leftTrigger = TriggerByte(SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
                        st.rightTrigger = TriggerByte(SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
                        st.lx = SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_LEFTX);
                        st.ly = InvertAxis(SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_LEFTY));
                        st.rx = SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_RIGHTX);
                        st.ry = InvertAxis(SDL_GameControllerGetAxis(slot.pad, SDL_CONTROLLER_AXIS_RIGHTY));
                        st.packet = slot.state.packet;
                        if (st.buttons != slot.state.buttons || st.leftTrigger != slot.state.leftTrigger ||
                            st.rightTrigger != slot.state.rightTrigger || st.lx != slot.state.lx ||
                            st.ly != slot.state.ly || st.rx != slot.state.rx || st.ry != slot.state.ry)
                            st.packet++;
                        slot.state = st;
                        // Rumble: send changes at once, and refresh a running one before
                        // SDL's duration runs out.
                        uint16_t low = options::Vibration() ? slot.wantLow : 0;
                        uint16_t high = options::Vibration() ? slot.wantHigh : 0;
                        if (low != slot.sentLow || high != slot.sentHigh || ((low || high) && now - slot.sentAt > 500))
                        {
                            SDL_GameControllerRumble(slot.pad, low, high, 1000);
                            slot.sentLow = low;
                            slot.sentHigh = high;
                            slot.sentAt = now;
                        }
                    }
                }
                SDL_Delay(2);
            }
        }

        // True once SDL is running (starts it on first use).
        bool SdlReady()
        {
            int expected = 0;
            if (s_sdlState.compare_exchange_strong(expected, 1))
                std::thread(SdlThread).detach();
            while (s_sdlState.load() == 1)
                std::this_thread::yield();
            return s_sdlState.load() == 2;
        }
#endif

#ifdef _WIN32
        // XInputGetState on an empty slot can take a millisecond or more; look at
        // empty slots again only every second (hot-plugging).
        bool XInputRead(uint32_t user, XINPUT_STATE& s)
        {
            static std::atomic<uint64_t> s_retryAt[4];
            uint64_t now = uint64_t(GetTickCount64());
            if (user >= 4 || now < s_retryAt[user].load(std::memory_order_relaxed))
                return false;
            if (XInputGetState(user, &s) == ERROR_SUCCESS)
                return true;
            s_retryAt[user] = now + 1000;
            return false;
        }
#endif
    }

    std::vector<std::string> ConnectedControllers()
    {
        std::vector<std::string> out;
#if WET_HAVE_SDL
        if (SdlReady())
        {
            std::lock_guard lock(s_sdlLock);
            for (auto& slot : s_sdl)
                out.push_back(slot.pad ? slot.name : std::string());
            return out;
        }
#endif
#ifdef _WIN32
        for (uint32_t i = 0; i < 4; i++)
        {
            XINPUT_STATE s;
            out.push_back(XInputRead(i, s) ? "Xbox controller (XInput)" : "");
        }
#endif
        return out;
    }

    namespace
    {
        std::atomic<bool> s_stickOverride{ false };
        std::atomic<int> s_overrideRx{ 0 }, s_overrideRy{ 0 };
    }

    void SetRightStickOverride(bool on, int16_t rx, int16_t ry)
    {
        s_overrideRx = rx;
        s_overrideRy = ry;
        s_stickOverride = on;
    }

    bool HostPadConnected(uint32_t user)
    {
#if WET_HAVE_SDL
        if (SdlReady())
        {
            std::lock_guard lock(s_sdlLock);
            return user == 0 || (user < 4 && s_sdl[user].pad);
        }
#endif
#ifdef _WIN32
        XINPUT_STATE s;
        if (XInputRead(user, s))
            return true;
#endif
        return user == 0;   // keyboard / virtual pad
    }

    bool HostPadGetState(uint32_t user, HostPadState& out)
    {
        out = {};
        bool connected = false;
#if WET_HAVE_SDL
        bool viaSdl = SdlReady();
        if (viaSdl)
        {
            std::lock_guard lock(s_sdlLock);
            if (user < 4 && s_sdl[user].pad)
            {
                connected = true;
                out = s_sdl[user].state;
            }
        }
#else
        constexpr bool viaSdl = false;
#endif
        (void)viaSdl;
#ifdef _WIN32
        XINPUT_STATE s;
        if (!viaSdl && XInputRead(user, s))
        {
            connected = true;
            out.packet = s.dwPacketNumber;
            out.buttons = s.Gamepad.wButtons;
            out.leftTrigger = s.Gamepad.bLeftTrigger;
            out.rightTrigger = s.Gamepad.bRightTrigger;
            out.lx = s.Gamepad.sThumbLX;
            out.ly = s.Gamepad.sThumbLY;
            out.rx = s.Gamepad.sThumbRX;
            out.ry = s.Gamepad.sThumbRY;
        }
#endif
        if (user != 0)
            return connected;

        uint16_t extra = Auto().Buttons();
        int16_t klx = 0, kly = 0, krx = 0, kry = 0;
#ifdef _WIN32
        extra |= KeyboardButtons(out.leftTrigger, out.rightTrigger, klx, kly, krx, kry);
#endif
        // Keyboard / mouse sticks win over a resting controller stick.
        auto merge = [](int16_t& pad, int16_t kb) {
            if (kb && std::abs(int(kb)) > std::abs(int(pad)))
                pad = kb;
        };
        merge(out.lx, klx);
        merge(out.ly, kly);
        merge(out.rx, krx);
        merge(out.ry, kry);
        if (s_stickOverride.load())
        {
            out.rx = krx = int16_t(s_overrideRx.load());   // (krx/kry feed the packet number below)
            out.ry = kry = int16_t(s_overrideRy.load());
        }
        // The packet number must change whenever the state does.
        static std::atomic<uint64_t> s_lastExtra{ 0 };
        static std::atomic<uint32_t> s_extraPackets{ 0 };
        uint64_t extraState = (uint64_t(extra) | uint64_t(uint16_t(klx)) << 16 | uint64_t(uint16_t(kly)) << 32 |
                               uint64_t(uint16_t(krx ^ (kry << 3))) << 48) ^
                              (uint64_t(out.leftTrigger) << 8) ^ (uint64_t(out.rightTrigger) << 24);
        if (s_lastExtra.exchange(extraState) != extraState)
            s_extraPackets++;
        out.packet += s_extraPackets.load() << 20;
        out.buttons |= extra;
        LogButtons(out.buttons);
        return true;
    }

    bool HostPadSetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
#if WET_HAVE_SDL
        if (SdlReady())
        {
            std::lock_guard lock(s_sdlLock);
            if (user < 4 && s_sdl[user].pad)
            {
                s_sdl[user].wantLow = left;
                s_sdl[user].wantHigh = right;
                return true;
            }
            return user == 0;
        }
#endif
#ifdef _WIN32
        if (!options::Vibration())
            left = right = 0;
        XINPUT_VIBRATION v{ left, right };
        if (user < 4 && XInputSetState(user, &v) == ERROR_SUCCESS)
            return true;
#else
        (void)left; (void)right;
#endif
        return user == 0;
    }
}
