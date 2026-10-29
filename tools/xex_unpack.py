#!/usr/bin/env python3
"""Decrypt (retail AES) + decompress (basic/none) an XEX2.
Writes <out>.xex (plain XEX: enc=0, comp=0) and <out>.pe (raw memory image).
Usage: xex_unpack.py default.xex out_prefix"""
import struct, subprocess, sys
RETAIL_KEY = "20B185A59D28FDC340583FBB0896BF91"

def aes(data, key_hex, mode):
    return subprocess.run(["openssl", "enc", "-d", f"-aes-128-{mode}", "-K", key_hex,
                           *(["-iv", "0"*32] if mode == "cbc" else []), "-nopad"],
                          input=data, capture_output=True, check=True).stdout

src, out = sys.argv[1], sys.argv[2]
d = bytearray(open(src, "rb").read())
assert d[:4] == b"XEX2"
_, _, pe_off, _, sec_off, n = struct.unpack_from(">4sIIIII", d, 0)
H = {k: v for k, v in (struct.unpack_from(">II", d, 24 + i*8) for i in range(n))}
ff = H[0x3FF]
ffsize, enc, comp = struct.unpack_from(">IHH", d, ff)
print(f"encryption={enc} compression={comp}")
image_size, = struct.unpack_from(">I", d, sec_off + 4)
payload = bytes(d[pe_off:])
if enc == 1:
    file_key = bytes(d[sec_off + 0x150: sec_off + 0x160])
    session = aes(file_key, RETAIL_KEY, "ecb").hex()
    payload = aes(payload[:len(payload)//16*16], session, "cbc") + payload[len(payload)//16*16:]
if comp == 0:
    image = payload
elif comp == 1:
    image = bytearray(); p = 0
    for i in range((ffsize - 8)//8):
        ds, zs = struct.unpack_from(">II", d, ff + 8 + i*8)
        image += payload[p:p+ds]; p += ds
        image += b"\0"*zs
    image = bytes(image)
else:
    raise SystemExit("LZX (normal) compression not handled by this script")
image = image[:image_size].ljust(image_size, b"\0")
assert image[:2] == b"MZ", "decryption failed (no MZ header)"
open(out + ".pe", "wb").write(image)
struct.pack_into(">IHH", d, ff, 8, 0, 0)   # mark plain
open(out + ".xex", "wb").write(bytes(d[:pe_off]) + image)
print("ok image_size", hex(image_size))
