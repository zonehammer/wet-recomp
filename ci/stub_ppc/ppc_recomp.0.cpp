// Exercises the constructs the real generated code relies on.
#include "ppc_recomp_shared.h"

__attribute__((alias("__imp__sub_82510000"))) PPC_WEAK_FUNC(sub_82510000);
PPC_FUNC_IMPL(__imp__sub_82510000) {
	PPC_FUNC_PROLOGUE();
	uint32_t ea{};
	PPCRegister temp{};
	// mftb
	ctx.r3.u64 = PPC_QUERY_TIMEBASE();
	// mulhd
	ctx.r4.s64 = int64_t((__int128(ctx.r3.s64) * __int128(ctx.r5.s64)) >> 64);
	// stwu / lwz
	ea = -16 + ctx.r1.u32;
	PPC_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	ctx.r6.u64 = PPC_LOAD_U32(ctx.r1.u32 + 0);
	// vector op through simde
	simde_mm_store_ps(ctx.v1.f32, simde_mm_add_ps(simde_mm_load_ps(ctx.v2.f32), simde_mm_load_ps(ctx.v3.f32)));
	temp.f32 = PPCHalfToFloat(0x3C00);
	ctx.f1.f64 = double(temp.f32);
	// import call
	ctx.lr = 0x82510030;
	__imp__KeQueryPerformanceFrequency(ctx, base);
	// switch
	switch (ctx.r3.u64 & 1) {
	case 0:
		goto loc_82510038;
	case 1:
		goto loc_82510038;
	default:
		__builtin_unreachable();
	}
loc_82510038:
	ctx.r1.u32 = ctx.r1.u32 + 16;
	// bctrl (checked indirect call)
	ctx.ctr.u64 = 0x82510040;
	PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

__attribute__((alias("__imp__sub_82510040"))) PPC_WEAK_FUNC(sub_82510040);
PPC_FUNC_IMPL(__imp__sub_82510040) {
	PPC_FUNC_PROLOGUE();
	ctx.r3.u64 = 0;
}
