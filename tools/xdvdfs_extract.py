#!/usr/bin/env python3
"""Minimal, resumable Xbox 360 (XGD2/XGD3) XDVDFS extractor.
Usage: xdvdfs_extract.py <iso> <outdir> [--list]"""
import os, struct, sys
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
OFFSETS = [0x0, 0xFD90000, 0x2080000, 0x18300000]  # plain, XGD2, XGD3, XGD1

def find_partition(f):
    for off in OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return off
    raise SystemExit("XDVDFS volume descriptor not found")

def walk(f, base, sector, size, path, out):
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        o = stack.pop()
        if o in seen or o + 14 > len(data):
            continue
        seen.add(o)
        left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, o)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # padding
        name = data[o + 14:o + 14 + nlen].decode("latin-1")
        p = os.path.join(path, name)
        if attr & 0x10:
            out.append(("d", p, 0, 0))
            walk(f, base, start, fsize, p, out)
        else:
            out.append(("f", p, start, fsize))
        if left:  stack.append(left * 4)
        if right: stack.append(right * 4)

def main():
    iso, outdir = sys.argv[1], sys.argv[2]
    f = open(iso, "rb")
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + 20)
    root_sec, root_size = struct.unpack("<II", f.read(8))
    entries = []
    walk(f, base, root_sec, root_size, "", entries)
    if "--list" in sys.argv:
        for t, p, s, n in sorted(entries, key=lambda e: e[1].lower()):
            print(t, n, p)
        print(f"partition@{hex(base)} files={sum(e[0]=='f' for e in entries)} "
              f"bytes={sum(e[3] for e in entries)}")
        return
    for t, p, s, n in entries:
        dst = os.path.join(outdir, p)
        if t == "d":
            os.makedirs(dst, exist_ok=True); continue
        os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == n:
            continue  # already done (resumable)
        f.seek(base + s * SECTOR)
        tmp = dst + ".part"
        with open(tmp, "wb") as o:
            left = n
            while left:
                chunk = f.read(min(left, 8 << 20))
                if not chunk: raise SystemExit(f"short read: {p}")
                o.write(chunk); left -= len(chunk)
        os.replace(tmp, dst)
        print("extracted", p, n, flush=True)
    print("DONE", flush=True)

if __name__ == "__main__":
    main()
