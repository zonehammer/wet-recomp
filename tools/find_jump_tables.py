#!/usr/bin/env python3
"""Find jump tables that XenonAnalyse misses and emit XenonRecomp [[switch]] entries.

Usage: find_jump_tables.py <plain.xex> <existing WET_switch_tables.toml> [extra functions as start:size,...] > new_tables.toml

Needs a decrypted/decompressed XEX (tools/xex_unpack.py).

For every `bctr` in .text not already covered by a switch entry, it finds the
dominating bounds check (`cmplwi crN, rIdx, N` + a branch on crN.gt) and
symbolically evaluates the instructions up to the `bctr`. Recognised shapes:
  absolute:   target = u32 table[idx]
  relative:   target = base + (u8|u16 table[idx]) * scale
Anything else (function-pointer tail calls, vtables) is left alone. Every decoded
target must land inside the containing function (.pdata bounds).
"""
import re
import struct
import sys

BASE = 0x82000000


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


class Image:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert d[:4] == b"XEX2"
        pe_off, = struct.unpack_from(">I", d, 8)
        n, = struct.unpack_from(">I", d, 20)
        hdr = {k: v for k, v in (struct.unpack_from(">II", d, 24 + i * 8) for i in range(n))}
        _, enc, comp = struct.unpack_from(">IHH", d, hdr[0x3FF])
        assert enc == 0 and comp == 0, "XEX must be decrypted and decompressed"
        self.d = d[pe_off:]
        pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        nsec, = struct.unpack_from("<H", self.d, pe + 6)
        optsz, = struct.unpack_from("<H", self.d, pe + 20)
        self.sections = {}
        self.code_sections = []
        for i in range(nsec):
            o = pe + 24 + optsz + i * 40
            name = self.d[o:o + 8].rstrip(b"\0").decode()
            vsize, va = struct.unpack_from("<II", self.d, o + 8)
            chars, = struct.unpack_from("<I", self.d, o + 36)
            self.sections[name] = (BASE + va, vsize)
            if chars & 0x20000000:      # IMAGE_SCN_MEM_EXECUTE
                self.code_sections.append((BASE + va, vsize))
        pva, psz = self.sections[".pdata"]
        self.funcs = []
        for p in range(pva - BASE, pva - BASE + psz, 8):
            b, info = struct.unpack_from(">II", self.d, p)
            if b:
                self.funcs.append((b, ((info >> 8) & 0x3FFFFF) * 4))
        self.funcs.sort()

    def u32(self, a): return struct.unpack_from(">I", self.d, a - BASE)[0]
    def u16(self, a): return struct.unpack_from(">H", self.d, a - BASE)[0]
    def u8(self, a): return self.d[a - BASE]

    def load(self, size, a):
        return {1: self.u8, 2: self.u16, 4: self.u32}[size](a)

    def func_of(self, a):
        lo, hi = 0, len(self.funcs)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.funcs[mid][0] <= a:
                lo = mid + 1
            else:
                hi = mid
        if lo:
            b, n = self.funcs[lo - 1]
            if b <= a < b + n:
                return b, n
        return None


# Symbolic values: ("c", k) constant, ("i", s) idx*s, ("a", k, s) k + idx*s (an address),
# ("l", size, table, s, mult) mem[table + idx*s]*mult, ("t", size, table, s, mult, base) base + that
def evaluate(img, ranges, cmp_addr, idx_reg):
    regs = {}
    target = None
    idx_live = False    # the index is only defined from the bounds check onwards
    for a in (x for lo, hi in ranges for x in range(lo, hi, 4)):
        if a == cmp_addr:
            # Copies of the index taken before the bounds check (`mr r4,r3; cmplwi r3,N`)
            # hold the same value.
            for r, v in list(regs.items()):
                if v == ("m", idx_reg):
                    regs[r] = ("i", 1)
            regs[idx_reg] = ("i", 1)
            idx_live = True
        w = img.u32(a)
        op = w >> 26
        rD, rA, rB = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        g = regs.get

        def kill(r):
            if r == idx_reg and idx_live:
                raise ValueError("index register overwritten")
            regs.pop(r, None)

        if w == 0x60000000:                     # nop
            pass
        elif op == 14:                          # addi
            imm = sext16(w & 0xFFFF)
            v = ("c", imm) if rA == 0 else g(rA)
            if v and v[0] == "c": regs[rD] = ("c", (v[1] + (imm if rA else 0)) & 0xFFFFFFFF)
            elif v and v[0] == "a": regs[rD] = ("a", (v[1] + imm) & 0xFFFFFFFF, v[2])
            elif v and v[0] == "t": regs[rD] = v[:5] + ((v[5] + imm) & 0xFFFFFFFF,)
            else: kill(rD)
        elif op == 15:                          # addis / lis
            imm = (w & 0xFFFF) << 16
            v = ("c", 0) if rA == 0 else g(rA)
            if v and v[0] == "c": regs[rD] = ("c", (v[1] + imm) & 0xFFFFFFFF)
            else: kill(rD)
        elif op == 24:                          # ori
            v, imm = g(rD), w & 0xFFFF          # rS is in the rD field
            if imm == 0 and rD == rA: pass
            elif v and v[0] == "c": regs[rA] = ("c", v[1] | imm)
            elif v and imm == 0: regs[rA] = v
            else: kill(rA)
        elif op == 21:                          # rlwinm rA, rS, sh, mb, me
            sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
            v = g(rD)
            if v and sh and mb == 0 and me == 31 - sh:   # slwi
                if v[0] == "i": regs[rA] = ("i", v[1] << sh)
                elif v[0] == "l": regs[rA] = v[:4] + (v[4] << sh,)
                else: kill(rA)
            elif v and sh == 0 and me == 31 and v[0] in ("i", "l"):  # clrlwi on a small value
                regs[rA] = v
            else:
                kill(rA)
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo == 467 and (((w >> 16) & 31) | (((w >> 11) & 31) << 5)) == 9:   # mtctr
                target = g(rD)
            elif xo == 444 and rD == rB:        # mr rA, rS
                if g(rD) and g(rD)[0] != "m": regs[rA] = g(rD)
                elif not idx_live: regs[rA] = ("m", rD)
                else: kill(rA)
            elif xo == 266:                     # add rD, rA, rB
                x, y = g(rA), g(rB)
                if x and y and y[0] == "c": x, y = y, x
                if x and y and x[0] == "c" and y[0] == "i": regs[rD] = ("a", x[1], y[1])
                elif x and y and x[0] == "c" and y[0] == "l": regs[rD] = ("t",) + y[1:] + (x[1],)
                elif x and y and x[0] == "i" and y[0] == "i": regs[rD] = ("i", x[1] + y[1])
                else: kill(rD)
            elif xo in (87, 279, 23):           # lbzx, lhzx, lwzx
                size = {87: 1, 279: 2, 23: 4}[xo]
                x, y = g(rA) if rA else ("c", 0), g(rB)
                if x and y and y[0] == "c": x, y = y, x
                if x and y and x[0] == "c" and y[0] == "i": regs[rD] = ("l", size, x[1], y[1], 1)
                elif x and y and x[0] == "a" and y[0] == "c" and y[1] == 0: regs[rD] = ("l", size, x[1], x[2], 1)
                else: kill(rD)
            elif xo in (0, 32):                 # cmp / cmpl: no register writes
                pass
            else:
                kill(rD if xo not in (24, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 954, 922, 986, 26) else rA)
        elif op in (32, 34, 40):                # lwz/lbz/lhz rD, d(rA)
            v = g(rA)
            size = {32: 4, 34: 1, 40: 2}[op]
            if v and v[0] == "a": regs[rD] = ("l", size, (v[1] + sext16(w & 0xFFFF)) & 0xFFFFFFFF, v[2], 1)
            else: kill(rD)
        elif op in (10, 11, 16):                # cmpli / cmpi / bc
            pass
        elif op in (36, 38, 44, 37, 39, 45, 62, 54, 52, 4):  # stores / vector: no GPR result we track
            if op in (37, 39, 45):              # update forms write rA
                kill(rA)
        else:
            kill(rD)

    if not target:
        raise ValueError("ctr not derived from a table")
    if target[0] == "l" and target[1] == 4 and target[4] == 1:
        return lambda i: img.u32(target[2] + i * target[3])
    if target[0] == "t":
        _, size, table, s, mult, base = target
        return lambda i: (base + img.load(size, table + i * s) * mult) & 0xFFFFFFFF
    raise ValueError(f"unsupported shape {target[0]}")


def find_bounds_check(img, bctr, lo):
    """Nearest `cmplwi crN, rI, N` before bctr with a later bc on crN.gt."""
    a = bctr - 4
    while a >= lo and a > bctr - 0x100:
        w = img.u32(a)
        if (w >> 26) == 18 and not (w & 1) and a != bctr - 4:
            pass  # an unconditional branch inside the window is fine (block layout)
        if (w >> 26) == 10 and not ((w >> 21) & 1):     # cmpli, 32-bit
            crf, reg, n = (w >> 23) & 7, (w >> 16) & 31, w & 0xFFFF
            b = a + 4
            while b < bctr:
                x = img.u32(b)
                if (x >> 26) == 19 and ((x >> 1) & 0x3FF) == 16 and ((x >> 16) & 31) == crf * 4 + 1 \
                        and (((x >> 21) & 31) & 0x1C) == 0x0C:
                    # bgtlr: out-of-range returns; the table follows
                    return a, reg, n, None, [(max(lo, a - 0x20), bctr)]
                if (x >> 26) == 16 and ((x >> 16) & 31) == crf * 4 + 1:
                    bo = (x >> 21) & 31
                    tgt = b + sext16(x & 0xFFFC)
                    if (bo & 0x1C) == 0x04:    # ble (branch if gt false): in-range path continues at tgt
                        return a, reg, n, b + 4, [(max(lo, a - 0x20), a + 4), (tgt, bctr)]
                    if (bo & 0x1C) == 0x0C:    # bgt (branch if gt true): default at tgt, fall through to the table
                        return a, reg, n, tgt, [(max(lo, a - 0x20), bctr)]
                b += 4
        a -= 4
    return None


def main():
    img = Image(sys.argv[1])
    existing = set(int(m, 16) for m in re.findall(r"base\s*=\s*(0x[0-9A-Fa-f]+)", open(sys.argv[2]).read()))
    extra = {}
    if len(sys.argv) > 3 and sys.argv[3]:
        for item in sys.argv[3].split(","):
            s, n = item.split(":")
            extra[int(s, 16)] = int(n, 16)

    covered = set()
    # An existing entry covers the first bctr at or after its base.
    for b in existing:
        a = b
        while img.u32(a) != 0x4E800420:
            a += 4
        covered.add(a)

    found, skipped = [], 0
    for a in (x for va, sz in img.code_sections for x in range(va, va + sz, 4)):
        if img.u32(a) != 0x4E800420 or a in covered:
            continue
        fn = img.func_of(a) or next(((s, n) for s, n in extra.items() if s <= a < s + n), None)
        lo = fn[0] if fn else a - 0x100
        bc = find_bounds_check(img, a, lo)
        if not bc:
            skipped += 1
            continue
        cmp_addr, reg, n, default, ranges = bc
        try:
            fn_target = evaluate(img, ranges, cmp_addr, reg)
            labels = [fn_target(i) for i in range(n + 1)]
        except (ValueError, KeyError, struct.error):
            skipped += 1
            continue
        if fn:
            ok = all(fn[0] <= t < fn[0] + fn[1] and t % 4 == 0 for t in labels + ([default] if default else []))
        else:
            ok = all(abs(t - a) < 0x4000 and t % 4 == 0 for t in labels)
        if not ok:
            print(f"# rejected bctr 0x{a:08X}: targets leave the function", file=sys.stderr)
            continue
        found.append((a, reg, default, labels))

    print(f"# {len(found)} jump tables found by tools/find_jump_tables.py "
          f"({skipped} other bctr sites are indirect tail calls)")
    for a, reg, default, labels in found:
        print("[[switch]]")
        print(f"base = 0x{a:08X}")
        print(f"r = {reg}")
        print(f"default = 0x{(default or a + 4):08X}")
        print("labels = [")
        for l in labels:
            print(f"    0x{l:08X},")
        print("]\n")
    print(f"found {len(found)} tables, {skipped} non-table bctr sites", file=sys.stderr)


if __name__ == "__main__":
    main()
