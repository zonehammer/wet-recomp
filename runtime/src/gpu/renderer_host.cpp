// Kernel-side services for the renderer (see renderer.h): guest-capable host
// threads, interrupt delivery and physical-memory write watches.
#include "gpu/renderer.h"
#include "kernel/kernel.h"
#include "kernel/threads.h"
#include "log.h"
#include "memory.h"
#include "os.h"

namespace wet::renderer_host
{
    void StartThread(void (*entry)(void*), void* arg, const char*)
    {
        if (!os::StartThread(entry, arg, 16 << 20))
            LogFatal("Renderer: could not start a thread");
    }

    void* AdoptThread(const char* name)
    {
        PPCContext& ctx = AdoptHostThread(0x40000);
        SetThreadName(0xFFFFFFFF, std::string("GPU: ") + (name && *name ? name : "worker"));
        return &ctx;
    }

    uint64_t CallInterrupt(void* p, uint32_t address, const uint64_t* args, size_t count)
    {
        auto& ctx = *static_cast<PPCContext*>(p);
        GuestThread* t = CurrentThread();
        PPCContext saved = ctx;
        ctx.r1.u64 = t ? t->stackBase - 0x80 : saved.r1.u64 - 0x100;
        if (t)
            ctx.r13.u64 = t->pcr;
        PPCRegister* regs[] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
        for (size_t i = 0; i < count && i < 8; i++)
            regs[i]->u64 = args[i];
        SetCurrentImport(args && count && args[0] == 0 ? "(vblank interrupt)" : "(graphics interrupt)");
        CallGuest(ctx, address);
        uint64_t r = ctx.r3.u64;
        ctx = saved;
        return r;
    }

    void SetActiveCpu(void*, uint32_t cpu)
    {
        if (GuestThread* t = CurrentThread())
            St8(t->pcr + 0x10C, uint8_t(cpu));
    }

    void Log(const char* text) { wet::Log("%s", text); }
    void ExitProcess() { LogFlush(); os::ExitProcess(0); }

    uint8_t* PhysicalHostView() { return wet::PhysicalHostView(); }
    void WatchPhysical(uint32_t physStart, uint32_t length) { wet::WatchPhysical(physStart, length); }
    void SetPhysicalWriteCallback(void (*callback)(uint32_t, uint32_t)) { wet::SetPhysicalWriteCallback(callback); }
    void UnwatchPhysical(uint32_t start, uint32_t length) { wet::UnwatchPhysical(start, length); }
}
