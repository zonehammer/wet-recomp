// PlayStation button prompts.
//
// WET's menus (Scaleform movies) were shared with the PS3 version: they carry
// the PlayStation button art and a console switch (CApplication.consoleType,
// "X360" or "PS3", passed to CGamePad.setConfig and set again through
// UpdateConsole). The game's scripts learn the console from the script
// function GetCurrentPlateform (sub_82782A90), which returns the string
// "X360" through sub_82D52E20 (push a string result). When PlayStation prompts
// are wanted, that call gets "PS3" (the tail of the engine's own
// "ePLATFORM_PS3" string) instead, so the menus switch to their PS3 set.
#include "input.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

namespace
{
    constexpr uint32_t kPlatformReturn = 0x82782AD0;   // lr of the string push in GetCurrentPlateform
    constexpr uint32_t kStringPs3 = 0x8229F29E;        // "PS3" ("ePLATFORM_PS3" + 10)

    bool WantPlayStation()
    {
        switch (wet::options::ButtonPrompts())
        {
        case 1: return false;
        case 2: return true;
        default: return wet::PlayStationControllerConnected();
        }
    }
}

extern "C" void __imp__sub_82D52E20(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82D52E20)
{
    if (uint32_t(ctx.lr) == kPlatformReturn)
    {
        bool ps = WantPlayStation();
        static int last = -1;
        if (int(ps) != last)
        {
            last = ps;
            wet::Log("Prompts: the game asked for the platform - %s button prompts", ps ? "PlayStation" : "Xbox");
        }
        if (ps && PPC_LOAD_U32(kStringPs3) == 0x50533300u)   // "PS3\0"
            ctx.r4.u64 = kStringPs3;
    }
    __imp__sub_82D52E20(ctx, base);
}
