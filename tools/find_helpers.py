#!/usr/bin/env python3
"""Find Xbox 360 CRT register save/restore helpers + setjmp/longjmp in a raw PE image."""
import struct, sys
BASE = 0x82000000
d = open(sys.argv[1], "rb").read()
W = lambda off: struct.unpack_from(">I", d, off)[0]

def find_seq(words, start=0x510000, end=None):
    end = end or len(d) - 4*len(words)
    pat = b"".join(struct.pack(">I", w) for w in words)
    res, i = [], d.find(pat, start, end)
    while i != -1:
        if i % 4 == 0: res.append(BASE + i)
        i = d.find(pat, i + 4, end)
    return res

r = {}
# std r14,-0x98(r1); std r15,-0x90(r1)
r["savegprlr_14_address"] = find_seq([0xF9C1FF68, 0xF9E1FF70])
# ld  r14,-0x98(r1); ld r15,-0x90(r1)
r["restgprlr_14_address"] = find_seq([0xE9C1FF68, 0xE9E1FF70])
# stfd f14,-0x90(r12); stfd f15,-0x88(r12)
r["savefpr_14_address"]  = find_seq([0xD9CCFF70, 0xD9ECFF78])
r["restfpr_14_address"]  = find_seq([0xC9CCFF70, 0xC9ECFF78])
# li r11,-0x120; stvx v14,r11,r12
r["savevmx_14_address"]  = find_seq([0x3960FEE0, 0x7DCB61CE])
r["restvmx_14_address"]  = find_seq([0x3960FEE0, 0x7DCB60CE])
# li r11,-0x400 ; (vmx128 store/load follows)
li400 = find_seq([0x3960FC00])
r["savevmx_64_address"] = [a for a in li400 if (W(a-BASE+4) >> 26) == 4 and (W(a-BASE+4) & 0x3F3) == 0x1C3]
r["restvmx_64_address"] = [a for a in li400 if (W(a-BASE+4) >> 26) == 4 and (W(a-BASE+4) & 0x3F3) == 0x0C3]
r["_li_r11_-0x400_all"] = li400
for k, v in r.items():
    print(f"{k:24} {', '.join(hex(x) for x in v[:6])}{' ...' if len(v)>6 else ''}")
