# mftb -> PPC_QUERY_TIMEBASE() so the host can supply a 50 MHz Xbox 360 timebase.
import sys
rec, hdr = sys.argv[1], sys.argv[2]
s = open(rec).read()
old = 'println("\\t{}.u64 = __rdtsc();", r(insn.operands[0]));'
if old in s:
    s = s.replace(old, 'println("\\t{}.u64 = PPC_QUERY_TIMEBASE();", r(insn.operands[0]));')
    open(rec, "w").write(s); print("recompiler patched")
h = open(hdr).read()
if "PPC_QUERY_TIMEBASE" not in h:
    anchor = "#ifndef PPC_CALL_INDIRECT_FUNC"
    assert h.count(anchor) == 1
    h = h.replace(anchor, "#ifndef PPC_QUERY_TIMEBASE\n#define PPC_QUERY_TIMEBASE() __rdtsc()\n#endif\n\n" + anchor)
    open(hdr, "w").write(h); print("header patched")
