// Force-included into every Xenia translation unit (cmake/xenia.cmake).
//
// libc++ 19+ (llvm-mingw) no longer provides std::char_traits for arbitrary
// character types, but the fmt 6 bundled with Xenia instantiates it for its
// pre-C++20 char8_type. Provide that one specialisation.
#pragma once
#if defined(__cplusplus) && !defined(__cpp_char8_t)
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <ios>
#include <string>
#if defined(_LIBCPP_VERSION)
namespace fmt { inline namespace v6 { namespace internal { enum char8_type : unsigned char; } } }
template <>
struct std::char_traits<fmt::internal::char8_type>
{
    using char_type = fmt::internal::char8_type;
    using int_type = unsigned int;
    using off_type = std::streamoff;
    using pos_type = std::streampos;
    using state_type = std::mbstate_t;
    static void assign(char_type& r, const char_type& a) noexcept { r = a; }
    static constexpr bool eq(char_type a, char_type b) noexcept { return a == b; }
    static constexpr bool lt(char_type a, char_type b) noexcept { return (unsigned char)a < (unsigned char)b; }
    static int compare(const char_type* a, const char_type* b, std::size_t n) { return n ? std::memcmp(a, b, n) : 0; }
    static std::size_t length(const char_type* s) { std::size_t n = 0; while ((unsigned char)s[n]) n++; return n; }
    static const char_type* find(const char_type* s, std::size_t n, const char_type& c)
    {
        for (std::size_t i = 0; i < n; i++) if (s[i] == c) return s + i;
        return nullptr;
    }
    static char_type* move(char_type* d, const char_type* s, std::size_t n) { return n ? (char_type*)std::memmove(d, s, n) : d; }
    static char_type* copy(char_type* d, const char_type* s, std::size_t n) { return n ? (char_type*)std::memcpy(d, s, n) : d; }
    static char_type* assign(char_type* d, std::size_t n, char_type c) { if (n) std::memset(d, (unsigned char)c, n); return d; }
    static constexpr int_type not_eof(int_type c) noexcept { return c == eof() ? 0 : c; }
    static constexpr char_type to_char_type(int_type c) noexcept { return char_type(c); }
    static constexpr int_type to_int_type(char_type c) noexcept { return int_type((unsigned char)c); }
    static constexpr bool eq_int_type(int_type a, int_type b) noexcept { return a == b; }
    static constexpr int_type eof() noexcept { return int_type(-1); }
};
#endif
#endif

// MSVC accepts `_uuidof` as a synonym of `__uuidof`; MinGW doesn't.
#if defined(__MINGW32__) && !defined(_uuidof)
#define _uuidof __uuidof
#endif
