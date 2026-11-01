// PlayStation button art, drawn over the game's own Xbox button textures
// (prompts.cpp). Plain functions with no runtime dependencies.
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
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace wet::prompt_art
{
    enum class Symbol { None, Cross, Circle, Square, Triangle, L1, R1, L2, R2 };

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
        }
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

    inline float Coverage(float px, float py, const std::vector<Seg>& segs, float width, float cx, float cy, float ringR)
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
                hit += in;
            }
        return hit / 16.0f;
    }

    // ---- Icons ----

    // A flat icon with its glyph cut out: fill the cut-out, cut the new symbol.
    // flatten: also paint over anything drawn in another colour (the "_s"
    // variant has a coloured copy of the glyph as a shadow).
    inline bool RedrawCutout(Rgba* px, int w, int h, Symbol sym, bool flatten = false)
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
        float width, ringR = 0;
        if (sym == Symbol::L1 || sym == Symbol::R1 || sym == Symbol::L2 || sym == Symbol::R2)
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
                float c = Coverage(float(x), float(y), segs, width, cx, cy, ringR);
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
    inline bool RedrawOrb(Rgba* px, int w, int h, Symbol sym)
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
        float width, ringR;
        std::vector<Seg> segs = SymbolStrokes(sym, cx, cy, inner, width, ringR);
        width *= 0.85f;
        int sr, sg, sb;
        SymbolColour(sym, sr, sg, sb);
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
                float c = Coverage(float(x), float(y), segs, width, cx, cy, ringR) * edgeFade;
                float r = base + (sr - base) * c, g = base + (sg - base) * c, b = base + (sb - base) * c;
                float keep = 1 - edgeFade;   // blend with the darkened rim at the boundary
                p.r = uint8_t(std::clamp(r * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
                p.g = uint8_t(std::clamp(g * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
                p.b = uint8_t(std::clamp(b * (1 - keep) + l * 0.6f * keep, 0.0f, 255.0f));
            }
        return true;
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
        if (n.compare(0, 7, "button_") != 0)
            return Symbol::None;
        std::string k = n.substr(7);
        static const struct { const char* key; Symbol s; } numbered[] = {
            { "1", Symbol::Square }, { "2", Symbol::Cross }, { "3", Symbol::Circle }, { "4", Symbol::Triangle },
            { "5", Symbol::L1 }, { "6", Symbol::R1 }, { "7", Symbol::L2 }, { "8", Symbol::R2 },
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
        return Symbol::None;
    }

    // Redraw a DDS file in place (DXT5, top level only). False if it isn't one
    // of the button textures or not a format handled here.
    inline bool RedrawDds(std::string_view name, uint8_t* dds, size_t size)
    {
        bool orb, flatten;
        Symbol sym = ForName(name, orb, flatten);
        if (sym == Symbol::None || size < 128 || std::memcmp(dds, "DDS ", 4) != 0 || std::memcmp(dds + 84, "DXT5", 4) != 0)
            return false;
        auto le32 = [&](size_t o) { return uint32_t(dds[o] | dds[o + 1] << 8 | dds[o + 2] << 16 | uint32_t(dds[o + 3]) << 24); };
        int h = int(le32(12)), w = int(le32(16));
        if (w < 8 || h < 8 || w > 256 || h > 256 || (w & 3) || (h & 3) || size < 128 + size_t(w) * h)
            return false;
        std::vector<Rgba> px(size_t(w) * h);
        DecodeDxt5(dds + 128, w, h, px.data());
        if (!(orb ? RedrawOrb(px.data(), w, h, sym) : RedrawCutout(px.data(), w, h, sym, flatten)))
            return false;
        EncodeDxt5(px.data(), w, h, dds + 128);
        return true;
    }
}
