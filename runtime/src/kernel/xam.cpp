// XAM: users, input, notifications, loader, title/module info, console settings.
#include "kernel.h"
#include "module.h"
#include "objects.h"
#include "input.h"
#include "os.h"

using namespace wet;

// ---------------------------------------------------------------------------
// Console / title info
// ---------------------------------------------------------------------------
GUEST_IMPORT(XGetLanguage) { Ret(ctx, 1); }        // English
GUEST_IMPORT(XGetGameRegion) { Ret(ctx, 0xFFFF); } // region free

GUEST_IMPORT(XamGetExecutionId)
{
    St32(Arg(ctx, 0), g_module.executionId);
    Ret(ctx, STATUS_SUCCESS);
}

// ExGetXConfigSetting(category, setting, buffer*, buffer_size, required_size*)
GUEST_IMPORT(ExGetXConfigSetting)
{
    uint16_t category = uint16_t(Arg(ctx, 0)), setting = uint16_t(Arg(ctx, 1));
    uint32_t buffer = Arg(ctx, 2), bufferSize = Arg(ctx, 3), required = Arg(ctx, 4);
    uint8_t value[16] = {};
    uint32_t size = 4;
    auto be32 = [&](uint32_t v) { value[0] = uint8_t(v >> 24); value[1] = uint8_t(v >> 16); value[2] = uint8_t(v >> 8); value[3] = uint8_t(v); };

    if (category == 0x0002 && setting == 0x0002)       be32(0x00001000);  // SECURED: AV region (NTSC-M)
    else if (category == 0x0003 && setting == 0x0009)  be32(1);           // USER: language English
    else if (category == 0x0003 && setting == 0x000A)  be32(0x00040000);  // USER: video flags (widescreen)
    else if (category == 0x0003 && setting == 0x000B)  be32(0x00010001);  // USER: audio flags
    else if (category == 0x0003 && setting == 0x000C)  be32(0x00000040);  // USER: retail flags
    else if (category == 0x0003 && setting == 0x000E) { value[0] = 103; size = 1; } // USER: country (US)
    else
        Log("ExGetXConfigSetting(%04X, %04X): unknown setting, returning zeros", category, setting);

    if (buffer)
    {
        if (bufferSize < size)
            return Ret(ctx, STATUS_BUFFER_TOO_SMALL);
        Zero(buffer, bufferSize);
        std::memcpy(HostPtr(buffer), value, size);
    }
    if (required)
        St16(required, uint16_t(size));
    Ret(ctx, STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

// XexGetModuleHandle(const char* name, HANDLE* out)
GUEST_IMPORT(XexGetModuleHandle)
{
    std::string name = ReadCString(Arg(ctx, 0));
    uint32_t h = 0;
    if (name.empty() || name == "default.xex")
        h = g_module.ldrEntry;
    else
        Log("XexGetModuleHandle(\"%s\"): not available", name.c_str());
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), h);
    Ret(ctx, h ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL);
}

// XexGetProcedureAddress(module, ordinal, void** out)
GUEST_IMPORT(XexGetProcedureAddress)
{
    Log("XexGetProcedureAddress(module 0x%08X, ordinal/name 0x%08X): not supported", Arg(ctx, 0), Arg(ctx, 1));
    if (Arg(ctx, 2))
        St32(Arg(ctx, 2), 0);
    Ret(ctx, STATUS_PROCEDURE_NOT_FOUND);
}

// XexCheckExecutablePrivilege(privilege) -> BOOL (privileges 0-31 are the system flags)
GUEST_IMPORT(XexCheckExecutablePrivilege)
{
    uint32_t p = Arg(ctx, 0);
    Ret(ctx, p < 32 ? (g_module.systemFlags >> p) & 1 : 0);
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------
GUEST_IMPORT(XamLoaderGetLaunchDataSize)
{
    if (Arg(ctx, 0))
        St32(Arg(ctx, 0), 0);
    Ret(ctx, ERROR_NOT_FOUND);
}

GUEST_IMPORT(XamLoaderGetLaunchData) { Ret(ctx, ERROR_NOT_FOUND); }
GUEST_IMPORT(XamLoaderSetLaunchData) { Ret(ctx, ERROR_SUCCESS); }

GUEST_IMPORT(XamLoaderTerminateTitle)
{
    Log("XamLoaderTerminateTitle: title exited");
    os::ExitProcess(0);
}

GUEST_IMPORT(XamLoaderLaunchTitle)
{
    Log("XamLoaderLaunchTitle(\"%s\", flags 0x%X): launching other titles is not supported; exiting",
        ReadCString(Arg(ctx, 0)).c_str(), Arg(ctx, 1));
    os::ExitProcess(0);
}

// ---------------------------------------------------------------------------
// Users: one local profile signed in on controller 0
// ---------------------------------------------------------------------------
constexpr uint64_t kXuid = 0xB13EBABEBABEBABEull;

GUEST_IMPORT(XamUserGetSigninState)
{
    Ret(ctx, Arg(ctx, 0) == 0 ? 1 /* signed in locally */ : 0);
}

// XamUserGetXUID(user, type_mask, u64* xuid)
GUEST_IMPORT(XamUserGetXUID)
{
    if (Arg(ctx, 0) != 0)
        return Ret(ctx, ERROR_NO_SUCH_USER);
    St64(Arg(ctx, 2), kXuid);
    Ret(ctx, ERROR_SUCCESS);
}

// XamUserGetName(user, char* buffer, buffer_len)
GUEST_IMPORT(XamUserGetName)
{
    if (Arg(ctx, 0) != 0)
        return Ret(ctx, ERROR_NO_SUCH_USER);
    std::string name = "Player";
    uint32_t len = Arg(ctx, 2);
    if (len)
    {
        name.resize(std::min<size_t>(name.size(), len - 1));
        WriteCString(Arg(ctx, 1), name);
    }
    Ret(ctx, ERROR_SUCCESS);
}

// ---------------------------------------------------------------------------
// Input. X_INPUT_STATE { be32 packet; X_INPUT_GAMEPAD { be16 buttons; u8 lt, rt;
// be16 lx, ly, rx, ry; } } - the same as XINPUT_STATE but big-endian.
// ---------------------------------------------------------------------------
// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*)
GUEST_IMPORT(XamInputGetCapabilities)
{
    uint32_t user = Arg(ctx, 0) == 0xFF ? 0 : Arg(ctx, 0), caps = Arg(ctx, 2);
    if (!HostPadConnected(user))
        return Ret(ctx, ERROR_DEVICE_NOT_CONNECTED);
    Zero(caps, 0x14);
    St8(caps + 0, 1);           // type: gamepad
    St8(caps + 1, 1);           // subtype: gamepad
    St16(caps + 4, 0xFFFF);     // buttons supported
    St8(caps + 6, 0xFF);
    St8(caps + 7, 0xFF);
    for (int i = 0; i < 4; i++)
        St16(caps + 8 + i * 2, 0xFFC0);
    St16(caps + 0x10, 0xFFFF);  // vibration motors
    St16(caps + 0x12, 0xFFFF);
    Ret(ctx, ERROR_SUCCESS);
}

// XamInputGetState(user, flags, X_INPUT_STATE*)
GUEST_IMPORT(XamInputGetState)
{
    uint32_t user = Arg(ctx, 0) == 0xFF ? 0 : Arg(ctx, 0), state = Arg(ctx, 2);
    HostPadState s;
    if (!HostPadGetState(user, s))
        return Ret(ctx, ERROR_DEVICE_NOT_CONNECTED);
    St32(state + 0, s.packet);
    St16(state + 4, s.buttons);
    St8(state + 6, s.leftTrigger);
    St8(state + 7, s.rightTrigger);
    St16(state + 8, uint16_t(s.lx));
    St16(state + 10, uint16_t(s.ly));
    St16(state + 12, uint16_t(s.rx));
    St16(state + 14, uint16_t(s.ry));
    Ret(ctx, ERROR_SUCCESS);
}

// XamInputSetState(user, flags, X_INPUT_VIBRATION* { be16 left; be16 right; })
GUEST_IMPORT(XamInputSetState)
{
    uint32_t user = Arg(ctx, 0) == 0xFF ? 0 : Arg(ctx, 0);
    if (!HostPadSetVibration(user, Ld16(Arg(ctx, 2)), Ld16(Arg(ctx, 2) + 2)))
        return Ret(ctx, ERROR_DEVICE_NOT_CONNECTED);
    Ret(ctx, ERROR_SUCCESS);
}

GUEST_IMPORT(XamInputGetKeystrokeEx) { Ret(ctx, ERROR_EMPTY); }

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

// XamNotifyCreateListener(u64 mask, max_version) -> handle
GUEST_IMPORT(XamNotifyCreateListener)
{
    auto l = std::make_shared<KNotifyListener>(ctx.r3.u64);
    uint32_t h = CreateHandle(l);
    Log("XamNotifyCreateListener(mask 0x%llX) -> 0x%X", (unsigned long long)ctx.r3.u64, h);
    Ret(ctx, h);
}

// XNotifyGetNext(handle, match_id, u32* id, u32* param) -> BOOL
GUEST_IMPORT(XNotifyGetNext)
{
    auto l = LookupHandleAs<KNotifyListener>(Arg(ctx, 0), ObjType::NotifyListener);
    if (!l)
        return Ret(ctx, 0);
    std::lock_guard lock(DispatchLock());
    uint32_t match = Arg(ctx, 1);
    for (auto it = l->queue.begin(); it != l->queue.end(); ++it)
    {
        if (match && it->id != match)
            continue;
        if (Arg(ctx, 2)) St32(Arg(ctx, 2), it->id);
        if (Arg(ctx, 3)) St32(Arg(ctx, 3), it->param);
        l->queue.erase(it);
        return Ret(ctx, 1);
    }
    Ret(ctx, 0);
}

GUEST_IMPORT(XNotifyPositionUI) {}
