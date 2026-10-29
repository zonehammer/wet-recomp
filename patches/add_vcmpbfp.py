# Replace upstream's debugtrap stub for vcmpbfp/vcmpbfp128 with a real implementation.
import sys
p = sys.argv[1]; s = open(p).read()
old = '''    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        println("\\t__builtin_debugtrap();");
        break;
'''
new = '''    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        // WET: bit31 = !(a <= b), bit30 = !(a >= -b); NaN sets both. Rc -> cr6.eq = all in bounds.
        printSetFlushMode(true);
        println("\\t{{ simde__m128 a_ = simde_mm_load_ps({}.f32), b_ = simde_mm_load_ps({}.f32);", v(insn.operands[1]), v(insn.operands[2]));
        println("\\t  simde__m128i le_ = simde_mm_castps_si128(simde_mm_cmple_ps(a_, b_));");
        println("\\t  simde__m128i ge_ = simde_mm_castps_si128(simde_mm_cmpge_ps(a_, simde_mm_xor_ps(b_, simde_mm_set1_ps(-0.0f))));");
        println("\\t  simde__m128i r_ = simde_mm_or_si128(simde_mm_andnot_si128(le_, simde_mm_set1_epi32(int32_t(0x80000000))), simde_mm_andnot_si128(ge_, simde_mm_set1_epi32(0x40000000)));");
        println("\\t  simde_mm_store_si128((simde__m128i*){}.u32, r_);", v(insn.operands[0]));
        if (strchr(insn.opcode->name, '.'))
        {
            println("\\t  {}.lt = 0; {}.gt = 0; {}.so = 0;", cr(6), cr(6), cr(6));
            println("\\t  {}.eq = simde_mm_testz_si128(r_, r_); }}", cr(6));
        }
        else
            println("\\t}}");
        break;
'''
if "WET: bit31" in s: print("already patched"); sys.exit(0)
assert s.count(old) == 1, "anchor not found"
s = s.replace(old, new)
open(p, "w").write(s); print("patched")
