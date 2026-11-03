// Rubi's outfit colour (outfit_colour in wet.ini): her clothes' textures are
// recoloured where the GPU emulation first reads them (the texture hook).
// They're DXT1/DXT5: each 4x4 block's colours are two end colours and 2-bit
// picks between them, so recolouring a block is recolouring its two colours -
// the same for every mip level, and no untiling. Her top shares its texture
// with her skin and tattoos, so only its red is recoloured there.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wet::outfit
{
    enum Colour { kOriginal, kRed, kBlue, kBlack, kWhite, kGreen, kPurple, kPink, kCount };
    inline const char* const kNames[kCount] = { "Original", "Red", "Blue", "Black", "White", "Green", "Purple", "Pink" };

    struct Hsv { float h, s, v; };

    inline Hsv ToHsv(float r, float g, float b)
    {
        float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
        float h = 0;
        if (d > 1e-6f)
        {
            if (mx == r) h = std::fmod((g - b) / d, 6.0f);
            else if (mx == g) h = (b - r) / d + 2;
            else h = (r - g) / d + 4;
            h *= 60;
            if (h < 0) h += 360;
        }
        return { h, mx > 0 ? d / mx : 0, mx };
    }

    inline void FromHsv(Hsv c, float& r, float& g, float& b)
    {
        float C = c.v * c.s, X = C * (1 - std::fabs(std::fmod(c.h / 60, 2.0f) - 1)), m = c.v - C;
        float rr = 0, gg = 0, bb = 0;
        int seg = int(c.h / 60) % 6;
        switch (seg)
        {
        case 0: rr = C; gg = X; break;
        case 1: rr = X; gg = C; break;
        case 2: gg = C; bb = X; break;
        case 3: gg = X; bb = C; break;
        case 4: rr = X; bb = C; break;
        default: rr = C; bb = X; break;
        }
        r = rr + m; g = gg + m; b = bb + m;
    }

    inline void From565(uint16_t c, float& r, float& g, float& b)
    {
        r = ((c >> 11) & 31) / 31.0f;
        g = ((c >> 5) & 63) / 63.0f;
        b = (c & 31) / 31.0f;
    }

    inline uint16_t To565(float r, float g, float b)
    {
        auto q = [](float v, int max) { return int(std::clamp(v, 0.0f, 1.0f) * max + 0.5f); };
        return uint16_t(q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31));
    }

    // Skin (her hands, feet, face) stays: warm, fairly saturated and light.
    // Lips and other strong reds stay too.
    inline bool Skin(float r, float g, float b)
    {
        Hsv c = ToHsv(r, g, b);
        if ((c.h <= 15 || c.h >= 340) && c.s > 0.4f && c.v > 0.25f)
            return true;
        return c.h >= 5 && c.h <= 34 && c.s >= 0.25f && c.s <= 0.7f && c.v >= 0.45f;
    }

    inline void Recolour(float& r, float& g, float& b, Colour to)
    {
        Hsv c = ToHsv(r, g, b);
        switch (to)
        {
        case kBlack:
            c.v *= 0.32f;
            c.s *= 0.25f;
            break;
        case kWhite:
            c.v = 0.5f + 0.5f * c.v;
            c.s *= 0.12f;
            break;
        default:
        {
            static const float kHue[kCount] = { 0, 2, 218, 0, 0, 115, 278, 330 };
            c.h = kHue[to];
            // Her trousers are dull brown leather: give the new colour some
            // strength (not neon).
            c.s = std::clamp(0.15f + c.s, 0.0f, 0.7f);
            c.v = std::min(1.0f, c.v * 1.05f);
            break;
        }
        }
        FromHsv(c, r, g, b);
    }

    // Which blocks to recolour.
    enum class Pick { kNotSkin, kRed, kAll };

    inline bool Picked(Pick pick, float r, float g, float b)
    {
        switch (pick)
        {
        case Pick::kAll:
            return true;
        case Pick::kRed:
        {
            // Her top: a deep red (her skin is orange-pink, h about 20).
            Hsv c = ToHsv(r, g, b);
            return (c.h >= 335 || c.h <= 10) && c.s >= 0.4f;
        }
        default:
            return !Skin(r, g, b);
        }
    }

    // Recolours one DXT colour block (8 bytes, little-endian colours) in place.
    // `alwaysFour`: DXT3/5 colour blocks, always 4 colours whatever their order.
    inline void RecolourColourBlock(uint8_t* p, Colour to, Pick pick, bool alwaysFour)
    {
        uint16_t c0 = uint16_t(p[0] | p[1] << 8), c1 = uint16_t(p[2] | p[3] << 8);
        float r0, g0, b0, r1, g1, b1;
        From565(c0, r0, g0, b0);
        From565(c1, r1, g1, b1);
        if (!Picked(pick, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2))
            return;
        Recolour(r0, g0, b0, to);
        Recolour(r1, g1, b1, to);
        uint16_t n0 = To565(r0, g0, b0), n1 = To565(r1, g1, b1);
        uint32_t bits = uint32_t(p[4] | p[5] << 8 | p[6] << 16 | uint32_t(p[7]) << 24);
        if (!alwaysFour)
        {
            bool fourColour = c0 > c1;
            // Keep the block's mode (4 colours, or 3 + transparent): swap the
            // colours and their indices if their order flipped.
            if (fourColour ? n0 <= n1 : n0 > n1)
            {
                if (n0 == n1)
                {
                    // Both the same colour: a 4-colour block that's all colour 0.
                    if (n0 == 0)
                        n0 = 1;
                    n1 = uint16_t(n0 - 1);
                    bits = 0;
                }
                else
                {
                    std::swap(n0, n1);
                    uint32_t out = 0;
                    for (int k = 0; k < 16; k++)
                    {
                        uint32_t idx = (bits >> (2 * k)) & 3;
                        if (fourColour)
                            idx = idx ^ 1;               // 0<->1, 2<->3
                        else if (idx < 2)
                            idx ^= 1;                    // 0<->1; 2 (mid) and 3 (transparent) stay
                        out |= idx << (2 * k);
                    }
                    bits = out;
                }
            }
        }
        p[0] = uint8_t(n0); p[1] = uint8_t(n0 >> 8);
        p[2] = uint8_t(n1); p[3] = uint8_t(n1 >> 8);
        p[4] = uint8_t(bits); p[5] = uint8_t(bits >> 8); p[6] = uint8_t(bits >> 16); p[7] = uint8_t(bits >> 24);
    }

    // Recolours DXT1 (8-byte blocks) or DXT5 (16-byte blocks: alpha, then colour) data.
    inline void RecolourDxt(uint8_t* blocks, size_t bytes, bool dxt5, Colour to, Pick pick)
    {
        size_t stride = dxt5 ? 16 : 8;
        for (size_t i = 0; i + stride <= bytes; i += stride)
            RecolourColourBlock(blocks + i + (dxt5 ? 8 : 0), to, pick, dxt5);
    }

    inline void RecolourDxt1(uint8_t* blocks, size_t count, Colour to)
    {
        RecolourDxt(blocks, count * 8, false, to, Pick::kNotSkin);
    }
}
