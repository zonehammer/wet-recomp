// PlayStation and keyboard / mouse button art, drawn over the game's own Xbox
// button textures (prompts.cpp). Plain functions with no runtime dependencies.
//
// The game's button icons are small DXT5 textures (button_A_b.dds, button_3.dds,
// ...): a flat disc (or shoulder-button shape) with the letter cut out of it as
// transparency, in several tints. The PlayStation version keeps each texture's
// shape, colour and tint - the cut-out is filled in and a cross / circle /
// square / triangle (or L1 / R1 / L2 / R2) is cut out instead. The four large
// coloured buttons (button_a/b/x/y.dds) get a dark centre with the symbol in
// its usual colour.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace wet::prompt_art
{
    enum class Symbol { None, Cross, Circle, Square, Triangle, L1, R1, L2, R2,
                        // Only redrawn with keyboard / mouse art:
                        Start, LPress, RPress, Up, Down, Left, Right, LStick, RStick, Back };

    struct Rgba { uint8_t r, g, b, a; };

    // ---- DXT5 ----

    inline void Rgb565(uint16_t c, int& r, int& g, int& b)
    {
        r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    }

    inline void DecodeDxt5(const uint8_t* src, int w, int h, Rgba* out)
    {
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++, src += 16)
            {
                int a0 = src[0], a1 = src[1], alpha[8] = { a0, a1 };
                for (int i = 2; i < 8; i++)
                    alpha[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7
                                       : (i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255));
                uint64_t abits = 0;
                for (int i = 0; i < 6; i++) abits |= uint64_t(src[2 + i]) << (8 * i);
                uint16_t c0 = uint16_t(src[8] | src[9] << 8), c1 = uint16_t(src[10] | src[11] << 8);
                int col[4][3];
                Rgb565(c0, col[0][0], col[0][1], col[0][2]);
                Rgb565(c1, col[1][0], col[1][1], col[1][2]);
                for (int k = 0; k < 3; k++)
                {
                    col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                    col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
                }
                uint32_t cbits = uint32_t(src[12] | src[13] << 8 | src[14] << 16 | uint32_t(src[15]) << 24);
                for (int i = 0; i < 16; i++)
                {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h)
                        continue;
                    int ci = (cbits >> (2 * i)) & 3, ai = int((abits >> (3 * i)) & 7);
                    out[y * w + x] = { uint8_t(col[ci][0]), uint8_t(col[ci][1]), uint8_t(col[ci][2]), uint8_t(alpha[ai]) };
                }
            }
    }

    inline uint16_t To565(int r, int g, int b)
    {
        return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
    }

    inline void EncodeDxt5(const Rgba* px, int w, int h, uint8_t* dst)
    {
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++, dst += 16)
            {
                Rgba p[16];
                for (int i = 0; i < 16; i++)
                {
                    int x = std::min(bx * 4 + (i & 3), w - 1), y = std::min(by * 4 + (i >> 2), h - 1);
                    p[i] = px[y * w + x];
                }
                // Alpha: 8-level mode between the block's extremes.
                int amin = 255, amax = 0;
                for (auto& q : p) { amin = std::min<int>(amin, q.a); amax = std::max<int>(amax, q.a); }
                dst[0] = uint8_t(amax);
                dst[1] = uint8_t(amin);
                uint64_t abits = 0;
                for (int i = 0; i < 16; i++)
                {
                    int idx = 0;
                    if (amax > amin)
                    {
                        int t = ((p[i].a - amin) * 7 + (amax - amin) / 2) / (amax - amin);   // 0 = amin .. 7 = amax
                        static const int map[8] = { 1, 7, 6, 5, 4, 3, 2, 0 };
                        idx = map[t];
                    }
                    abits |= uint64_t(idx) << (3 * i);
                }
                for (int i = 0; i < 6; i++) dst[2 + i] = uint8_t(abits >> (8 * i));
                // Colour: endpoints at the extremes along the bounding box's diagonal
                // (only pixels that show count).
                int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
                bool any = false;
                for (auto& q : p)
                    if (q.a > 8)
                    {
                        any = true;
                        int c[3] = { q.r, q.g, q.b };
                        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
                    }
                if (!any)
                    for (auto& q : p)
                    {
                        int c[3] = { q.r, q.g, q.b };
                        for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
                    }
                int axis[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
                int best0 = 0, best1 = 0, dmin = 1 << 30, dmax = -(1 << 30);
                for (int i = 0; i < 16; i++)
                {
                    if (any && p[i].a <= 8) continue;
                    int d = p[i].r * axis[0] + p[i].g * axis[1] + p[i].b * axis[2];
                    if (d < dmin) { dmin = d; best1 = i; }
                    if (d > dmax) { dmax = d; best0 = i; }
                }
                uint16_t c0 = To565(p[best0].r, p[best0].g, p[best0].b), c1 = To565(p[best1].r, p[best1].g, p[best1].b);
                if (c0 < c1) std::swap(c0, c1);
                uint32_t cbits = 0;
                if (c0 != c1)
                {
                    int col[4][3];
                    Rgb565(c0, col[0][0], col[0][1], col[0][2]);
                    Rgb565(c1, col[1][0], col[1][1], col[1][2]);
                    for (int k = 0; k < 3; k++)
                    {
                        col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                        col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
                    }
                    for (int i = 0; i < 16; i++)
                    {
                        int bi = 0, bd = 1 << 30;
                        for (int j = 0; j < 4; j++)
                        {
                            int dr = p[i].r - col[j][0], dg = p[i].g - col[j][1], db = p[i].b - col[j][2];
                            int d = dr * dr + dg * dg + db * db;
                            if (d < bd) { bd = d; bi = j; }
                        }
                        cbits |= uint32_t(bi) << (2 * i);
                    }
                }
                dst[8] = uint8_t(c0); dst[9] = uint8_t(c0 >> 8);
                dst[10] = uint8_t(c1); dst[11] = uint8_t(c1 >> 8);
                dst[12] = uint8_t(cbits); dst[13] = uint8_t(cbits >> 8);
                dst[14] = uint8_t(cbits >> 16); dst[15] = uint8_t(cbits >> 24);
            }
    }

    // ---- Shapes (coverage of a pixel by a symbol, 0..1) ----

    struct Seg { float x0, y0, x1, y1; };

    inline float SegDist(float px, float py, const Seg& s)
    {
        float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
        float l = dx * dx + dy * dy;
        float t = l > 0 ? std::clamp(((px - s.x0) * dx + (py - s.y0) * dy) / l, 0.0f, 1.0f) : 0.0f;
        float ex = s.x0 + t * dx - px, ey = s.y0 + t * dy - py;
        return std::sqrt(ex * ex + ey * ey);
    }

    // Strokes of a character in a box 0..1 wide-ish, 0 (top) .. 1 (bottom).
    inline void Glyph(char ch, float ox, float oy, float sx, float sy, std::vector<Seg>& out)
    {
        std::vector<std::pair<float, float>> line;
        auto flush = [&] {
            for (size_t i = 1; i < line.size(); i++)
                out.push_back({ ox + line[i - 1].first * sx, oy + line[i - 1].second * sy, ox + line[i].first * sx,
                                oy + line[i].second * sy });
            line.clear();
        };
        auto arc = [&](float cx, float cy, float rx, float ry, float a0, float a1) {   // degrees, 0 = right, 90 = down
            for (int i = 0; i <= 12; i++)
            {
                float a = (a0 + (a1 - a0) * i / 12.0f) * 3.14159265f / 180.0f;
                line.push_back({ cx + rx * std::cos(a), cy + ry * std::sin(a) });
            }
        };
        switch (ch)
        {
        case 'L':
            line = { { 0, 0 }, { 0, 1 }, { 0.62f, 1 } };
            flush();
            break;
        case 'R':
            line = { { 0, 1 }, { 0, 0 }, { 0.36f, 0 } };
            arc(0.36f, 0.25f, 0.27f, 0.25f, -90, 90);
            line.push_back({ 0, 0.5f });
            flush();
            line = { { 0.3f, 0.5f }, { 0.66f, 1 } };
            flush();
            break;
        case '1':
            line = { { 0.08f, 0.2f }, { 0.34f, 0 }, { 0.34f, 1 } };
            flush();
            break;
        case '2':
            arc(0.32f, 0.27f, 0.3f, 0.27f, 190, 360 + 20);
            line.push_back({ 0, 1 });
            line.push_back({ 0.66f, 1 });
            flush();
            break;
        case '3':
            arc(0.32f, 0.25f, 0.29f, 0.25f, 200, 360 + 90);
            flush();
            arc(0.32f, 0.74f, 0.32f, 0.26f, -90, 160);
            flush();
            break;
        default:
        {
            // Keyboard labels: a simple stroke font, polylines on a 0.62 x 1
            // grid separated by '|' ("x,y x,y|x,y ..."), arcs as 'o' (full
            // ellipse in the box).
            static const struct { char c; const char* d; } kFont[] = {
                { 'A', "0,1 .31,0 .62,1|.12,.62 .5,.62" },
                { 'B', "0,1 0,0 .42,0 .56,.12 .56,.36 .42,.48 0,.48|.42,.48 .6,.62 .6,.86 .44,1 0,1" },
                { 'C', ".62,.15 .48,0 .14,0 0,.18 0,.82 .14,1 .48,1 .62,.85" },
                { 'D', "0,0 0,1 .38,1 .62,.76 .62,.24 .38,0 0,0" },
                { 'E', ".62,0 0,0 0,1 .62,1|0,.5 .48,.5" },
                { 'F', ".62,0 0,0 0,1|0,.5 .48,.5" },
                { 'G', ".62,.15 .48,0 .14,0 0,.18 0,.82 .14,1 .48,1 .62,.82 .62,.55 .34,.55" },
                { 'H', "0,0 0,1|.62,0 .62,1|0,.5 .62,.5" },
                { 'I', ".1,0 .52,0|.31,0 .31,1|.1,1 .52,1" },
                { 'J', ".62,0 .62,.8 .46,1 .14,1 0,.82" },
                { 'K', "0,0 0,1|.62,0 0,.6|.18,.45 .62,1" },
                { 'L', "0,0 0,1 .62,1" },
                { 'M', "0,1 0,0 .31,.55 .62,0 .62,1" },
                { 'N', "0,1 0,0 .62,1 .62,0" },
                { 'O', "o" },
                { 'P', "0,1 0,0 .44,0 .62,.14 .62,.38 .44,.52 0,.52" },
                { 'Q', "o|.36,.7 .66,1.04" },
                { 'R', "0,1 0,0 .44,0 .62,.14 .62,.36 .44,.5 0,.5|.3,.5 .62,1" },
                { 'S', ".62,.14 .48,0 .14,0 0,.14 0,.36 .14,.48 .48,.52 .62,.64 .62,.86 .48,1 .14,1 0,.86" },
                { 'T', "0,0 .62,0|.31,0 .31,1" },
                { 'U', "0,0 0,.82 .14,1 .48,1 .62,.82 .62,0" },
                { 'V', "0,0 .31,1 .62,0" },
                { 'W', "0,0 .14,1 .31,.45 .48,1 .62,0" },
                { 'X', "0,0 .62,1|.62,0 0,1" },
                { 'Y', "0,0 .31,.5 .62,0|.31,.5 .31,1" },
                { 'Z', "0,0 .62,0 0,1 .62,1" },
                { '0', "o|.56,.12 .06,.88" },
                { '4', ".48,1 .48,0 0,.68 .62,.68" },
                { '5', ".6,0 .06,0 .02,.44 .4,.4 .62,.56 .62,.84 .44,1 .1,1 0,.9" },
                { '6', ".56,.06 .3,0 .08,.2 0,.6 .04,.9 .3,1 .56,.9 .62,.66 .5,.44 .2,.42 0,.6" },
                { '7', "0,0 .62,0 .22,1" },
                { '8', ".31,.48 .06,.36 .06,.12 .31,0 .56,.12 .56,.36 .31,.48 0,.66 0,.86 .31,1 .62,.86 .62,.66 .31,.48" },
                { '9', ".06,.94 .32,1 .54,.8 .62,.4 .58,.1 .32,0 .06,.1 0,.34 .12,.56 .42,.58 .62,.4" },
                { '-', ".1,.5 .52,.5" },
                { '/', ".62,0 0,1" },
                { '<', ".62,.12 0,.5 .62,.88" },   // arrows (left / right / up / down keys)
                { '>', "0,.12 .62,.5 0,.88" },
                { '^', "0,.7 .31,.2 .62,.7" },
                { 'v', "0,.3 .31,.8 .62,.3" },
            };
            for (auto& f : kFont)
            {
                if (f.c != ch)
                    continue;
                for (const char* p = f.d; *p;)
                {
                    if (*p == '|') { flush(); p++; continue; }
                    if (*p == ' ') { p++; continue; }
                    if (*p == 'o') { arc(0.31f, 0.5f, 0.31f, 0.5f, 0, 360); p++; continue; }
                    char* end;
                    float x = std::strtof(p, &end);
                    if (end == p || *end != ',') break;
                    float y = std::strtof(end + 1, &end);
                    line.push_back({ x, y });
                    p = end;
                }
                flush();
                break;
            }
            break;
        }
        }
    }

    // ---- Keyboard and mouse labels ----

    // What a button icon shows instead of its Xbox letter: a symbol (the
    // PlayStation art), a key name, or a mouse with one button highlighted.
    struct Label
    {
        enum class Kind { Text, Mouse } kind = Kind::Text;
        std::string text;    // upper case, short ("SPC", "E", "SHFT")
        int mouseButton = 0; // 0 left, 1 right, 2 middle, -1 none (moving the mouse)
    };

    struct Box { float x0, y0, x1, y1; };   // a filled rectangle

    // A key name shortened to fit a small icon.
    inline std::string ShortKeyName(std::string key)
    {
        static const struct { const char* from; const char* to; } kShort[] = {
            { "SPACE", "SPC" }, { "ENTER", "ENT" }, { "BACKSPACE", "BKSP" }, { "LSHIFT", "SHFT" }, { "RSHIFT", "SHFT" },
            { "LCTRL", "CTRL" }, { "RCTRL", "CTRL" }, { "LALT", "ALT" }, { "RALT", "ALT" }, { "CAPSLOCK", "CAPS" },
            { "UP", "^" }, { "DOWN", "v" }, { "LEFT", "<" }, { "RIGHT", ">" }, { "INSERT", "INS" }, { "DELETE", "DEL" },
            { "HOME", "HOME" }, { "END", "END" }, { "PAGEUP", "PGUP" }, { "PAGEDOWN", "PGDN" }, { "MINUS", "-" },
            { "EQUALS", "EQ" }, { "LBRACKET", "LBR" }, { "RBRACKET", "RBR" }, { "SEMICOLON", "SEMI" },
            { "APOSTROPHE", "APOS" }, { "COMMA", "COMA" }, { "PERIOD", "PER" }, { "SLASH", "/" },
            { "BACKSLASH", "BSL" }, { "GRAVE", "GRV" },
        };
        for (auto& e : kShort)
            if (key == e.from)
                return e.to;
        if (key.size() == 4 && key.compare(0, 3, "NUM") == 0)
            return "N" + key.substr(3);
        return key.size() > 4 ? key.substr(0, 4) : key;
    }

    // Strokes (and filled boxes) for a label centred in a box of the given size.
    inline void LabelStrokes(const Label& label, float cx, float cy, float bw, float bh, std::vector<Seg>& segs,
                             std::vector<Box>& fills, float& width)
    {
        if (label.kind == Label::Kind::Mouse)
        {
            // A mouse: rounded body, a split between the buttons, the bound one filled.
            float mh = bh * 0.86f, mw = std::min(bw * 0.8f, mh * 0.68f);
            float x0 = cx - mw * 0.5f, x1 = cx + mw * 0.5f, y0 = cy - mh * 0.5f, y1 = cy + mh * 0.5f;
            float r = mw * 0.42f;
            std::vector<std::pair<float, float>> body;
            for (int i = 0; i <= 8; i++)   // top arc
            {
                float a = 3.14159265f * (1 + i / 8.0f);
                body.push_back({ cx + (mw * 0.5f) * std::cos(a), y0 + r + r * std::sin(a) });
            }
            body.push_back({ x1, y1 - r * 0.6f });
            for (int i = 0; i <= 8; i++)   // bottom arc
            {
                float a = 3.14159265f * (i / 8.0f);
                body.push_back({ cx + (mw * 0.5f) * std::cos(a), y1 - r * 0.6f + r * 0.6f * std::sin(a) });
            }
            body.push_back(body.front());
            for (size_t i = 1; i < body.size(); i++)
                segs.push_back({ body[i - 1].first, body[i - 1].second, body[i].first, body[i].second });
            float split = y0 + mh * 0.42f;
            segs.push_back({ x0, split, x1, split });
            segs.push_back({ cx, y0, cx, split });
            width = std::max(1.3f, mw * 0.11f);
            float in = width * 0.5f;
            if (label.mouseButton == 0)
                fills.push_back({ x0 + in, y0 + in, cx, split });
            else if (label.mouseButton == 1)
                fills.push_back({ cx, y0 + in, x1 - in, split });
            else if (label.mouseButton == 2)
                fills.push_back({ cx - mw * 0.1f, y0 + mh * 0.1f, cx + mw * 0.1f, split - mh * 0.06f });
            return;   // -1: just the mouse (moving it)
        }
        int n = int(std::max<size_t>(label.text.size(), 1));
        float th = std::min(bh * 0.62f, bw * 0.86f / (n * 0.62f + (n - 1) * 0.24f));
        float cw = th * 0.62f, gap = th * 0.24f;
        float total = n * cw + (n - 1) * gap;
        float ox = cx - total * 0.5f, oy = cy - th * 0.5f;
        for (int i = 0; i < int(label.text.size()); i++)
            Glyph(label.text[i], ox + i * (cw + gap), oy, th, th, segs);
        width = std::max(1.25f, th * (n > 2 ? 0.15f : 0.19f));
    }

    // Symbol geometry around (cx, cy) for a shape of radius r (pixels).
    inline std::vector<Seg> SymbolStrokes(Symbol s, float cx, float cy, float r, float& width, float& ringR)
    {
        std::vector<Seg> segs;
        ringR = 0;
        width = r * 0.2f;
        switch (s)
        {
        case Symbol::Cross:
        {
            float d = r * 0.42f;
            segs.push_back({ cx - d, cy - d, cx + d, cy + d });
            segs.push_back({ cx - d, cy + d, cx + d, cy - d });
            break;
        }
        case Symbol::Circle:
            ringR = r * 0.44f;
            break;
        case Symbol::Square:
        {
            float d = r * 0.38f;
            segs.push_back({ cx - d, cy - d, cx + d, cy - d });
            segs.push_back({ cx + d, cy - d, cx + d, cy + d });
            segs.push_back({ cx + d, cy + d, cx - d, cy + d });
            segs.push_back({ cx - d, cy + d, cx - d, cy - d });
            break;
        }
        case Symbol::Triangle:
        {
            float R = r * 0.52f, y = cy + r * 0.08f;
            float x0 = cx, y0 = y - R;
            float x1 = cx + R * 0.866f, y1 = y + R * 0.5f;
            float x2 = cx - R * 0.866f, y2 = y + R * 0.5f;
            segs.push_back({ x0, y0, x1, y1 });
            segs.push_back({ x1, y1, x2, y2 });
            segs.push_back({ x2, y2, x0, y0 });
            break;
        }
        default:
            break;
        }
        return segs;
    }

    inline float Coverage(float px, float py, const std::vector<Seg>& segs, float width, float cx, float cy, float ringR,
                          const std::vector<Box>* fills = nullptr)
    {
        // 4x4 supersampling of the pixel.
        int hit = 0;
        for (int sy = 0; sy < 4; sy++)
            for (int sx = 0; sx < 4; sx++)
            {
                float x = px + (sx + 0.5f) / 4, y = py + (sy + 0.5f) / 4;
                bool in = false;
                if (ringR > 0)
                {
                    float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                    in = std::fabs(d - ringR) <= width * 0.5f;
                }
                for (auto& s : segs)
                    if (!in && SegDist(x, y, s) <= width * 0.5f)
                        in = true;
                if (fills)
                    for (auto& b : *fills)
                        if (!in && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1)
                            in = true;
                hit += in;
            }
        return hit / 16.0f;
    }

    // ---- Icons ----

    // A flat icon with its glyph cut out: fill the cut-out, cut the new symbol.
    // flatten: also paint over anything drawn in another colour (the "_s"
    // variant has a coloured copy of the glyph as a shadow).
    inline bool RedrawCutout(Rgba* px, int w, int h, Symbol sym, bool flatten = false, const Label* label = nullptr)
    {
        // Outside: transparent pixels connected to the border.
        std::vector<uint8_t> outside(size_t(w) * h, 0);
        std::vector<int> stack;
        auto push = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return;
            int i = y * w + x;
            if (outside[i] || px[i].a >= 128) return;
            outside[i] = 1;
            stack.push_back(i);
        };
        for (int x = 0; x < w; x++) { push(x, 0); push(x, h - 1); }
        for (int y = 0; y < h; y++) { push(0, y); push(w - 1, y); }
        while (!stack.empty())
        {
            int i = stack.back();
            stack.pop_back();
            int x = i % w, y = i / w;
            push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
        }
        // The shape's extent.
        int x0 = w, y0 = h, x1 = -1, y1 = -1, solid = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                const Rgba& p = px[y * w + x];
                if (outside[y * w + x]) continue;
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                solid += p.a >= 250;
            }
        if (x1 < x0 || solid < 16)
            return false;
        // Fill the old cut-out (anything not next to the outside) with the
        // colour of the nearest solid pixel, keeping the shape's grain.
        std::vector<uint8_t> isHole(size_t(w) * h, 0);
        int hx0 = w, hy0 = h, hx1 = -1, hy1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                if (outside[i] || px[i].a >= 250) continue;
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h || outside[yy * w + xx]) { edge = true; break; }
                    }
                if (edge) continue;
                isHole[i] = 1;
                if (px[i].a < 128)   // the glyph itself, not the grain's speckles
                {
                    hx0 = std::min(hx0, x); hy0 = std::min(hy0, y); hx1 = std::max(hx1, x); hy1 = std::max(hy1, y);
                }
            }
        std::vector<int> from(size_t(w) * h, -1), queue;
        for (int i = 0; i < w * h; i++)
            if (!outside[i] && !isHole[i] && px[i].a >= 250) { from[i] = i; queue.push_back(i); }
        for (size_t q = 0; q < queue.size(); q++)
        {
            int i = queue[q], x = i % w, y = i / w;
            const int nx[4] = { x + 1, x - 1, x, x }, ny[4] = { y, y, y + 1, y - 1 };
            for (int k = 0; k < 4; k++)
            {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                int j = ny[k] * w + nx[k];
                if (!isHole[j] || from[j] >= 0) continue;
                from[j] = from[i];
                queue.push_back(j);
            }
        }
        for (int i = 0; i < w * h; i++)
            if (isHole[i] && from[i] >= 0)
            {
                px[i] = px[from[i]];
                px[i].a = 255;
            }
        if (flatten)
        {
            // The most common colour (coarsely binned) of the solid pixels.
            std::vector<int> bins(4096, 0);
            auto bin = [](const Rgba& p) { return (p.r >> 4) << 8 | (p.g >> 4) << 4 | (p.b >> 4); };
            for (int i = 0; i < w * h; i++)
                if (!outside[i] && px[i].a >= 250) bins[bin(px[i])]++;
            int top = int(std::max_element(bins.begin(), bins.end()) - bins.begin());
            int tr = 0, tg = 0, tb = 0, tn = 0;
            for (int i = 0; i < w * h; i++)
                if (!outside[i] && px[i].a >= 250 && bin(px[i]) == top) { tr += px[i].r; tg += px[i].g; tb += px[i].b; tn++; }
            if (tn)
                for (int i = 0; i < w * h; i++)
                {
                    Rgba& p = px[i];
                    int dr = p.r - tr / tn, dg = p.g - tg / tn, db = p.b - tb / tn;
                    if (dr * dr + dg * dg + db * db > 40 * 40)
                        p = { uint8_t(tr / tn), uint8_t(tg / tn), uint8_t(tb / tn), p.a };
                }
        }
        // Cut the symbol where the old glyph was.
        float cx = (x0 + x1 + 1) * 0.5f, cy = (y0 + y1 + 1) * 0.5f;
        float bw = float(x1 - x0 + 1), bh = float(y1 - y0 + 1);
        float gw = bw * 0.5f, gh = bh * 0.5f;
        if (hx1 >= hx0)
        {
            cx = (hx0 + hx1 + 1) * 0.5f;
            cy = (hy0 + hy1 + 1) * 0.5f;
            gw = float(hx1 - hx0 + 1);
            gh = float(hy1 - hy0 + 1);
        }
        std::vector<Seg> segs;
        std::vector<Box> fills;
        float width, ringR = 0;
        if (label)
        {
            // A key name or mouse uses most of the shape, not just the old letter.
            bool mouse = label->kind == Label::Kind::Mouse;
            float lw = std::max(gw, bw * (mouse ? 0.8f : 0.72f)), lh = std::max(gh, bh * (mouse ? 0.74f : 0.62f));
            float lcx = (x0 + x1 + 1) * 0.5f, lcy = hx1 >= hx0 ? cy : (y0 + y1 + 1) * 0.5f;
            cx = lcx;
            cy = lcy;
            LabelStrokes(*label, cx, cy, lw, lh, segs, fills, width);
        }
        else if (sym == Symbol::L1 || sym == Symbol::R1 || sym == Symbol::L2 || sym == Symbol::R2)
        {
            // Two characters in the old text's box.
            float th = std::min(gh, gw * 1.05f / 1.52f);
            float cw = th * 0.62f, gap = th * 0.28f;
            float total = cw * 2 + gap;
            float ox = cx - total * 0.5f, oy = cy - th * 0.5f;
            char a = (sym == Symbol::L1 || sym == Symbol::L2) ? 'L' : 'R';
            char b = (sym == Symbol::L1 || sym == Symbol::R1) ? '1' : '2';
            Glyph(a, ox, oy, th, th, segs);
            Glyph(b, ox + cw + gap, oy, th, th, segs);
            width = std::max(1.6f, th * 0.2f);
        }
        else
            segs = SymbolStrokes(sym, cx, cy, std::min(bw, bh) * 0.5f, width, ringR);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float c = Coverage(float(x), float(y), segs, width, cx, cy, ringR, &fills);
                Rgba& p = px[y * w + x];
                p.a = uint8_t(std::lround(p.a * (1 - c)));
            }
        return true;
    }

    inline void SymbolColour(Symbol s, int& r, int& g, int& b)
    {
        switch (s)
        {
        case Symbol::Cross: r = 124; g = 168; b = 236; break;
        case Symbol::Circle: r = 240; g = 92; b = 102; break;
        case Symbol::Square: r = 228; g = 136; b = 200; break;
        default: r = 64; g = 214; b = 168; break;   // triangle
        }
    }

    // The large coloured buttons: dark glossy centre, coloured symbol.
    inline bool RedrawOrb(Rgba* px, int w, int h, Symbol sym, const Label* label = nullptr)
    {
        double sx = 0, sy = 0;
        int n = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (px[y * w + x].a >= 200) { sx += x + 0.5; sy += y + 0.5; n++; }
        if (n < 64)
            return false;
        float cx = float(sx / n), cy = float(sy / n), R = float(std::sqrt(n / 3.14159265));
        float inner = R * 0.8f;
        float width, ringR = 0;
        std::vector<Seg> segs;
        std::vector<Box> fills;
        int sr, sg, sb;
        if (label)
        {
            // Light key name / mouse on the dark centre.
            LabelStrokes(*label, cx, cy, inner * 1.5f, inner * 1.3f, segs, fills, width);
            sr = sg = sb = 236;
        }
        else
        {
            segs = SymbolStrokes(sym, cx, cy, inner, width, ringR);
            width *= 0.85f;
            SymbolColour(sym, sr, sg, sb);
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                Rgba& p = px[y * w + x];
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                // Grey the rim (it carried the old colour's fringe).
                int l = (p.r * 3 + p.g * 6 + p.b) / 10;
                if (d >= inner)
                {
                    l = l * 6 / 10;
                    p.r = p.g = p.b = uint8_t(l);
                    continue;
                }
                // Glossy dark centre: lighter towards the top left.
                float t = std::clamp((dx + dy) / (2 * inner) * 0.5f + 0.5f, 0.0f, 1.0f);
                float base = 70 - 50 * t;
                float hl = std::max(0.0f, 1 - std::sqrt((dx + inner * 0.3f) * (dx + inner * 0.3f) +
                                                       (dy + inner * 0.45f) * (dy + inner * 0.45f)) / (inner * 0.6f));
                base += 40 * hl * hl;
                float edgeFade = std::clamp((inner - d) / 1.5f, 0.0f, 1.0f);   // soften into the rim
                float c = Coverage(float(x), float(y), segs, width, cx, cy, ringR, &fills) * edgeFade;
                float r = base + (sr - base) * c, g = base + (sg - base) * c, b = base + (sb - base) * c;
                float keep = 1 - edgeFade;   // blend with the darkened rim at the boundary
                p.r = uint8_t(std::clamp(r * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
                p.g = uint8_t(std::clamp(g * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
                p.b = uint8_t(std::clamp(b * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
            }
        return true;
    }

    // ---- DXT1 (the in-world prompts) ----

    inline void DecodeDxt1(const uint8_t* src, int w, int h, Rgba* out)
    {
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++, src += 8)
            {
                uint16_t c0 = uint16_t(src[0] | src[1] << 8), c1 = uint16_t(src[2] | src[3] << 8);
                int col[4][4];
                Rgb565(c0, col[0][0], col[0][1], col[0][2]);
                Rgb565(c1, col[1][0], col[1][1], col[1][2]);
                col[0][3] = col[1][3] = col[2][3] = col[3][3] = 255;
                for (int k = 0; k < 3; k++)
                    if (c0 > c1)
                    {
                        col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                        col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
                    }
                    else
                    {
                        col[2][k] = (col[0][k] + col[1][k]) / 2;
                        col[3][k] = 0;
                    }
                if (c0 <= c1)
                    col[3][3] = 0;
                uint32_t bits = uint32_t(src[4] | src[5] << 8 | src[6] << 16 | uint32_t(src[7]) << 24);
                for (int i = 0; i < 16; i++)
                {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h)
                        continue;
                    int ci = (bits >> (2 * i)) & 3;
                    out[y * w + x] = { uint8_t(col[ci][0]), uint8_t(col[ci][1]), uint8_t(col[ci][2]), uint8_t(col[ci][3]) };
                }
            }
    }

    // Opaque DXT1: the colour half of the DXT5 encoder's blocks.
    inline void EncodeDxt1(const Rgba* px, int w, int h, uint8_t* dst)
    {
        int blocks = ((w + 3) / 4) * ((h + 3) / 4);
        std::vector<uint8_t> dxt5(size_t(blocks) * 16);
        std::vector<Rgba> opaque(px, px + size_t(w) * h);
        for (auto& p : opaque)
            p.a = 255;
        EncodeDxt5(opaque.data(), w, h, dxt5.data());
        for (int b = 0; b < blocks; b++)
            std::memcpy(dst + b * 8, dxt5.data() + b * 16 + 8, 8);
    }

    // The in-world prompt (over an enemy to finish, on usable objects): an
    // opaque disc in the button's colour with a black letter, a dark grungy ring
    // round it and white outside (drawn multiplied, so white shows the scene).
    // Finds it and says which button it is (None if it isn't one).
    inline Symbol FindWorldOrb(const Rgba* px, int w, int h, float& cx, float& cy, float& radius)
    {
        auto at = [&](int x, int y) { return px[y * w + x]; };
        // White corners.
        for (auto [x, y] : { std::pair{ 1, 1 }, { w - 2, 1 }, { 1, h - 2 }, { w - 2, h - 2 } })
        {
            Rgba p = at(x, y);
            if (p.r < 215 || p.g < 215 || p.b < 215)
                return Symbol::None;
        }
        // A saturated disc in the middle: its colour picks the button.
        double sx = 0, sy = 0, sr = 0, sg = 0, sb = 0;
        int n = 0, dark = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                Rgba p = at(x, y);
                int mx = std::max({ p.r, p.g, p.b }), mn = std::min({ p.r, p.g, p.b });
                float dx = x + 0.5f - w / 2.0f, dy = y + 0.5f - h / 2.0f;
                if (dx * dx + dy * dy < (w * 0.4f) * (w * 0.4f))
                {
                    if (mx - mn > 50 && mx > 70)
                    {
                        sx += x + 0.5; sy += y + 0.5; n++;
                        sr += p.r; sg += p.g; sb += p.b;
                    }
                    else if (mx < 40)
                        dark++;
                }
            }
        if (n < w * h / 20 || dark < w * h / 60)
            return Symbol::None;
        cx = float(sx / n);
        cy = float(sy / n);
        // The disc's area includes the letter cut into it.
        radius = float(std::sqrt((n + dark * 0.5) / 3.14159265));
        if (std::fabs(cx - w / 2.0f) > w * 0.12f || std::fabs(cy - h / 2.0f) > h * 0.12f || radius > w * 0.45f)
            return Symbol::None;
        double r = sr / n, g = sg / n, b = sb / n;
        if (b > r * 1.3 && b > g * 1.1) return Symbol::Square;              // blue X
        if (g > r * 1.3 && g > b * 1.2) return Symbol::Cross;               // green A
        if (r > g * 1.5 && r > b * 1.5) return Symbol::Circle;              // red B
        if (r > b * 1.6 && g > b * 1.4) return Symbol::Triangle;            // yellow Y
        return Symbol::None;
    }

    // Repaints the disc: PlayStation symbol, or the key / mouse button.
    inline void RedrawWorldOrb(Rgba* px, int w, int h, float cx, float cy, float radius, Symbol sym, const Label* label)
    {
        float inner = radius * 1.02f;
        float width, ringR = 0;
        std::vector<Seg> segs;
        std::vector<Box> fills;
        int sr, sg, sb;
        if (label)
        {
            LabelStrokes(*label, cx, cy, inner * 1.5f, inner * 1.3f, segs, fills, width);
            sr = sg = sb = 245;
        }
        else
        {
            segs = SymbolStrokes(sym, cx, cy, inner * 0.95f, width, ringR);
            width *= 0.85f;
            SymbolColour(sym, sr, sg, sb);
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                if (d >= inner + 1)
                    continue;
                Rgba& p = px[y * w + x];
                // Dark glossy disc, lighter towards the top left (as the HUD's).
                float t = std::clamp((dx + dy) / (2 * inner) * 0.5f + 0.5f, 0.0f, 1.0f);
                float base = 62 - 42 * t;
                float hl = std::max(0.0f, 1 - std::sqrt((dx + inner * 0.3f) * (dx + inner * 0.3f) +
                                                       (dy + inner * 0.45f) * (dy + inner * 0.45f)) / (inner * 0.6f));
                base += 36 * hl * hl;
                float edge = std::clamp(inner + 1 - d, 0.0f, 1.0f);
                float c = Coverage(float(x), float(y), segs, width, cx, cy, ringR, &fills);
                float r = base + (sr - base) * c, g = base + (sg - base) * c, b = base + (sb - base) * c;
                p.r = uint8_t(std::clamp(p.r + (r - p.r) * edge, 0.0f, 255.0f));
                p.g = uint8_t(std::clamp(p.g + (g - p.g) * edge, 0.0f, 255.0f));
                p.b = uint8_t(std::clamp(p.b + (b - p.b) * edge, 0.0f, 255.0f));
            }
    }

    // Which symbol a button texture gets ("button_A_b.dds", "button_3", ...).
    inline Symbol ForName(std::string_view name, bool& orb, bool& flatten)
    {
        orb = false;
        flatten = false;
        std::string n(name);
        size_t slash = n.find_last_of("/\\:");
        if (slash != std::string::npos) n = n.substr(slash + 1);
        for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".dds") == 0) n.resize(n.size() - 4);
        // The front end's "select" prompt (A); its _japan twin is only shown on
        // Japanese consoles.
        if (n == "selectcardbutton")
        {
            orb = true;   // a filled button like the a / b / x / y ones (the letter isn't cut out)
            return Symbol::Cross;
        }
        if (n.compare(0, 7, "button_") != 0)
            return Symbol::None;
        std::string k = n.substr(7);
        static const struct { const char* key; Symbol s; } numbered[] = {
            { "1", Symbol::Square }, { "2", Symbol::Cross }, { "3", Symbol::Circle }, { "4", Symbol::Triangle },
            { "5", Symbol::L1 }, { "6", Symbol::R1 }, { "7", Symbol::L2 }, { "8", Symbol::R2 },
            // Keyboard / mouse art only:
            { "9", Symbol::Back }, { "10", Symbol::Start }, { "11", Symbol::Down }, { "12", Symbol::Up },
            { "13", Symbol::Left }, { "14", Symbol::Right }, { "15", Symbol::LPress }, { "16", Symbol::RPress },
        };
        for (auto& e : numbered)
            if (k == e.key) return e.s;
        static const struct { const char* key; Symbol s; } letters[] = {
            { "a", Symbol::Cross }, { "b", Symbol::Circle }, { "x", Symbol::Square }, { "y", Symbol::Triangle },
        };
        for (auto& e : letters)
            if (k == e.key) { orb = true; return e.s; }
        static const struct { const char* prefix; Symbol s; } families[] = {
            { "a_", Symbol::Cross }, { "b_", Symbol::Circle }, { "x_", Symbol::Square }, { "y_", Symbol::Triangle },
            { "lb_", Symbol::L1 }, { "rb_", Symbol::R1 }, { "lt_", Symbol::L2 }, { "rt_", Symbol::R2 },
        };
        for (auto& e : families)
            if (k.compare(0, std::strlen(e.prefix), e.prefix) == 0)
            {
                flatten = k.size() >= 2 && k.compare(k.size() - 2, 2, "_s") == 0;
                return e.s;
            }
        static const struct { const char* prefix; Symbol s; } keyboardOnly[] = {
            { "start_", Symbol::Start }, { "lpress_", Symbol::LPress }, { "rpress_", Symbol::RPress },
            { "up_", Symbol::Up }, { "down_", Symbol::Down }, { "left_", Symbol::Left }, { "right_", Symbol::Right },
            { "lthumb_", Symbol::LStick }, { "rthumb_", Symbol::RStick },
            { "select_", Symbol::Back }, { "updown_", Symbol::Up }, { "leftright_", Symbol::Left },
        };
        for (auto& e : keyboardOnly)
            if (k.compare(0, std::strlen(e.prefix), e.prefix) == 0)
                return e.s;
        return Symbol::None;
    }

    // Redraw a DDS file in place (DXT5, top level only). False if it isn't one
    // of the button textures or not a format handled here.
    inline bool RedrawDds(std::string_view name, uint8_t* dds, size_t size, const Label* label = nullptr)
    {
        bool orb, flatten;
        Symbol sym = ForName(name, orb, flatten);
        if (sym > Symbol::R2 && !label)
            return false;   // no PlayStation version: those stay as they are
        if (sym == Symbol::None || size < 128 || std::memcmp(dds, "DDS ", 4) != 0 || std::memcmp(dds + 84, "DXT5", 4) != 0)
            return false;
        auto le32 = [&](size_t o) { return uint32_t(dds[o] | dds[o + 1] << 8 | dds[o + 2] << 16 | uint32_t(dds[o + 3]) << 24); };
        int h = int(le32(12)), w = int(le32(16));
        if (w < 8 || h < 8 || w > 256 || h > 256 || (w & 3) || (h & 3) || size < 128 + size_t(w) * h)
            return false;
        std::vector<Rgba> px(size_t(w) * h);
        DecodeDxt5(dds + 128, w, h, px.data());
        if (!(orb ? RedrawOrb(px.data(), w, h, sym, label) : RedrawCutout(px.data(), w, h, sym, flatten, label)))
            return false;
        EncodeDxt5(px.data(), w, h, dds + 128);
        return true;
    }
}
