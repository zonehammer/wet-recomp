// Force-included before every generated file (see runtime/CMakeLists.txt).
// Overrides hook macros that XenonUtils/ppc_context.h leaves #ifndef-guarded.
#pragma once
#include <cstdint>

struct PPCContext;

// Xbox 360 timebase: 50 MHz. Replaces the recompiler's default (host __rdtsc,
// which ticks at GHz rates and would make every game timer run far too fast).
uint64_t WetQueryTimebase();
#define PPC_QUERY_TIMEBASE() WetQueryTimebase()

// Indirect calls (bctrl / function pointers / vtables): report misses instead of
// jumping through a null table entry.
[[noreturn]] void WetIndirectCallMiss(PPCContext& ctx, uint8_t* base, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x)                                       \
    do {                                                                \
        uint32_t wetTarget__ = uint32_t(x);                             \
        if (wetTarget__ < PPC_CODE_BASE || wetTarget__ >= PPC_CODE_BASE + PPC_CODE_SIZE) [[unlikely]] \
            WetIndirectCallMiss(ctx, base, wetTarget__);                \
        auto* wetFn__ = PPC_LOOKUP_FUNC(base, wetTarget__);             \
        if (!wetFn__) [[unlikely]]                                      \
            WetIndirectCallMiss(ctx, base, wetTarget__);                \
        wetFn__(ctx, base);                                             \
    } while (0)
