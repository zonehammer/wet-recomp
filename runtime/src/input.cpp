#include "input.h"

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>

namespace wet
{
    bool HostPadConnected(uint32_t user)
    {
        XINPUT_STATE s;
        return user < 4 && XInputGetState(user, &s) == ERROR_SUCCESS;
    }

    bool HostPadGetState(uint32_t user, HostPadState& out)
    {
        XINPUT_STATE s;
        if (user >= 4 || XInputGetState(user, &s) != ERROR_SUCCESS)
            return false;
        out.packet = s.dwPacketNumber;
        out.buttons = s.Gamepad.wButtons;
        out.leftTrigger = s.Gamepad.bLeftTrigger;
        out.rightTrigger = s.Gamepad.bRightTrigger;
        out.lx = s.Gamepad.sThumbLX;
        out.ly = s.Gamepad.sThumbLY;
        out.rx = s.Gamepad.sThumbRX;
        out.ry = s.Gamepad.sThumbRY;
        return true;
    }

    bool HostPadSetVibration(uint32_t user, uint16_t left, uint16_t right)
    {
        XINPUT_VIBRATION v{ left, right };
        return user < 4 && XInputSetState(user, &v) == ERROR_SUCCESS;
    }
}
#else
namespace wet
{
    bool HostPadConnected(uint32_t user) { return user == 0; }
    bool HostPadGetState(uint32_t user, HostPadState& out)
    {
        if (user != 0)
            return false;
        out = {};
        return true;
    }
    bool HostPadSetVibration(uint32_t user, uint16_t, uint16_t) { return user == 0; }
}
#endif
