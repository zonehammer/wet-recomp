// Kernel CRT printf family: sprintf, _snprintf, _vsnprintf, swprintf, _snwprintf,
// vswprintf, _vscwprintf (plus the engine used by DbgPrint).
#include "printf.h"

#include <cstdio>

namespace wet
{
    namespace
    {
        template <typename Ch>
        std::basic_string<Ch> FromNarrow(const std::string& s)
        {
            return std::basic_string<Ch>(s.begin(), s.end());
        }

        template <typename Ch>
        std::basic_string<Ch> Format(const std::basic_string<Ch>& fmt, GuestArgs& args)
        {
            constexpr bool kWide = sizeof(Ch) == 2;
            std::basic_string<Ch> out;
            size_t i = 0;
            while (i < fmt.size())
            {
                Ch c = fmt[i++];
                if (c != '%')
                {
                    out.push_back(c);
                    continue;
                }
                if (i < fmt.size() && fmt[i] == '%')
                {
                    out.push_back('%');
                    i++;
                    continue;
                }

                // flags
                std::string spec = "%";
                bool leftAlign = false;
                while (i < fmt.size() && std::string("-+ #0").find(char(fmt[i])) != std::string::npos)
                {
                    if (fmt[i] == '-') leftAlign = true;
                    spec.push_back(char(fmt[i++]));
                }
                // width
                int width = -1;
                if (i < fmt.size() && fmt[i] == '*')
                {
                    width = int32_t(args.Next32());
                    if (width < 0) { leftAlign = true; spec.push_back('-'); width = -width; }
                    spec += std::to_string(width);
                    i++;
                }
                else
                {
                    while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
                    {
                        width = (width < 0 ? 0 : width * 10) + int(fmt[i] - '0');
                        spec.push_back(char(fmt[i++]));
                    }
                }
                // precision
                int precision = -1;
                if (i < fmt.size() && fmt[i] == '.')
                {
                    spec.push_back('.');
                    i++;
                    precision = 0;
                    if (i < fmt.size() && fmt[i] == '*')
                    {
                        precision = int32_t(args.Next32());
                        spec += std::to_string(precision < 0 ? 0 : precision);
                        i++;
                    }
                    else
                    {
                        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
                        {
                            precision = precision * 10 + int(fmt[i] - '0');
                            spec.push_back(char(fmt[i++]));
                        }
                    }
                }
                // length
                enum { LenDefault, LenShort, LenChar, LenLong, LenLongLong, LenWide } len = LenDefault;
                if (i < fmt.size())
                {
                    if (fmt[i] == 'h') { len = LenShort; i++; if (i < fmt.size() && fmt[i] == 'h') { len = LenChar; i++; } }
                    else if (fmt[i] == 'l') { len = LenLong; i++; if (i < fmt.size() && fmt[i] == 'l') { len = LenLongLong; i++; } }
                    else if (fmt[i] == 'w') { len = LenWide; i++; }
                    else if (fmt[i] == 'L' || fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't') { i++; }
                    else if (fmt[i] == 'I')
                    {
                        i++;
                        if (i + 1 < fmt.size() && fmt[i] == '6' && fmt[i + 1] == '4') { len = LenLongLong; i += 2; }
                        else if (i + 1 < fmt.size() && fmt[i] == '3' && fmt[i + 1] == '2') { i += 2; }
                    }
                }
                if (i >= fmt.size())
                    break;
                char conv = char(fmt[i++]);

                auto pad = [&](std::basic_string<Ch> s) {
                    if (width > 0 && int(s.size()) < width)
                    {
                        std::basic_string<Ch> p(size_t(width) - s.size(), Ch(' '));
                        s = leftAlign ? s + p : p + s;
                    }
                    out += s;
                };

                char buf[512];
                switch (conv)
                {
                case 'd': case 'i':
                {
                    int64_t v = len == LenLongLong ? int64_t(args.Next64()) : int64_t(int32_t(args.Next32()));
                    if (len == LenShort) v = int16_t(v);
                    if (len == LenChar) v = int8_t(v);
                    std::snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)v);
                    out += FromNarrow<Ch>(buf);
                    break;
                }
                case 'u': case 'o': case 'x': case 'X':
                {
                    uint64_t v = len == LenLongLong ? args.Next64() : uint64_t(args.Next32());
                    if (len == LenShort) v = uint16_t(v);
                    if (len == LenChar) v = uint8_t(v);
                    std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)v);
                    out += FromNarrow<Ch>(buf);
                    break;
                }
                case 'p':
                    std::snprintf(buf, sizeof(buf), "%08X", args.Next32());
                    out += FromNarrow<Ch>(buf);
                    break;
                case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                    std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), args.NextDouble());
                    out += FromNarrow<Ch>(buf);
                    break;
                case 'c': case 'C':
                {
                    uint32_t v = args.Next32();
                    pad(std::basic_string<Ch>(1, Ch(kWide ? (v & 0xFFFF) : (v & 0xFF))));
                    break;
                }
                case 's': case 'S':
                {
                    // MS rules: in narrow printf %s is narrow and %S wide; in wide printf the
                    // reverse. h forces narrow, l/w force wide.
                    bool wideArg = kWide ? (conv == 's') : (conv == 'S');
                    if (len == LenShort) wideArg = false;
                    if (len == LenLong || len == LenWide) wideArg = true;
                    uint32_t p = args.Next32();
                    std::basic_string<Ch> s;
                    if (!p)
                        s = FromNarrow<Ch>("(null)");
                    else if (wideArg)
                    {
                        std::u16string w = ReadWideCString(p, precision >= 0 ? size_t(precision) : 1 << 20);
                        if constexpr (kWide) s = w; else s = Narrow(w);
                    }
                    else
                    {
                        s = FromNarrow<Ch>(ReadCString(p, precision >= 0 ? size_t(precision) : 1 << 20));
                    }
                    pad(s);
                    break;
                }
                case 'n':
                    St32(args.Next32(), uint32_t(out.size()));
                    break;
                default:
                    out.push_back('%');
                    out.push_back(Ch(conv));
                    break;
                }
            }
            return out;
        }

        // Writes a narrow result. limit < 0: unlimited (always terminated).
        // _snprintf semantics otherwise: no terminator and -1 when truncated.
        int32_t PutNarrow(uint32_t buffer, const std::string& s, int64_t limit)
        {
            if (limit < 0)
            {
                WriteCString(buffer, s);
                return int32_t(s.size());
            }
            size_t n = std::min<size_t>(s.size(), size_t(limit));
            std::memcpy(HostPtr(buffer), s.data(), n);
            if (n < size_t(limit))
                St8(buffer + uint32_t(n), 0);
            return s.size() > size_t(limit) ? -1 : int32_t(s.size());
        }

        int32_t PutWide(uint32_t buffer, const std::u16string& s, int64_t limit)
        {
            size_t n = limit < 0 ? s.size() : std::min<size_t>(s.size(), size_t(limit));
            for (size_t i = 0; i < n; i++)
                St16(buffer + uint32_t(i * 2), uint16_t(s[i]));
            if (limit < 0 || n < size_t(limit))
                St16(buffer + uint32_t(n * 2), 0);
            return (limit >= 0 && s.size() > size_t(limit)) ? -1 : int32_t(s.size());
        }
    }

    std::string FormatGuestString(const std::string& fmt, GuestArgs& args) { return Format<char>(fmt, args); }
    std::u16string FormatGuestWide(const std::u16string& fmt, GuestArgs& args) { return Format<char16_t>(fmt, args); }
}

using namespace wet;

// sprintf(buffer, format, ...)
GUEST_IMPORT(sprintf)
{
    GuestArgs a(ctx, 2);
    Ret(ctx, uint32_t(PutNarrow(Arg(ctx, 0), FormatGuestString(ReadCString(Arg(ctx, 1)), a), -1)));
}

// _snprintf(buffer, count, format, ...)
GUEST_IMPORT(_snprintf)
{
    GuestArgs a(ctx, 3);
    Ret(ctx, uint32_t(PutNarrow(Arg(ctx, 0), FormatGuestString(ReadCString(Arg(ctx, 2)), a), Arg(ctx, 1))));
}

// _vsnprintf(buffer, count, format, va_list)
GUEST_IMPORT(_vsnprintf)
{
    GuestArgs a = GuestArgs::FromVaList(Arg(ctx, 3));
    Ret(ctx, uint32_t(PutNarrow(Arg(ctx, 0), FormatGuestString(ReadCString(Arg(ctx, 2)), a), Arg(ctx, 1))));
}

// swprintf(buffer, format, ...)   (pre-C99 MS signature, no count)
GUEST_IMPORT(swprintf)
{
    GuestArgs a(ctx, 2);
    Ret(ctx, uint32_t(PutWide(Arg(ctx, 0), FormatGuestWide(ReadWideCString(Arg(ctx, 1)), a), -1)));
}

// _snwprintf(buffer, count, format, ...)
GUEST_IMPORT(_snwprintf)
{
    GuestArgs a(ctx, 3);
    Ret(ctx, uint32_t(PutWide(Arg(ctx, 0), FormatGuestWide(ReadWideCString(Arg(ctx, 2)), a), Arg(ctx, 1))));
}

// vswprintf(buffer, format, va_list)
GUEST_IMPORT(vswprintf)
{
    GuestArgs a = GuestArgs::FromVaList(Arg(ctx, 2));
    Ret(ctx, uint32_t(PutWide(Arg(ctx, 0), FormatGuestWide(ReadWideCString(Arg(ctx, 1)), a), -1)));
}

// _vscwprintf(format, va_list) -> length
GUEST_IMPORT(_vscwprintf)
{
    GuestArgs a = GuestArgs::FromVaList(Arg(ctx, 1));
    Ret(ctx, uint32_t(FormatGuestWide(ReadWideCString(Arg(ctx, 0)), a).size()));
}
