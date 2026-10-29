// printf-family formatting over guest arguments.
//
// Xbox 360 varargs: every argument (including doubles) occupies one 64-bit slot,
// first in r3..r10, then in the caller's parameter area (r1 + 0x50 + 8*k).
// A va_list is a pointer to such 8-byte slots.
#pragma once
#include "kernel.h"

#include <string>

namespace wet
{
    class GuestArgs
    {
    public:
        // Varargs starting at argument index `first` of the current call.
        GuestArgs(PPCContext& ctx, int first) : m_ctx(&ctx), m_index(first) {}
        // A guest va_list.
        static GuestArgs FromVaList(uint32_t vaList) { GuestArgs a; a.m_vaList = vaList; return a; }

        uint64_t Next64()
        {
            if (m_ctx)
                return Arg64(*m_ctx, m_index++);
            uint64_t v = Ld64(m_vaList);
            m_vaList += 8;
            return v;
        }
        uint32_t Next32() { return uint32_t(Next64()); }
        double NextDouble()
        {
            uint64_t bits = Next64();
            double d;
            std::memcpy(&d, &bits, 8);
            return d;
        }

    private:
        GuestArgs() = default;
        PPCContext* m_ctx = nullptr;
        int m_index = 0;
        uint32_t m_vaList = 0;
    };

    std::string FormatGuestString(const std::string& fmt, GuestArgs& args);
    std::u16string FormatGuestWide(const std::u16string& fmt, GuestArgs& args);
}
