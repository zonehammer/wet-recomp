#!/usr/bin/env python3
"""Extract your WET disc image into game/extracted/ and sanity-check it.

Usage:  python tools/prepare_game.py "path/to/WET (USA, Europe) (En,Fr,De,Es,It).iso"
        python tools/prepare_game.py "path/to/an already extracted disc folder"

A folder is copied (it must contain default.xex, at the top or one or two levels
down). Resumable: re-running skips files that are already complete.
"""
import os, shutil, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdvdfs_extract as xd

EXPECTED_TITLE_ID = 0x425307DB
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "game", "extracted")

def find_xex_folder(folder):
    """The folder holding default.xex: the one given, or one or two levels down."""
    for root, dirs, files in os.walk(folder):
        if root[len(folder):].count(os.sep) > 2:
            dirs[:] = []
            continue
        if any(f.lower() == "default.xex" for f in files):
            return root
    return None


def copy_folder(src):
    src = find_xex_folder(os.path.abspath(src))
    if not src:
        sys.exit("No default.xex in that folder (or one or two levels below it). Choose the folder of your "
                 "extracted WET disc: the one with default.xex, lu0, media, streams0...")
    if os.path.abspath(src) == os.path.abspath(OUT):
        print("   already in place")
        return
    files = []
    for root, _, names in os.walk(src):
        for n in names:
            full = os.path.join(root, n)
            files.append((os.path.relpath(full, src), os.path.getsize(full)))
    total = sum(n for _, n in files)
    print(f"Copying {len(files)} files, {total / 1e9:.2f} GB, from {src}")
    done = 0
    for rel, n in files:
        dst = os.path.join(OUT, rel)
        # default.xex is looked up in lower case.
        if rel.lower() == "default.xex":
            dst = os.path.join(OUT, "default.xex")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.exists(dst) and os.path.getsize(dst) == n):
            tmp = dst + ".part"
            shutil.copyfile(os.path.join(src, rel), tmp)
            os.replace(tmp, dst)
        done += n
        print(f"\r  {done * 100 // max(total, 1):3d}%  {rel[:60]:60}", end="", flush=True)
    print()
    for want in ("lu0", "media", "streams0"):
        if not os.path.isdir(os.path.join(OUT, want)):
            print(f"WARNING: there's no {want} folder: the extracted disc may be incomplete.")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if os.path.isdir(sys.argv[1]):
        copy_folder(sys.argv[1])
        check_xex()
        return
    iso = sys.argv[1]
    f = open(iso, "rb")
    base = xd.find_partition(f)
    f.seek(base + 32 * xd.SECTOR + 20)
    root_sec, root_size = struct.unpack("<II", f.read(8))
    entries = []
    xd.walk(f, base, root_sec, root_size, "", entries)
    files = [e for e in entries if e[0] == "f"]
    total = sum(e[3] for e in files)
    print(f"Game partition at {base:#x}: {len(files)} files, {total / 1e9:.2f} GB")

    done = 0
    for t, p, s, n in entries:
        dst = os.path.join(OUT, p)
        if t == "d":
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst) or OUT, exist_ok=True)
        if not (os.path.exists(dst) and os.path.getsize(dst) == n):
            f.seek(base + s * xd.SECTOR)
            tmp = dst + ".part"
            with open(tmp, "wb") as o:
                left = n
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk:
                        sys.exit(f"short read: {p}")
                    o.write(chunk)
                    left -= len(chunk)
            os.replace(tmp, dst)
        done += n
        print(f"\r  {done * 100 // max(total, 1):3d}%  {p[:60]:60}", end="", flush=True)
    print()

    bad = [p for t, p, s, n in files
           if os.path.getsize(os.path.join(OUT, p)) != n]
    if bad:
        sys.exit(f"{len(bad)} files have the wrong size, e.g. {bad[:3]}")

    check_xex()


def check_xex():
    xex = open(os.path.join(OUT, "default.xex"), "rb").read(0x10000)
    assert xex[:4] == b"XEX2", "default.xex is not an XEX2 file"
    n, = struct.unpack_from(">I", xex, 20)
    hdr = {k: v for k, v in (struct.unpack_from(">II", xex, 24 + i * 8) for i in range(n))}
    tid, = struct.unpack_from(">I", xex, hdr[0x40006] + 12)
    if tid != EXPECTED_TITLE_ID:
        print(f"WARNING: title ID {tid:08X} is not WET ({EXPECTED_TITLE_ID:08X}); "
              "the config's addresses will not match.")
    else:
        print(f"OK: WET (title {tid:08X}) extracted to {OUT}")

if __name__ == "__main__":
    main()
