#!/usr/bin/env python3
"""List the kernel/XAM imports of a *plain* (decrypted, uncompressed) XEX.

Usage: dump_imports.py <plain.xex> <xboxkrnl_table.inc> <xam_table.inc> [out.json]
Prints a summary; writes JSON with one entry per import:
  {"lib", "ordinal", "name", "kind": "function"|"variable", "slot": guest address}
For functions, `slot` is the thunk address; for variables it is the 32-bit slot
the loader must fill with a guest pointer to the variable.
"""
import json, re, struct, sys

def load_table(path):
    t = {}
    for m in re.finditer(r"XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)", open(path).read()):
        t[int(m[1], 16)] = (m[2], "variable" if m[3] == "kVariable" else "function")
    return t

def main():
    xex, krnl, xam = sys.argv[1:4]
    tables = {"xboxkrnl.exe": load_table(krnl), "xam.xex": load_table(xam)}
    d = open(xex, "rb").read()
    assert d[:4] == b"XEX2"
    pe_off, = struct.unpack_from(">I", d, 8)
    n, = struct.unpack_from(">I", d, 20)
    H = {k: v for k, v in (struct.unpack_from(">II", d, 24 + i * 8) for i in range(n))}
    ff = H[0x3FF]
    _, enc, comp = struct.unpack_from(">IHH", d, ff)
    assert enc == 0 and comp == 0, "XEX must be decrypted and decompressed (run xex_unpack.py)"
    image = d[pe_off:]
    base, = struct.unpack_from(">I", d, H[0x10201]) if (H[0x10201] & 0xFF) == 0xFF else (H[0x10201],)
    il = H[0x103FF]
    _, strsz, nlibs = struct.unpack_from(">III", d, il)
    names, p = [], il + 12
    raw = d[p:p + strsz]
    off = 0
    for _ in range(nlibs):
        s = raw[off:raw.index(b"\0", off)].decode()
        names.append(s)
        off += ((len(s) + 1) + 3) & ~3
    p += strsz
    out = []
    for _ in range(nlibs):
        size, = struct.unpack_from(">I", d, p)
        name_idx, count = struct.unpack_from(">HH", d, p + 36)
        lib = names[name_idx]
        for i in range(count):
            slot, = struct.unpack_from(">I", d, p + 40 + i * 4)
            word, = struct.unpack_from(">I", image, slot - base)
            typ, ordinal = word >> 24, word & 0xFFFF
            nm, kind = tables[lib].get(ordinal, (f"{lib.split('.')[0]}_{ordinal:03X}", "function"))
            if typ == 0:   # import-address-table entry (data slot)
                if kind == "variable":
                    out.append(dict(lib=lib, ordinal=ordinal, name=nm, kind="variable", slot=slot))
            else:          # code thunk
                out.append(dict(lib=lib, ordinal=ordinal, name=nm, kind="function", slot=slot))
        p += size
    funcs = [e for e in out if e["kind"] == "function"]
    vars_ = [e for e in out if e["kind"] == "variable"]
    print(f"{len(funcs)} functions, {len(vars_)} variables")
    for v in vars_:
        print(f"  variable {v['lib']}!{v['name']} slot 0x{v['slot']:08X}")
    if len(sys.argv) > 4:
        json.dump(out, open(sys.argv[4], "w"), indent=1)

if __name__ == "__main__":
    main()
