# blrl, vupkd3d128 FLOAT16_2/FLOAT16_4, vpkd3d128 FLOAT16_2 (pack=1) for WET.
import sys
rec, hdr = sys.argv[1], sys.argv[2]
s = open(rec).read()
if "WET_D3D_HALF" not in s:
    old_blrl = '''    case PPC_INST_BLRL:
        println("__builtin_debugtrap();");
        break;
'''
    new_blrl = '''    case PPC_INST_BLRL:
        // WET_D3D_HALF: indirect call through LR
        println("\\t{}.u32 = ctx.lr;", temp());
        if (!config.skipLr)
            println("\\tctx.lr = 0x{:X};", base + 4);
        println("\\tPPC_CALL_INDIRECT_FUNC({}.u32);", temp());
        csrState = CSRState::Unknown;
        break;
'''
    assert s.count(old_blrl) == 1; s = s.replace(old_blrl, new_blrl)

    # vupkd3d128: insert FLOAT16_2 (3) and FLOAT16_4 (5) before its default
    anchor = '''            println("\\t{}.f32[0] = 1.0f;", vTemp());
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        default:
            println("\\t__builtin_debugtrap();");'''
    add = '''            println("\\t{}.f32[0] = 1.0f;", vTemp());
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 3: // float16_2 -> (x, y, 0, 1); BE halfwords 6,7 live in u16[1],u16[0]
            println("\\t{}.f32[3] = PPCHalfToFloat({}.u16[1]);", vTemp(), v(insn.operands[1]));
            println("\\t{}.f32[2] = PPCHalfToFloat({}.u16[0]);", vTemp(), v(insn.operands[1]));
            println("\\t{}.f32[1] = 0.0f;", vTemp());
            println("\\t{}.f32[0] = 1.0f;", vTemp());
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 5: // float16_4 -> (x, y, z, w); BE halfwords 4..7 live in u16[3..0]
            for (size_t i = 0; i < 4; i++)
                println("\\t{}.f32[{}] = PPCHalfToFloat({}.u16[{}]);", vTemp(), 3 - i, v(insn.operands[1]), 3 - i);
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        default:
            println("\\t__builtin_debugtrap();");'''
    assert s.count(anchor) == 1; s = s.replace(anchor, add)

    # vpkd3d128: FLOAT16_2 (type 3) with VPACK_32 (pack 1): dest BE word (3-shift) = half(x)<<16 | half(y)
    anchor2 = '''        case 5: // float16_4
            if (insn.operands[3] != 2 || insn.operands[4] > 2)'''
    add2 = '''        case 3: // float16_2 (WET)
            if (insn.operands[3] != 1)
            {
                fmt::println("Unexpected float16_2 pack instruction at {:X}", base);
                println("\\t__builtin_debugtrap();");
                break;
            }
            println("\\t{}.u32 = (uint32_t(PPCFloatToHalf({}.f32[3])) << 16) | PPCFloatToHalf({}.f32[2]);", temp(), v(insn.operands[1]), v(insn.operands[1]));
            println("\\t{}.u32[{}] = {}.u32;", v(insn.operands[0]), insn.operands[4], temp());
            break;

''' + anchor2
    assert s.count(anchor2) == 1; s = s.replace(anchor2, add2)
    open(rec, "w").write(s); print("recompiler patched")

h = open(hdr).read()
if "PPCHalfToFloat" not in h:
    helpers = '''
// ---- WET: half-float helpers used by vpkd3d128/vupkd3d128 ----
inline float PPCHalfToFloat(uint16_t h)
{
    uint32_t s = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (e == 0)
    {
        if (m == 0) f = s;
        else { e = 113; while (!(m & 0x400)) { m <<= 1; --e; } f = s | (e << 23) | ((m & 0x3FF) << 13); }
    }
    else if (e == 31) f = s | 0x7F800000 | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

// Saturating float->half matching the existing float16_4 pack path (truncate, clamp to 0x7FFF).
inline uint16_t PPCFloatToHalf(float x)
{
    uint32_t u; memcpy(&u, &x, 4);
    uint16_t sign = uint16_t((u & 0x80000000) >> 16);
    uint32_t a = u & 0x7FFFFFFF; float af; memcpy(&af, &a, 4);
    if (af != af || af > 65504.0f) return sign | 0x7FFF;
    uint32_t e = (a & 0x7F800000) >> 23, m = (a & 0x7FE000) >> 13;
    if (e > 0x70) return sign | uint16_t(((e - 0x70) << 10) + m);
    uint32_t sh = 0x71 - e;
    return sign | uint16_t(sh > 31 ? 0 : ((0x400 + m) >> sh));
}
'''
    anchor = "union alignas(0x10) PPCVRegister"
    assert h.count(anchor) == 1
    h = h.replace(anchor, helpers + "\n" + anchor)
    open(hdr, "w").write(h); print("header patched")
