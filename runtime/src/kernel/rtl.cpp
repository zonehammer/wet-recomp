// Rtl* runtime library, debug output, time, and helper implementations.
#include "kernel.h"
#include "module.h"
#include "os.h"
#include "printf.h"
#include "threads.h"

#include <cctype>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace wet
{
    // ---- String helpers ----
    std::string ReadCString(uint32_t addr, size_t maxLen)
    {
        std::string s;
        if (!addr)
            return s;
        for (size_t i = 0; i < maxLen; i++)
        {
            char c = char(Ld8(addr + uint32_t(i)));
            if (!c) break;
            s.push_back(c);
        }
        return s;
    }

    std::u16string ReadWideCString(uint32_t addr, size_t maxLen)
    {
        std::u16string s;
        if (!addr)
            return s;
        for (size_t i = 0; i < maxLen; i++)
        {
            char16_t c = char16_t(Ld16(addr + uint32_t(i * 2)));
            if (!c) break;
            s.push_back(c);
        }
        return s;
    }

    std::string ReadAnsiString(uint32_t p)
    {
        if (!p)
            return {};
        uint16_t len = Ld16(p);
        uint32_t buf = Ld32(p + 4);
        std::string s(len, '\0');
        for (uint16_t i = 0; i < len; i++)
            s[i] = char(Ld8(buf + i));
        return s;
    }

    void WriteCString(uint32_t addr, const std::string& s)
    {
        std::memcpy(HostPtr(addr), s.c_str(), s.size() + 1);
    }

    void WriteWideCString(uint32_t addr, const std::u16string& s)
    {
        for (size_t i = 0; i < s.size(); i++)
            St16(addr + uint32_t(i * 2), uint16_t(s[i]));
        St16(addr + uint32_t(s.size() * 2), 0);
    }

    std::string Narrow(const std::u16string& s)
    {
        std::string r;
        for (char16_t c : s)
            r.push_back(c < 0x80 ? char(c) : '?');
        return r;
    }

    std::u16string Widen(const std::string& s)
    {
        std::u16string r;
        for (unsigned char c : s)
            r.push_back(char16_t(c));
        return r;
    }

    // ---- Call tracing ----
    static std::mutex s_traceLock;
    static std::unordered_map<const char*, uint32_t> s_callCounts;

    static bool ShouldLog(const char* name, uint32_t* countOut)
    {
        std::lock_guard lock(s_traceLock);
        uint32_t n = ++s_callCounts[name];
        *countOut = n;
        // First 8 calls, then every power of two so a hot stub stays visible.
        return n <= 8 || (n & (n - 1)) == 0;
    }

    void StubCall(const char* name, PPCContext& ctx)
    {
        SetCurrentImport(name);
        uint32_t n;
        if (ShouldLog(name, &n))
            Log("STUB %s(%08X, %08X, %08X, %08X, %08X) #%u lr=%08X",
                name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, n, uint32_t(ctx.lr));
        ctx.r3.u64 = 0;
    }

    void TraceCall(const char* name, PPCContext& ctx, const char* note)
    {
        SetCurrentImport(name);
        uint32_t n;
        if (ShouldLog(name, &n))
            Log("%s(%08X, %08X, %08X, %08X) #%u%s%s", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, n,
                note ? " - " : "", note ? note : "");
    }
}

using namespace wet;

// ---------------------------------------------------------------------------
// Debug / fatal
// ---------------------------------------------------------------------------
GUEST_IMPORT(DbgPrint)
{
    GuestArgs args(ctx, 1);
    std::string s = FormatGuestString(ReadCString(Arg(ctx, 0)), args);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    Log("[game] %s", s.c_str());
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(DbgBreakPoint)
{
    Log("DbgBreakPoint called (lr=%08X) - continuing", uint32_t(ctx.lr));
}

GUEST_IMPORT(KeBugCheck)
{
    LogFatal("KeBugCheck(0x%08X)", Arg(ctx, 0));
    LogCrashContext();
    os::ExitProcess(1);
}

GUEST_IMPORT(KeBugCheckEx)
{
    LogFatal("KeBugCheckEx(0x%08X, %08X, %08X, %08X, %08X)", Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3), Arg(ctx, 4));
    LogCrashContext();
    os::ExitProcess(1);
}

GUEST_IMPORT(HalReturnToFirmware)
{
    Log("HalReturnToFirmware(%u): title requested shutdown", Arg(ctx, 0));
    os::ExitProcess(0);
}

// RtlRaiseException(EXCEPTION_RECORD*)
GUEST_IMPORT(RtlRaiseException)
{
    uint32_t rec = Arg(ctx, 0);
    uint32_t code = Ld32(rec);
    if (code == 0x406D1388)
    {
        // MSVC "SetThreadName": THREADNAME_INFO in ExceptionInformation[] at +0x14:
        // { dwType = 0x1000, szName, dwThreadID, dwFlags }
        uint32_t namePtr = Ld32(rec + 0x18), threadId = Ld32(rec + 0x1C);
        std::string name = ReadCString(namePtr, 64);
        SetThreadName(threadId, name);
        Log("Thread %u named \"%s\"", threadId == 0xFFFFFFFF ? (CurrentThread() ? CurrentThread()->id : 0) : threadId,
            name.c_str());
        return;
    }
    LogFatal("RtlRaiseException(code 0x%08X) - structured exceptions are not supported yet", code);
    LogCrashContext();
    os::ExitProcess(1);
}

GUEST_IMPORT(RtlUnwind)
{
    LogFatal("RtlUnwind called - structured exception unwinding is not supported yet");
    LogCrashContext();
    os::ExitProcess(1);
}

GUEST_IMPORT(RtlCaptureContext)
{
    TraceCall("RtlCaptureContext", ctx, "zero-filled");
    Zero(Arg(ctx, 0), 0x2D0);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// RtlInitAnsiString(ANSI_STRING*, const char*)
GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t dst = Arg(ctx, 0), src = Arg(ctx, 1);
    uint16_t len = uint16_t(src ? ReadCString(src, 0xFFFE).size() : 0);
    St16(dst, len);
    St16(dst + 2, src ? len + 1 : 0);
    St32(dst + 4, src);
}

// RtlInitUnicodeString(UNICODE_STRING*, const wchar_t*)
GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t dst = Arg(ctx, 0), src = Arg(ctx, 1);
    uint16_t len = uint16_t(src ? ReadWideCString(src, 0x7FFE).size() * 2 : 0);
    St16(dst, len);
    St16(dst + 2, src ? len + 2 : 0);
    St32(dst + 4, src);
}

GUEST_IMPORT(RtlFreeAnsiString)
{
    uint32_t s = Arg(ctx, 0);
    SystemFree(Ld32(s + 4));
    Zero(s, 8);
}

// RtlUnicodeStringToAnsiString(ANSI_STRING* dst, UNICODE_STRING* src, BOOLEAN allocate)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = Arg(ctx, 0), src = Arg(ctx, 1);
    uint16_t len = Ld16(src) / 2;
    uint32_t sbuf = Ld32(src + 4);
    uint32_t dbuf;
    if (Arg(ctx, 2))
    {
        dbuf = SystemAlloc(len + 1u);
        St16(dst + 2, len + 1);
        St32(dst + 4, dbuf);
    }
    else
    {
        dbuf = Ld32(dst + 4);
        if (Ld16(dst + 2) < len + 1u)
            return Ret(ctx, STATUS_BUFFER_TOO_SMALL);
    }
    for (uint16_t i = 0; i < len; i++)
    {
        uint16_t c = Ld16(sbuf + i * 2u);
        St8(dbuf + i, c < 0x80 ? uint8_t(c) : '?');
    }
    St8(dbuf + len, 0);
    St16(dst, len);
    Ret(ctx, STATUS_SUCCESS);
}

// RtlMultiByteToUnicodeN(wchar_t* dst, ULONG dst_bytes, ULONG* written, const char* src, ULONG src_bytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t dst = Arg(ctx, 0), dstBytes = Arg(ctx, 1), written = Arg(ctx, 2), src = Arg(ctx, 3), srcBytes = Arg(ctx, 4);
    uint32_t n = std::min(dstBytes / 2, srcBytes);
    for (uint32_t i = 0; i < n; i++)
        St16(dst + i * 2, Ld8(src + i));
    if (written)
        St32(written, n * 2);
    Ret(ctx, STATUS_SUCCESS);
}

// RtlUnicodeToMultiByteN(char* dst, ULONG dst_bytes, ULONG* written, const wchar_t* src, ULONG src_bytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t dst = Arg(ctx, 0), dstBytes = Arg(ctx, 1), written = Arg(ctx, 2), src = Arg(ctx, 3), srcBytes = Arg(ctx, 4);
    uint32_t n = std::min(dstBytes, srcBytes / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = Ld16(src + i * 2);
        St8(dst + i, c < 0x80 ? uint8_t(c) : '?');
    }
    if (written)
        St32(written, n);
    Ret(ctx, STATUS_SUCCESS);
}

// RtlCompareStringN(const char* a, int a_len, const char* b, int b_len, BOOLEAN case_insensitive)
GUEST_IMPORT(RtlCompareStringN)
{
    uint32_t a = Arg(ctx, 0), b = Arg(ctx, 2);
    int32_t la = int32_t(Arg(ctx, 1)), lb = int32_t(Arg(ctx, 3));
    bool ci = Arg(ctx, 4) != 0;
    if (la < 0) la = int32_t(ReadCString(a).size());
    if (lb < 0) lb = int32_t(ReadCString(b).size());
    int32_t n = std::min(la, lb);
    for (int32_t i = 0; i < n; i++)
    {
        int ca = Ld8(a + i), cb = Ld8(b + i);
        if (ci) { ca = toupper(ca); cb = toupper(cb); }
        if (ca != cb)
            return Ret(ctx, uint32_t(ca - cb));
    }
    Ret(ctx, uint32_t(la - lb));
}

GUEST_IMPORT(RtlUpcaseUnicodeChar)
{
    uint32_t c = Arg(ctx, 0) & 0xFFFF;
    Ret(ctx, (c >= 'a' && c <= 'z') ? c - 32 : c);
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

// RtlCompareMemoryUlong(source, length, pattern) -> bytes matching before first mismatch
GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = Arg(ctx, 0), len = Arg(ctx, 1) & ~3u, pattern = Arg(ctx, 2);
    uint32_t i = 0;
    while (i < len && Ld32(src + i) == pattern)
        i += 4;
    Ret(ctx, i);
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    uint32_t dst = Arg(ctx, 0), len = Arg(ctx, 1) & ~3u, pattern = Arg(ctx, 2);
    for (uint32_t i = 0; i < len; i += 4)
        St32(dst + i, pattern);
}

// ---------------------------------------------------------------------------
// Status codes
// ---------------------------------------------------------------------------
GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = Arg(ctx, 0);
    uint32_t r;
    switch (s)
    {
    case STATUS_SUCCESS:               r = 0; break;
    case STATUS_PENDING:               r = ERROR_IO_PENDING; break;
    case STATUS_TIMEOUT:               r = 258; break;   // WAIT_TIMEOUT
    case STATUS_NO_MORE_FILES:         r = ERROR_NO_MORE_FILES; break;
    case STATUS_INVALID_HANDLE:        r = 6; break;
    case STATUS_INVALID_PARAMETER:     r = ERROR_INVALID_PARAMETER; break;
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND: r = 2; break;     // ERROR_FILE_NOT_FOUND
    case STATUS_OBJECT_PATH_NOT_FOUND: r = 3; break;     // ERROR_PATH_NOT_FOUND
    case STATUS_END_OF_FILE:           r = 38; break;    // ERROR_HANDLE_EOF
    case STATUS_NO_MEMORY:             r = 8; break;
    case STATUS_ACCESS_DENIED:         r = ERROR_ACCESS_DENIED; break;
    case STATUS_BUFFER_TOO_SMALL:      r = ERROR_INSUFFICIENT_BUFFER; break;
    case STATUS_OBJECT_NAME_COLLISION: r = 183; break;   // ERROR_ALREADY_EXISTS
    case STATUS_NOT_IMPLEMENTED:       r = 120; break;
    default:
        r = (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317; // ERROR_MR_MID_NOT_FOUND
        break;
    }
    Ret(ctx, r);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
GUEST_IMPORT(KeQueryPerformanceFrequency) { Ret(ctx, 50000000); }

GUEST_IMPORT(KeQuerySystemTime)
{
    if (Arg(ctx, 0))
        St64(Arg(ctx, 0), CurrentFileTime());
}

// TIME_FIELDS: be16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
GUEST_IMPORT(RtlTimeToTimeFields)
{
    uint64_t ft = Ld64(Arg(ctx, 0));
    uint32_t tf = Arg(ctx, 1);
    int64_t unixSec = int64_t(ft / 10000000) - 11644473600ll;
    std::tm tmv{};
    time_t tt = time_t(unixSec);
#ifdef _WIN32
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    St16(tf + 0, uint16_t(tmv.tm_year + 1900));
    St16(tf + 2, uint16_t(tmv.tm_mon + 1));
    St16(tf + 4, uint16_t(tmv.tm_mday));
    St16(tf + 6, uint16_t(tmv.tm_hour));
    St16(tf + 8, uint16_t(tmv.tm_min));
    St16(tf + 10, uint16_t(tmv.tm_sec));
    St16(tf + 12, uint16_t((ft / 10000) % 1000));
    St16(tf + 14, uint16_t(tmv.tm_wday));
}

// RtlTimeFieldsToTime(TIME_FIELDS*, LARGE_INTEGER*) -> BOOLEAN
GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t tf = Arg(ctx, 0);
    std::tm tmv{};
    tmv.tm_year = Ld16(tf + 0) - 1900;
    tmv.tm_mon = Ld16(tf + 2) - 1;
    tmv.tm_mday = Ld16(tf + 4);
    tmv.tm_hour = Ld16(tf + 6);
    tmv.tm_min = Ld16(tf + 8);
    tmv.tm_sec = Ld16(tf + 10);
#ifdef _WIN32
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    if (t == time_t(-1))
        return Ret(ctx, 0);
    uint64_t ft = (uint64_t(t) + 11644473600ull) * 10000000ull + uint64_t(Ld16(tf + 12)) * 10000ull;
    St64(Arg(ctx, 1), ft);
    Ret(ctx, 1);
}

// ---------------------------------------------------------------------------
// XEX header access
// ---------------------------------------------------------------------------

// RtlImageXexHeaderField(xex_header_base, key)
GUEST_IMPORT(RtlImageXexHeaderField)
{
    uint32_t hdr = Arg(ctx, 0) ? Arg(ctx, 0) : g_module.guestHeader;
    Ret(ctx, XexHeaderField(hdr, Arg(ctx, 1)));
}

// ---------------------------------------------------------------------------
// No-ops
// ---------------------------------------------------------------------------
GUEST_IMPORT(KeEnableFpuExceptions) {}
GUEST_IMPORT(ExRegisterTitleTerminateNotification) {}
GUEST_IMPORT(FscSetCacheElementCount) { Ret(ctx, STATUS_SUCCESS); }
GUEST_IMPORT(KiApcNormalRoutineNop) {}
