// XAM content (save games), the storage-device selector, enumerators, profile
// settings and other "system UI" calls that complete through an XOVERLAPPED.
//
// There is one storage device, the hard disk (id 1). Content lives in
//   <hdd>/content/<title id>/<content type>/<file name>/
// with the display name kept beside it in <file name>.name (UTF-16LE).
#include "kernel.h"
#include "fs.h"
#include "log.h"
#include "module.h"
#include "objects.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

using namespace wet;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kHddDeviceId = 1;
    constexpr uint32_t kContentDataSize = 0x134;    // XCONTENT_DATA
    constexpr uint32_t XN_SYS_UI = 0x00000009;
    constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0000000A;
    constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
    constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
    constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
    constexpr uint32_t ERROR_INVALID_HANDLE = 6;

    // ---- XOVERLAPPED { be32 InternalLow (result), InternalHigh (length), InternalContext,
    //      hEvent, pCompletionRoutine, dwCompletionContext, dwExtendedError } ----
    void CompleteOverlapped(uint32_t ov, uint32_t result, uint32_t extendedError, uint32_t length)
    {
        St32(ov + 0x18, extendedError);
        St32(ov + 4, length);
        St32(ov + 0, result);
        if (uint32_t ev = Ld32(ov + 0xC))
            SignalEventHandle(ev);
        if (Ld32(ov + 0x10))
            Log("XOVERLAPPED 0x%08X has a completion routine 0x%08X: not supported", ov, Ld32(ov + 0x10));
    }

    // Finishes a call that may be overlapped: completes the XOVERLAPPED and returns
    // ERROR_IO_PENDING, or returns the result directly.
    uint32_t Finish(uint32_t ov, uint32_t result, uint32_t length = 0)
    {
        if (!ov)
            return result;
        CompleteOverlapped(ov, result, result, result ? 0xFFFFFFFF : length);
        return ERROR_IO_PENDING;
    }

    // A system dialog: XN_SYS_UI on, the answer a moment later, then XN_SYS_UI off,
    // as the dashboard does (titles often pause their own UI in between).
    uint32_t RunDialog(const char* name, uint32_t ov, std::function<uint32_t()> answer)
    {
        Log("%s: answered automatically (no system UI)", name);
        BroadcastNotification(XN_SYS_UI, 1);
        if (!ov)
        {
            uint32_t r = answer();
            BroadcastNotification(XN_SYS_UI, 0);
            return r;
        }
        St32(ov + 0, ERROR_IO_PENDING);
        std::thread([ov, answer] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint32_t r = answer();
            CompleteOverlapped(ov, r, r, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            BroadcastNotification(XN_SYS_UI, 0);
        }).detach();
        return ERROR_IO_PENDING;
    }

    // ---- Content ----
    struct ContentData
    {
        uint32_t deviceId = 0, type = 0;
        std::u16string displayName;
        std::string fileName;
    };

    ContentData ReadContentData(uint32_t p)
    {
        ContentData d;
        d.deviceId = Ld32(p);
        d.type = Ld32(p + 4);
        d.displayName = ReadWideCString(p + 8, 128);
        d.fileName = ReadCString(p + 8 + 256, 42);
        return d;
    }

    void WriteContentData(uint32_t p, const ContentData& d)
    {
        Zero(p, kContentDataSize);
        St32(p, d.deviceId);
        St32(p + 4, d.type);
        std::u16string name = d.displayName.substr(0, 127);
        WriteWideCString(p + 8, name);
        WriteCString(p + 8 + 256, d.fileName.substr(0, 41));
    }

    fs::path TypeDir(uint32_t type)
    {
        char t[16], tid[16];
        std::snprintf(t, sizeof(t), "%08X", type);
        std::snprintf(tid, sizeof(tid), "%08X", g_module.titleId);
        return HddRoot() / "content" / tid / t;
    }

    bool SafeFileName(const std::string& n)
    {
        return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:") == std::string::npos;
    }

    void SaveDisplayName(const ContentData& d)
    {
        std::ofstream f(TypeDir(d.type) / (d.fileName + ".name"), std::ios::binary);
        for (char16_t c : d.displayName)
        {
            f.put(char(c & 0xFF));
            f.put(char(c >> 8));
        }
    }

    std::u16string LoadDisplayName(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::u16string s;
        int lo, hi;
        while ((lo = f.get()) != EOF && (hi = f.get()) != EOF)
            s.push_back(char16_t(lo | (hi << 8)));
        return s;
    }

    std::mutex s_contentLock;
    std::map<std::string, fs::path> s_openRoots; // lower-case root -> folder

    std::string RootKey(std::string r)
    {
        for (auto& c : r) c = char(tolower((unsigned char)c));
        if (!r.empty() && r.back() == ':') r.pop_back();
        return r;
    }

    uint32_t CreateEnumeratorHandle(std::shared_ptr<KEnumerator> e, uint32_t bufferSizePtr, uint32_t handlePtr)
    {
        if (bufferSizePtr)
            St32(bufferSizePtr, e->itemSize * e->itemsPerCall);
        St32(handlePtr, CreateHandle(e));
        return ERROR_SUCCESS;
    }
}

// XamShowDeviceSelectorUI(user, content_type, content_flags, u64 total_requested, device_id*, overlapped)
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    uint32_t deviceIdPtr = Arg(ctx, 4);
    Ret(ctx, RunDialog("XamShowDeviceSelectorUI", Arg(ctx, 5), [deviceIdPtr] {
        if (deviceIdPtr)
            St32(deviceIdPtr, kHddDeviceId);
        return ERROR_SUCCESS;
    }));
}

// XamContentGetDeviceState(device_id, overlapped)
GUEST_IMPORT(XamContentGetDeviceState)
{
    uint32_t ov = Arg(ctx, 1);
    if (Arg(ctx, 0) != kHddDeviceId)
    {
        if (!ov)
            return Ret(ctx, ERROR_DEVICE_NOT_CONNECTED);
        CompleteOverlapped(ov, ERROR_FUNCTION_FAILED, ERROR_DEVICE_NOT_CONNECTED, 0);
        return Ret(ctx, ERROR_IO_PENDING);
    }
    Ret(ctx, Finish(ov, ERROR_SUCCESS));
}

// XamContentCreateEx(user, root_name, XCONTENT_DATA*, flags, disposition*, license_mask*,
//                    cache_size, u64 content_size, overlapped)
GUEST_IMPORT(XamContentCreateEx)
{
    std::string root = ReadCString(Arg(ctx, 1), 64);
    ContentData d = ReadContentData(Arg(ctx, 2));
    uint32_t flags = Arg(ctx, 3), dispPtr = Arg(ctx, 4), licPtr = Arg(ctx, 5), ov = Arg(ctx, 8);

    uint32_t result = ERROR_SUCCESS, disposition = 0;
    if (!SafeFileName(d.fileName) || root.empty())
        result = ERROR_INVALID_PARAMETER;
    else
    {
        fs::path dir = TypeDir(d.type) / d.fileName;
        std::error_code ec;
        bool exists = fs::is_directory(dir, ec);
        switch (flags & 0xF)
        {
        case 1: if (exists) result = ERROR_ALREADY_EXISTS; else disposition = 1; break;  // CREATE_NEW
        case 2: if (exists) fs::remove_all(dir, ec); disposition = 1; break;             // CREATE_ALWAYS
        case 3: if (!exists) result = ERROR_PATH_NOT_FOUND; else disposition = 2; break; // OPEN_EXISTING
        case 4: disposition = exists ? 2 : 1; break;                                     // OPEN_ALWAYS
        case 5:                                                                           // TRUNCATE_EXISTING
            if (!exists) result = ERROR_PATH_NOT_FOUND;
            else { fs::remove_all(dir, ec); disposition = 1; }
            break;
        default: result = ERROR_INVALID_PARAMETER; break;
        }
        if (disposition == 1)
        {
            fs::create_directories(dir, ec);
            SaveDisplayName(d);
        }
        if (disposition)
        {
            std::lock_guard lock(s_contentLock);
            s_openRoots[RootKey(root)] = dir;
            MountHostFolder(RootKey(root), dir, true);
        }
    }
    Log("XamContentCreateEx(\"%s\", type %08X, \"%s\", flags 0x%X) -> %u (%s)", root.c_str(), d.type,
        d.fileName.c_str(), flags, result, disposition == 1 ? "created" : disposition == 2 ? "opened" : "failed");
    if (dispPtr)
        St32(dispPtr, disposition);
    if (licPtr && result == ERROR_SUCCESS)
        St32(licPtr, 0);
    if (!ov)
        return Ret(ctx, result);
    CompleteOverlapped(ov, result, result, disposition);
    Ret(ctx, ERROR_IO_PENDING);
}

// XamContentClose(root_name, overlapped)
GUEST_IMPORT(XamContentClose)
{
    std::string root = RootKey(ReadCString(Arg(ctx, 0), 64));
    uint32_t result;
    {
        std::lock_guard lock(s_contentLock);
        result = s_openRoots.erase(root) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
    }
    UnmountHostFolder(root);
    Ret(ctx, Finish(Arg(ctx, 1), result));
}

// XamContentFlush(root_name, overlapped)
GUEST_IMPORT(XamContentFlush)
{
    Ret(ctx, Finish(Arg(ctx, 1), ERROR_SUCCESS));
}

// XamContentDelete(user, XCONTENT_DATA*, overlapped)
GUEST_IMPORT(XamContentDelete)
{
    ContentData d = ReadContentData(Arg(ctx, 1));
    uint32_t result = ERROR_FILE_NOT_FOUND;
    if (SafeFileName(d.fileName))
    {
        std::error_code ec;
        fs::path dir = TypeDir(d.type) / d.fileName;
        if (fs::is_directory(dir, ec))
        {
            fs::remove_all(dir, ec);
            fs::remove(TypeDir(d.type) / (d.fileName + ".name"), ec);
            result = ERROR_SUCCESS;
        }
    }
    Log("XamContentDelete(type %08X, \"%s\") -> %u", d.type, d.fileName.c_str(), result);
    Ret(ctx, Finish(Arg(ctx, 2), result));
}

// XamContentCreateEnumerator(user, device_id, content_type, flags, items_per_enum, buffer_size*, handle*)
GUEST_IMPORT(XamContentCreateEnumerator)
{
    uint32_t device = Arg(ctx, 1), type = Arg(ctx, 2), perCall = Arg(ctx, 4);
    if ((device && device != kHddDeviceId) || !Arg(ctx, 6))
    {
        if (Arg(ctx, 5))
            St32(Arg(ctx, 5), 0);
        return Ret(ctx, 0x80070057); // E_INVALIDARG
    }
    auto e = std::make_shared<KEnumerator>(kContentDataSize, perCall ? perCall : 1);
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(TypeDir(type), ec))
    {
        if (!entry.is_directory(ec))
            continue;
        ContentData d;
        d.deviceId = kHddDeviceId;
        d.type = type;
        d.fileName = entry.path().filename().string();
        d.displayName = LoadDisplayName(TypeDir(type) / (d.fileName + ".name"));
        std::vector<uint8_t> item(kContentDataSize);
        uint32_t tmp = SystemAlloc(kContentDataSize);
        WriteContentData(tmp, d);
        std::memcpy(item.data(), HostPtr(tmp), kContentDataSize);
        SystemFree(tmp);
        e->items.push_back(std::move(item));
    }
    Log("XamContentCreateEnumerator(device %u, type %08X): %zu items", device, type, e->items.size());
    Ret(ctx, CreateEnumeratorHandle(e, Arg(ctx, 5), Arg(ctx, 6)));
}

// XamUserCreateAchievementEnumerator(title_id, user, xuid, flags, offset, count, buffer_size*, handle*)
GUEST_IMPORT(XamUserCreateAchievementEnumerator)
{
    uint32_t count = Arg(ctx, 5);
    if (!count || !Arg(ctx, 6) || !Arg(ctx, 7))
        return Ret(ctx, ERROR_INVALID_PARAMETER);
    // X_ACHIEVEMENT_DETAILS (0x24) plus string space when details are requested.
    uint32_t size = 0x24 + ((Arg(ctx, 3) & 7) ? 0x8A0 : 0);
    Ret(ctx, CreateEnumeratorHandle(std::make_shared<KEnumerator>(size, count), Arg(ctx, 6), Arg(ctx, 7)));
}

// XamEnumerate(handle, flags, buffer, buffer_length, items_returned*, overlapped)
GUEST_IMPORT(XamEnumerate)
{
    auto obj = LookupHandle(Arg(ctx, 0));
    auto e = std::dynamic_pointer_cast<KEnumerator>(obj);
    if (!e)
        return Ret(ctx, ERROR_INVALID_HANDLE);
    uint32_t buffer = Arg(ctx, 2), length = Arg(ctx, 3), returned = Arg(ctx, 4), ov = Arg(ctx, 5);
    uint32_t result, count = 0;
    if (!buffer)
        result = ERROR_INVALID_PARAMETER;
    else
    {
        std::lock_guard lock(s_contentLock);
        uint32_t room = e->itemSize ? length / e->itemSize : 0;
        if (room > e->itemsPerCall)
            room = e->itemsPerCall;
        while (count < room && e->next < e->items.size())
        {
            std::memcpy(HostPtr(buffer + count * e->itemSize), e->items[e->next].data(), e->itemSize);
            e->next++;
            count++;
        }
        result = count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
    }
    if (ov)
    {
        CompleteOverlapped(ov, result, result, count);
        return Ret(ctx, ERROR_IO_PENDING);
    }
    if (returned)
        St32(returned, result == ERROR_SUCCESS ? count : 0);
    Ret(ctx, result);
}

// ---------------------------------------------------------------------------
// Profile settings
// ---------------------------------------------------------------------------
namespace
{
    // Dashboard defaults for the int32 settings titles commonly read.
    struct DefaultSetting { uint32_t id; uint32_t value; };
    constexpr DefaultSetting kDefaults[] = {
        { 0x10040002, 0 },      // XPROFILE_GAMER_YAXIS_INVERSION
        { 0x10040003, 3 },      // XPROFILE_OPTION_CONTROLLER_VIBRATION
        { 0x10040004, 0 },      // XPROFILE_GAMERCARD_ZONE
        { 0x10040005, 0 },      // XPROFILE_GAMERCARD_REGION
        { 0x10040006, 0xFA },   // XPROFILE_GAMERCARD_CRED
        { 0x1004000C, 0 },      // XPROFILE_OPTION_VOICE_MUTED
        { 0x1004000D, 0 },      // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
        { 0x1004000E, 0x64 },   // XPROFILE_OPTION_VOICE_VOLUME
        { 0x10040012, 1 },      // XPROFILE_GAMERCARD_TITLES_PLAYED
        { 0x10040013, 0 },      // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
        { 0x10040015, 0 },      // XPROFILE_GAMER_DIFFICULTY
        { 0x10040018, 0 },      // XPROFILE_GAMER_CONTROL_SENSITIVITY
        { 0x1004001D, 0xFFFF0000u }, // XPROFILE_GAMER_PREFERRED_COLOR_FIRST
        { 0x1004001E, 0xFF00FF00u }, // XPROFILE_GAMER_PREFERRED_COLOR_SECOND
        { 0x10040022, 1 },      // XPROFILE_GAMER_ACTION_AUTO_AIM
        { 0x10040023, 0 },      // XPROFILE_GAMER_ACTION_AUTO_CENTER
        { 0x10040024, 0 },      // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
        { 0x10040026, 0 },      // XPROFILE_GAMER_RACE_TRANSMISSION
        { 0x10040027, 0 },      // XPROFILE_GAMER_RACE_CAMERA_LOCATION
        { 0x10040028, 0 },      // XPROFILE_GAMER_RACE_BRAKE_CONTROL
        { 0x10040029, 0 },      // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
        { 0x10040038, 0 },      // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
        { 0x10040039, 0 },      // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
    };
}

// XamUserReadProfileSettings(title_id, user, xuid_count, xuids*, setting_count, setting_ids*,
//                            buffer_size*, buffer, overlapped)
// Buffer: { be32 count; be32 settings_ptr; } then X_USER_PROFILE_SETTING[count] (0x28 each):
// { be32 from; pad; be32 user_index / be64 xuid; be32 setting_id; pad; data (0x10) }.
// Settings with no stored value come back with from = 0 (unset), which titles
// treat as "use the default"; title-specific blobs therefore start out empty.
GUEST_IMPORT(XamUserReadProfileSettings)
{
    uint32_t user = Arg(ctx, 1), xuidCount = Arg(ctx, 2), count = Arg(ctx, 4), ids = Arg(ctx, 5);
    uint32_t sizePtr = Arg(ctx, 6), buffer = Arg(ctx, 7), ov = Arg(ctx, 8);
    if (count < 1 || count > 32 || !sizePtr)
        return Ret(ctx, ERROR_INVALID_PARAMETER);
    uint32_t have = Ld32(sizePtr);
    if (have && !buffer)
        return Ret(ctx, ERROR_INVALID_PARAMETER);

    uint32_t needed = 8 + count * 0x28 * (xuidCount ? xuidCount : 1);
    if (!buffer || have < needed)
    {
        if (!have)
            St32(sizePtr, needed);
        return Ret(ctx, ERROR_INSUFFICIENT_BUFFER);
    }
    if (!xuidCount && user != 0)
        return Ret(ctx, Finish(ov, ERROR_NO_SUCH_USER));

    Zero(buffer, needed);
    St32(buffer, count);
    St32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = Ld32(ids + i * 4), s = buffer + 8 + i * 0x28;
        if (xuidCount)
            St64(s + 8, Ld64(Arg(ctx, 3)));
        else
            St32(s + 8, user);
        St32(s + 0x10, id);
        for (auto& d : kDefaults)
        {
            if (d.id == id)
            {
                St32(s + 0, 1);             // from: profile default
                St8(s + 0x18, uint8_t(id >> 28));
                St32(s + 0x20, d.value);
            }
        }
    }
    TraceCall("XamUserReadProfileSettings", ctx);
    Ret(ctx, Finish(ov, ERROR_SUCCESS));
}

// XamUserWriteProfileSettings(user, setting_count, settings*, overlapped): accepted, not stored yet.
GUEST_IMPORT(XamUserWriteProfileSettings)
{
    TraceCall("XamUserWriteProfileSettings", ctx, "not persisted yet");
    Ret(ctx, Finish(Arg(ctx, 3), ERROR_SUCCESS));
}

// ---------------------------------------------------------------------------
// Other system UI
// ---------------------------------------------------------------------------

// XamShowSigninUI(panes, flags): the profile is always signed in.
GUEST_IMPORT(XamShowSigninUI)
{
    Log("XamShowSigninUI: player 1 is already signed in");
    BroadcastNotification(XN_SYS_SIGNINCHANGED, 1);
    BroadcastNotification(XN_SYS_UI, 0);
    Ret(ctx, ERROR_SUCCESS);
}

// XamShowMessageBoxUIEx(user, title, text, button_count, buttons**, focus_button, flags,
//                       result*, overlapped, ...): logs the text, picks the focused button.
GUEST_IMPORT(XamShowMessageBoxUIEx)
{
    std::string title = Arg(ctx, 1) ? Narrow(ReadWideCString(Arg(ctx, 1), 256)) : "";
    std::string text = Arg(ctx, 2) ? Narrow(ReadWideCString(Arg(ctx, 2), 1024)) : "";
    Log("Message box \"%s\": %s", title.c_str(), text.c_str());
    uint32_t focus = Arg(ctx, 5), resultPtr = Arg(ctx, 7);
    Ret(ctx, RunDialog("XamShowMessageBoxUIEx", Arg(ctx, 8), [focus, resultPtr] {
        if (resultPtr)
            St32(resultPtr, focus);
        return ERROR_SUCCESS;
    }));
}

// Guide screens the title can open: there is no guide, report them as closed again.
static void NoGuide(PPCContext& ctx, const char* name)
{
    Log("%s: no guide UI; ignored", name);
    BroadcastNotification(XN_SYS_UI, 0);
    Ret(ctx, ERROR_SUCCESS);
}
GUEST_IMPORT(XamShowAchievementsUI) { NoGuide(ctx, "XamShowAchievementsUI"); }
GUEST_IMPORT(XamShowFriendsUI) { NoGuide(ctx, "XamShowFriendsUI"); }
GUEST_IMPORT(XamShowMarketplaceUI) { NoGuide(ctx, "XamShowMarketplaceUI"); }
