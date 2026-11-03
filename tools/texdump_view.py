#!/usr/bin/env python3
"""Pictures of the textures saved with Ctrl+F9 (texdump/*.bin, guest layout).

    python tools/texdump_view.py build/runtime/texdump [out folder]

Untiles (Xbox 360 2D tiling), byte-swaps and decodes DXT1/3/5 and 8888 textures
to PNGs, plus contact sheets (sheet_N.png) with the file names. Needs Pillow.
"""
import os
import re
import struct
import sys

from PIL import Image, ImageDraw

NAME = re.compile(r"([0-9A-F]{8})_f(\d+)_(\d+)x(\d+)_t(\d)_e(\d)_p(\d+)\.bin$")
# format id -> (block size in pixels, bytes per block, DDS fourcc or None for 8888)
FORMATS = {18: (4, 8, b"DXT1"), 19: (4, 16, b"DXT3"), 20: (4, 16, b"DXT5"), 6: (1, 4, None)}


def tiled2d(x, y, pitch, bpb_log2):
    """texture_address::Tiled2D (x, y, pitch in blocks)."""
    outer = (((y >> 5) * (pitch >> 5) + (x >> 5)) << 6)
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oib = (outer | inner) << bpb_log2
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 15) | (((oib >> 4) & 1) << 5) | \
        (((oib >> 5) & 7) << 8) | (oib >> 8 << 12)


def swap(data, endian):
    if endian == 1:    # 8in16
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if endian == 2:    # 8in32
        return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    if endian == 3:    # 16in32
        return b"".join(data[i + 2:i + 4] + data[i:i + 2] for i in range(0, len(data), 4))
    return data


def decode(path):
    m = NAME.search(os.path.basename(path))
    if not m:
        return None
    fmt, w, h, tiled, endian, pitch = int(m[2]), int(m[3]), int(m[4]), int(m[5]), int(m[6]), int(m[7])
    if fmt not in FORMATS:
        return None
    bs, bpb, fourcc = FORMATS[fmt]
    raw = swap(open(path, "rb").read(), endian)
    wb, hb = (w + bs - 1) // bs, (h + bs - 1) // bs
    log2 = bpb.bit_length() - 1
    out = bytearray(wb * hb * bpb)
    for y in range(hb):
        for x in range(wb):
            src = tiled2d(x, y, pitch, log2) if tiled else (y * pitch + x) * bpb
            if src + bpb <= len(raw):
                out[(y * wb + x) * bpb:(y * wb + x + 1) * bpb] = raw[src:src + bpb]
    if fourcc is None:   # 8888: stored ARGB in guest order
        px = bytearray(len(out))
        px[0::4], px[1::4], px[2::4], px[3::4] = out[1::4], out[2::4], out[3::4], out[0::4]
        return Image.frombytes("RGBA", (wb, hb), bytes(px))
    header = struct.pack("<4sIIIIIII44xII4sIIIIIIIIII", b"DDS ", 124, 0x81007, h, w, len(out), 0, 1,
                         32, 4, fourcc, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    import io
    return Image.open(io.BytesIO(header + bytes(out))).convert("RGBA")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, "png")
    os.makedirs(dst, exist_ok=True)
    pictures = []
    for name in sorted(os.listdir(src)):
        if not name.endswith(".bin"):
            continue
        try:
            im = decode(os.path.join(src, name))
        except Exception as e:  # noqa: BLE001 - keep going
            print("  skipped", name, e)
            continue
        if im:
            im.save(os.path.join(dst, name[:-4] + ".png"))
            pictures.append((name, im))
    cell, cols = 160, 8
    for s in range(0, len(pictures), 48):
        page = pictures[s:s + 48]
        rows = (len(page) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell, rows * (cell + 14)), (70, 70, 70, 255))
        d = ImageDraw.Draw(sheet)
        for i, (name, im) in enumerate(page):
            t = im.copy()
            t.thumbnail((cell - 4, cell - 4))
            x, y = (i % cols) * cell, (i // cols) * (cell + 14)
            sheet.alpha_composite(t, (x + 2, y + 2))
            d.text((x + 2, y + cell), name[:24], fill=(255, 255, 0, 255))
        sheet.save(os.path.join(dst, f"sheet_{s // 48}.png"))
    print(f"{len(pictures)} textures -> {dst}")


if __name__ == "__main__":
    main()
