// Shared definitions for host implementations of Xbox 360 kernel/XAM imports.
//
// Writing an import:
//     GUEST_IMPORT(KeQueryPerformanceFrequency) { Ret(ctx, 50000000); }
// Arguments are Arg(ctx, 0..N): r3..r10, then the caller's stack parameter area.
// Anything not implemented under src/kernel is auto-stubbed (tools/gen_import_stubs.py).
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "log.h"
#include "memory.h"

#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)  // C++ linkage, matching PPC_EXTERN_FUNC in the generated code

namespace wet
{
    // ---- NTSTATUS / Win32 error values used by the kernel ----
    constexpr uint32_t STATUS_SUCCESS                 = 0x00000000;
    constexpr uint32_t STATUS_ABANDONED_WAIT_0        = 0x00000080;
    constexpr uint32_t STATUS_USER_APC                = 0x000000C0;
    constexpr uint32_t STATUS_ALERTED                 = 0x00000101;
    constexpr uint32_t STATUS_TIMEOUT                 = 0x00000102;
    constexpr uint32_t STATUS_PENDING                 = 0x00000103;
    constexpr uint32_t STATUS_NO_MORE_FILES           = 0x80000006;
    constexpr uint32_t STATUS_UNSUCCESSFUL            = 0xC0000001;
    constexpr uint32_t STATUS_NOT_IMPLEMENTED         = 0xC0000002;
    constexpr uint32_t STATUS_INVALID_INFO_CLASS      = 0xC0000003;
    constexpr uint32_t STATUS_INFO_LENGTH_MISMATCH    = 0xC0000004;
    constexpr uint32_t STATUS_INVALID_HANDLE          = 0xC0000008;
    constexpr uint32_t STATUS_INVALID_PARAMETER       = 0xC000000D;
    constexpr uint32_t STATUS_NO_SUCH_FILE            = 0xC000000F;
    constexpr uint32_t STATUS_END_OF_FILE             = 0xC0000011;
    constexpr uint32_t STATUS_NO_MEMORY               = 0xC0000017;
    constexpr uint32_t STATUS_ACCESS_DENIED           = 0xC0000022;
    constexpr uint32_t STATUS_BUFFER_TOO_SMALL        = 0xC0000023;
    constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH    = 0xC0000024;
    constexpr uint32_t STATUS_OBJECT_NAME_INVALID     = 0xC0000033;
    constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND   = 0xC0000034;
    constexpr uint32_t STATUS_OBJECT_NAME_COLLISION   = 0xC0000035;
    constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND   = 0xC000003A;
    constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED= 0xC0000047;
    constexpr uint32_t STATUS_MUTANT_NOT_OWNED        = 0xC0000046;
    constexpr uint32_t STATUS_FILE_IS_A_DIRECTORY     = 0xC00000BA;
    constexpr uint32_t STATUS_NOT_A_DIRECTORY         = 0xC0000103;
    constexpr uint32_t STATUS_PROCEDURE_NOT_FOUND     = 0xC000007A;

    constexpr uint32_t ERROR_SUCCESS                  = 0;
    constexpr uint32_t ERROR_ACCESS_DENIED            = 5;
    constexpr uint32_t ERROR_INVALID_PARAMETER        = 87;
    constexpr uint32_t ERROR_INSUFFICIENT_BUFFER      = 122;
    constexpr uint32_t ERROR_NO_MORE_FILES            = 18;
    constexpr uint32_t ERROR_IO_PENDING               = 997;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED     = 1167;
    constexpr uint32_t ERROR_NOT_FOUND                = 1168;
    constexpr uint32_t ERROR_NO_SUCH_USER             = 1317;
    constexpr uint32_t ERROR_FUNCTION_FAILED          = 1627;
    constexpr uint32_t ERROR_EMPTY                    = 4306;

    // ---- Guest memory access (big-endian) ----
    inline uint8_t* HostPtr(uint32_t a) { return g_mem.base + a; }
    inline uint8_t  Ld8 (uint32_t a) { return *HostPtr(a); }
    inline uint16_t Ld16(uint32_t a) { uint16_t v; std::memcpy(&v, HostPtr(a), 2); return __builtin_bswap16(v); }
    inline uint32_t Ld32(uint32_t a) { uint32_t v; std::memcpy(&v, HostPtr(a), 4); return __builtin_bswap32(v); }
    inline uint64_t Ld64(uint32_t a) { uint64_t v; std::memcpy(&v, HostPtr(a), 8); return __builtin_bswap64(v); }
    inline void St8 (uint32_t a, uint8_t v)  { *HostPtr(a) = v; }
    inline void St16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(HostPtr(a), &v, 2); }
    inline void St32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(HostPtr(a), &v, 4); }
    inline void St64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(HostPtr(a), &v, 8); }
    inline void Zero(uint32_t a, uint32_t n) { std::memset(HostPtr(a), 0, n); }

    std::string ReadCString(uint32_t addr, size_t maxLen = 4096);
    std::u16string ReadWideCString(uint32_t addr, size_t maxLen = 4096);
    // ANSI_STRING { be16 Length; be16 MaximumLength; be32 Buffer; }
    std::string ReadAnsiString(uint32_t ansiStringPtr);
    void WriteCString(uint32_t addr, const std::string& s);
    void WriteWideCString(uint32_t addr, const std::u16string& s);
    std::string Narrow(const std::u16string& s);
    std::u16string Widen(const std::string& s);

    // ---- Arguments / return ----
    inline uint64_t ArgRegister(PPCContext& ctx, int i)
    {
        switch (i)
        {
        case 0: return ctx.r3.u64;
        case 1: return ctx.r4.u64;
        case 2: return ctx.r5.u64;
        case 3: return ctx.r6.u64;
        case 4: return ctx.r7.u64;
        case 5: return ctx.r8.u64;
        case 6: return ctx.r9.u64;
        default: return ctx.r10.u64;
        }
    }

    // 64-bit argument i. Arguments past the 8th live in the caller's parameter
    // save area: 8-byte slots starting at r1 + 0x50.
    inline uint64_t Arg64(PPCContext& ctx, int i)
    {
        return i < 8 ? ArgRegister(ctx, i) : Ld64(ctx.r1.u32 + 0x50 + (i - 8) * 8);
    }
    inline uint32_t Arg(PPCContext& ctx, int i) { return uint32_t(Arg64(ctx, i)); }
    inline void Ret(PPCContext& ctx, uint64_t v) { ctx.r3.u64 = v; }

    // Default behaviour of unimplemented imports: log (rate-limited) and return 0.
    void StubCall(const char* name, PPCContext& ctx);
    // Log the first few calls of an implemented-but-approximate import.
    void TraceCall(const char* name, PPCContext& ctx, const char* note = nullptr);

    // Name of the import the current thread is in (for crash reports).
    void SetCurrentImport(const char* name);

    // Calls a recompiled guest function by address on the current thread.
    void CallGuest(PPCContext& ctx, uint32_t address);

    // 100 ns FILETIME <-> Xbox timeouts
    uint64_t CurrentFileTime();
}
