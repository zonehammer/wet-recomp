// Field of view (fov option, horizontal degrees at 16:9; the game's own is 90).
//
// sub_82C7E590 is the game's "set camera": it copies the view (r4) and projection
// (r5) matrices into the camera/render state and derives the inverse and
// view-projection from them (found with Ctrl+F7, projscan.txt). When the FOV
// isn't the default, it gets a copy of the projection with the x and y scale
// multiplied by tan(45 deg) / tan(fov / 2), so everything derived from it (culling
// included) agrees. Scaling keeps the game's own changes (zoom while aiming)
// relative to the new FOV. Only wide perspective projections are touched: the
// square 90-degree ones are shadow and reflection cameras.
#include "options.h"
#include "ppc_context.h"

#include <cmath>
#include <cstring>

namespace
{
    float LoadF(uint8_t* base, uint32_t a)
    {
        uint32_t u = PPC_LOAD_U32(a);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }

    void StoreF(uint8_t* base, uint32_t a, float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        PPC_STORE_U32(a, u);
    }

    bool WidePerspective(uint8_t* base, uint32_t m)
    {
        static constexpr int kZero[] = { 1, 2, 3, 4, 6, 7, 12, 13, 15 };
        for (int z : kZero)
            if (LoadF(base, m + z * 4) != 0.0f)
                return false;
        float x = LoadF(base, m), y = LoadF(base, m + 20);
        float w1 = LoadF(base, m + 44), w2 = LoadF(base, m + 56);
        return x > 0.0f && y > 0.0f && y / x > 1.2f && (std::fabs(w1) == 1.0f || std::fabs(w2) == 1.0f);
    }
}

extern "C" void __imp__sub_82C7E590(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82C7E590)
{
    int fov = wet::options::Fov();
    uint32_t proj = ctx.r5.u32;
    if (fov == 90 || !proj || !WidePerspective(base, proj))
    {
        __imp__sub_82C7E590(ctx, base);
        return;
    }
    // The projection is only read by the copy at the start, so a scratch copy well
    // below the stack pointer is safe.
    uint32_t copy = (ctx.r1.u32 - 1024) & ~15u;
    for (uint32_t i = 0; i < 64; i += 4)
        PPC_STORE_U32(copy + i, PPC_LOAD_U32(proj + i));
    float scale = float(1.0 / std::tan(fov * 3.14159265358979 / 360.0));
    StoreF(base, copy, LoadF(base, copy) * scale);
    StoreF(base, copy + 20, LoadF(base, copy + 20) * scale);
    ctx.r5.u64 = copy;
    __imp__sub_82C7E590(ctx, base);
}
