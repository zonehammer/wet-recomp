# Adds PPC instructions used by WET that upstream XenonRecomp does not implement.
import sys, re
p = sys.argv[1]
s = open(p).read()
if "WET_EXTRA_INSTRUCTIONS" in s:
    print("already patched"); sys.exit(0)

NEW = r'''
    // ---- WET_EXTRA_INSTRUCTIONS (added for WET recomp) ----
#define WET_LOAD_U(name, expr) \
    case name: \
        println("\t{} = {} + {}.u32;", ea(), int32_t(insn.operands[1]), r(insn.operands[2])); \
        expr; \
        println("\t{}.u32 = {};", r(insn.operands[2]), ea()); \
        break;
#define WET_LOAD_UX(name, expr) \
    case name: \
        println("\t{} = {}.u32 + {}.u32;", ea(), r(insn.operands[1]), r(insn.operands[2])); \
        expr; \
        println("\t{}.u32 = {};", r(insn.operands[1]), ea()); \
        break;

    WET_LOAD_U(PPC_INST_LHZU, println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea()))
    WET_LOAD_U(PPC_INST_LHAU, println("\t{}.s64 = int16_t(PPC_LOAD_U16({}));", r(insn.operands[0]), ea()))
    WET_LOAD_U(PPC_INST_LFSU, (printSetFlushMode(false), println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea()), println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp())))
    WET_LOAD_U(PPC_INST_LFDU, (printSetFlushMode(false), println("\t{}.u64 = PPC_LOAD_U64({});", f(insn.operands[0]), ea())))
    WET_LOAD_U(PPC_INST_STHU, println("\tPPC_STORE_U16({}, {}.u16);", ea(), r(insn.operands[0])))
    WET_LOAD_U(PPC_INST_STFSU, (printSetFlushMode(false), println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0])), println("\tPPC_STORE_U32({}, {}.u32);", ea(), temp())))
    WET_LOAD_U(PPC_INST_STFDU, (printSetFlushMode(false), println("\tPPC_STORE_U64({}, {}.u64);", ea(), f(insn.operands[0]))))

    WET_LOAD_UX(PPC_INST_LBZUX, println("\t{}.u64 = PPC_LOAD_U8({});", r(insn.operands[0]), ea()))
    WET_LOAD_UX(PPC_INST_LHZUX, println("\t{}.u64 = PPC_LOAD_U16({});", r(insn.operands[0]), ea()))
    WET_LOAD_UX(PPC_INST_LHAUX, println("\t{}.s64 = int16_t(PPC_LOAD_U16({}));", r(insn.operands[0]), ea()))
    WET_LOAD_UX(PPC_INST_LWZUX, println("\t{}.u64 = PPC_LOAD_U32({});", r(insn.operands[0]), ea()))
    WET_LOAD_UX(PPC_INST_LFSUX, (printSetFlushMode(false), println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea()), println("\t{}.f64 = double({}.f32);", f(insn.operands[0]), temp())))
    WET_LOAD_UX(PPC_INST_LFDUX, (printSetFlushMode(false), println("\t{}.u64 = PPC_LOAD_U64({});", f(insn.operands[0]), ea())))
    WET_LOAD_UX(PPC_INST_STBUX, println("\tPPC_STORE_U8({}, {}.u8);", ea(), r(insn.operands[0])))
    WET_LOAD_UX(PPC_INST_STHUX, println("\tPPC_STORE_U16({}, {}.u16);", ea(), r(insn.operands[0])))
    WET_LOAD_UX(PPC_INST_STDUX, println("\tPPC_STORE_U64({}, {}.u64);", ea(), r(insn.operands[0])))
    WET_LOAD_UX(PPC_INST_STFSUX, (printSetFlushMode(false), println("\t{}.f32 = float({}.f64);", temp(), f(insn.operands[0])), println("\tPPC_STORE_U32({}, {}.u32);", ea(), temp())))
#undef WET_LOAD_U
#undef WET_LOAD_UX

    case PPC_INST_BDZF:
    case PPC_INST_BDNZT:
    {
        static const char* bits[] = { "lt", "gt", "eq", "so" };
        const bool dz = (id == PPC_INST_BDZF);
        println("\t--{}.u64;", ctr());
        println("\tif ({}.u32 {} 0 && {}{}.{}) goto loc_{:X};", ctr(), dz ? "==" : "!=", dz ? "!" : "", cr(insn.operands[0] / 4), bits[insn.operands[0] % 4], insn.operands[1]);
        break;
    }

    case PPC_INST_EQV:
        println("\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDC:
        println("\t{}.u8 = uint32_t({}.u32 + {}.u32) < {}.u32;", temp(), r(insn.operands[1]), r(insn.operands[2]), r(insn.operands[1]));
        println("\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_ADDME:
        println("\t{}.u8 = ({}.u32 != 0) | {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\t{}.u64 = {}.u64 + {}.ca - 1;", r(insn.operands[0]), r(insn.operands[1]), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_SUBFZE:
        println("\t{}.u8 = ({}.u32 == 0) & {}.ca;", temp(), r(insn.operands[1]), xer());
        println("\t{}.u64 = ~{}.u64 + {}.ca;", r(insn.operands[0]), r(insn.operands[1]), xer());
        println("\t{}.ca = {}.u8;", xer(), temp());
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_MULHD:
        println("\t{}.s64 = int64_t((__int128({}.s64) * __int128({}.s64)) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        break;

    case PPC_INST_DCBST:
        // no op
        break;

    case PPC_INST_FRSQRTE:
        printSetFlushMode(false);
        println("\t{}.f64 = 1.0 / sqrt({}.f64);", f(insn.operands[0]), f(insn.operands[1]));
        break;

    case PPC_INST_VPKSWSS:
    case PPC_INST_VPKSWSS128:
        println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), simde_mm_load_si128((simde__m128i*){}.s32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VCMPGTSW:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_cmpgt_epi32(simde_mm_load_si128((simde__m128i*){}.s32), simde_mm_load_si128((simde__m128i*){}.s32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
        break;

    case PPC_INST_VSUBSHS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_subs_epi16(simde_mm_load_si128((simde__m128i*){}.s16), simde_mm_load_si128((simde__m128i*){}.s16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VADDSWS:
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.s64 = int64_t({}.s32[{}]) + int64_t({}.s32[{}]);", temp(), v(insn.operands[1]), i, v(insn.operands[2]), i);
            println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;", v(insn.operands[0]), i, temp(), temp(), temp());
        }
        break;

    case PPC_INST_VCTUXS:
    case PPC_INST_VCFPUXWS128:
        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.f64 = double({}.f32[{}]) * {}.0;", temp(), v(insn.operands[1]), i, 1u << insn.operands[2]);
            println("\t{}.u32[{}] = ({}.f64 != {}.f64 || {}.f64 <= 0.0) ? 0u : {}.f64 >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t({}.f64);", v(insn.operands[0]), i, temp(), temp(), temp(), temp(), temp());
        }
        break;
    // ---- end WET_EXTRA_INSTRUCTIONS ----

    case PPC_INST_ADD:
'''
anchor = "\n    case PPC_INST_ADD:\n"
assert s.count(anchor) == 1
s = s.replace(anchor, NEW, 1)
s = s.replace("    case PPC_INST_VSEL:\n", "    case PPC_INST_VSEL:\n    case PPC_INST_VSEL128:\n", 1)
s = s.replace("    case PPC_INST_LVEWX128:\n", "    case PPC_INST_LVEWX128:\n    case PPC_INST_LVEHX:\n", 1)
open(p, "w").write(s)
print("patched")
