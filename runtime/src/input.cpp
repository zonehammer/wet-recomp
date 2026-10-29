#include "input.h"
#include "log.h"
#include "gpu/renderer.h"
#include "profiler.h"

#include <atomic>
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

// Pad 0 is the union of: a real XInput controller (Windows), the keyboard
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

        uint16_t KeyboardButtons(uint8_t& lt, uint8_t& rt)
        {
            if (!OurWindowFocused())
                return 0;
            uint16_t b = 0;
            for (auto& k : kKeyMap)
                if (GetAsyncKeyState(k.vk) & 0x8000)
                    b |= k.bit;
            if (GetAsyncKeyState('Z') & 0x8000) lt = 255;
            if (GetAsyncKeyState('C') & 0x8000) rt = 255;
            // F9: trace the next two frames' draws to xenia_gpu.log.
            static bool f9 = false;
            bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (f9Now && !f9)
            {
                Log("Input: F9 - tracing the next 2 frames' draws to xenia_gpu.log");
                renderer::RequestDrawTrace(2);
            }
            f9 = f9Now;
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
    }

    bool HostPadConnected(uint32_t user)
    {
#ifdef _WIN32
        XINPUT_STATE s;
        if (user < 4 && XInputGetState(user, &s) == ERROR_SUCCESS)
            return true;
#endif
        return user == 0;   // keyboard / virtual pad
    }

    bool HostPadGetState(uint32_t user, HostPadState& out)
    {
        out = {};
        bool connected = false;
#ifdef _WIN32
        XINPUT_STATE s;
        if (user < 4 && XInputGetState(user, &s) == ERROR_SUCCESS)
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
#ifdef _WIN32
        extra |= KeyboardButtons(out.leftTrigger, out.rightTrigger);
#endif
        // The packet number must change whenever the state does.
        static std::atomic<uint16_t> s_lastExtra{ 0 };
        static std::atomic<uint32_t> s_extraPackets{ 0 };
        if (s_lastExtra.exchange(extra) != extra)
            s_extraPackets++;
        out.packet += s_extraPackets.load() << 20;
        out.buttons |= extra;
        LogButtons(out.buttons);
        return true;
    }

    bool HostPadSetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
#ifdef _WIN32
        XINPUT_VIBRATION v{ left, right };
        if (user < 4 && XInputSetState(user, &v) == ERROR_SUCCESS)
            return true;
#else
        (void)left; (void)right;
#endif
        return user == 0;
    }
}
