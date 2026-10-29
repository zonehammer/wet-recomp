// Stand-in GPU: just enough of the Xenos interface for the game to get past
// graphics initialisation and into its frame loop. Nothing is drawn.
//
// - GPU registers live at 0x7FC80000 (plain memory, see memory.h).
// - A command processor walks the PM4 packets the game submits and performs the
//   bookkeeping ones: register writes (with scratch-register write-back),
//   MEM_WRITE / EVENT_WRITE_SHD / COND_WRITE fences, WAIT_REG_MEM (the CP
//   blocks until the CPU sets a flag), INDIRECT_BUFFER and INTERRUPT. Everything
//   else (draws, state) is skipped by length. CP_RB_RPTR and the read-pointer
//   write-back then follow CP_RB_WPTR.
// - A 60 Hz vertical-blank interrupt is delivered to the game's graphics
//   interrupt callback (source 0), on a dedicated thread with its own guest stack.
//
// TODO (real GPU work): execute draws and state, translate rendering to D3D12/Vulkan.
#include "kernel.h"
#include "os.h"
#include "threads.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace wet;

namespace
{
    constexpr uint32_t kRegCpRbRptr = 0x01C4;
    constexpr uint32_t kRegCpRbWptr = 0x01C5;
    constexpr uint32_t kRegScratchUmsk = 0x01DC;
    constexpr uint32_t kRegScratchAddr = 0x01DD;
    constexpr uint32_t kRegScratchReg0 = 0x0578;

    std::atomic<uint32_t> s_interruptCallback{ 0 }, s_interruptUser{ 0 };
    std::atomic<uint32_t> s_ringBase{ 0 }, s_ringSizeLog2{ 0 }, s_rptrWriteBack{ 0 };
    std::atomic<uint64_t> s_vblanks{ 0 }, s_swaps{ 0 };
    std::once_flag s_gpuStarted;

    uint32_t Reg(uint32_t index) { return Ld32(kGpuRegisterBase + index * 4); }
    void SetReg(uint32_t index, uint32_t v) { St32(kGpuRegisterBase + index * 4, v); }

    constexpr uint64_t kVblankNs = 16666667;
    uint64_t s_nextVblank = 0;          // GPU thread only
    PPCContext* s_gpuCtx = nullptr;     // the GPU thread's guest context
    GuestThread* s_gpuThread = nullptr;

    // Runs the title's graphics interrupt callback as if on hardware thread `cpu`
    // (the handler finds per-CPU state through the PCR's current-processor byte).
    void DeliverInterrupt(uint32_t source, uint32_t cpu = 2)
    {
        uint32_t cb = s_interruptCallback.load();
        if (!cb || !s_gpuCtx)
            return;
        PPCContext& ctx = *s_gpuCtx;
        St8(s_gpuThread->pcr + 0x10C, uint8_t(cpu));
        ctx.r1.u64 = s_gpuThread->stackBase - 0x80;
        ctx.r13.u64 = s_gpuThread->pcr;
        ctx.r3.u64 = source;
        ctx.r4.u64 = s_interruptUser.load();
        SetCurrentImport(source == 0 ? "(vblank interrupt)" : "(command processor interrupt)");
        CallGuest(ctx, cb);
    }

    // Xenos endian swap modes (low 2 bits of many GPU addresses).
    uint32_t GpuSwap(uint32_t v, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);     // 8-in-16
        case 2: return __builtin_bswap32(v);                                   // 8-in-32
        case 3: return (v << 16) | (v >> 16);                                  // 16-in-32
        default: return v;
        }
    }

    // The GPU reads memory little-endian, then applies the endian mode encoded in the
    // low 2 bits of the address it was given.
    uint32_t GpuLoad(uint32_t guest, uint32_t endian)
    {
        uint32_t raw;
        std::memcpy(&raw, HostPtr(guest), 4);
        return GpuSwap(raw, endian);
    }

    void GpuStore(uint32_t guest, uint32_t value, uint32_t endian)
    {
        uint32_t raw = GpuSwap(value, endian);
        std::memcpy(HostPtr(guest), &raw, 4);
    }

    // GPU (physical) address -> guest address; 0 if unmapped.
    uint32_t GpuAddr(uint32_t a)
    {
        return PhysicalToGuest(a & 0x1FFFFFFF);
    }

    void ServiceVblank(); // keeps vblank interrupts flowing while the CP is blocked

    bool Compare(uint32_t func, uint32_t value, uint32_t ref)
    {
        switch (func & 7)
        {
        case 0: return false;
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;
        }
    }

    // ---------------------------------------------------------------------
    // Command processor
    // ---------------------------------------------------------------------
    struct CommandProcessor
    {
        uint32_t packetsSeen[128] = {};
        bool warnedUnmapped = false;

        uint32_t scratchSeen[8] = {};

        void ScratchWriteBack(uint32_t n, uint32_t value)
        {
            scratchSeen[n] = value;
            if (Reg(kRegScratchUmsk) & (1u << n))
            {
                uint32_t mem = GpuAddr(Reg(kRegScratchAddr) + n * 4);
                if (mem)
                    St32(mem, value);
            }
        }

        // The CPU also writes scratch registers directly through MMIO (e.g. to
        // acknowledge a command-processor interrupt). The register file is plain
        // memory here, so notice those writes and perform the hardware's write-back.
        void SyncScratchFromCpu()
        {
            for (uint32_t n = 0; n < 8; n++)
            {
                uint32_t v = Reg(kRegScratchReg0 + n);
                if (v != scratchSeen[n])
                    ScratchWriteBack(n, v);
            }
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= 0x10000)
                return;
            SetReg(index, value);
            if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8)
                ScratchWriteBack(index - kRegScratchReg0, value);
        }

        // Reads dwords from guest memory (big-endian), handling ring wrap.
        struct Reader
        {
            uint32_t base, sizeDwords, pos; // pos in dwords
            uint32_t Read() { uint32_t v = Ld32(base + (pos % sizeDwords) * 4); pos++; return v; }
        };

        uint32_t overruns = 0;

        void Execute(Reader& r, uint32_t endPos, int depth)
        {
            while (r.pos < endPos)
            {
                uint32_t header = r.Read();
                switch (header >> 30)
                {
                case 0: // type 0: register writes
                {
                    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    uint32_t base = header & 0x7FFF;
                    bool sameReg = (header >> 15) & 1;
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegister(sameReg ? base : base + i, r.Read());
                    break;
                }
                case 1: // type 1: two registers
                {
                    uint32_t a = r.Read(), b = r.Read();
                    WriteRegister(header & 0x7FF, a);
                    WriteRegister((header >> 11) & 0x7FF, b);
                    break;
                }
                case 2: // type 2: no-op
                    break;
                case 3:
                    ExecuteType3(r, header, depth);
                    break;
                }
            }
            if (r.pos > endPos && overruns++ < 5)
            {
                Log("GPU: a packet ran %u dwords past the end of its %s (depth %d)", r.pos - endPos,
                    depth ? "indirect buffer" : "ring segment", depth);
                if (depth && std::getenv("WET_GPU_TRACE"))
                {
                    // Re-walk the buffer printing each packet header.
                    Reader w{ r.base, r.sizeDwords, 0 };
                    Log("GPU TRACE: buffer at %08X, %u dwords:", r.base, endPos);
                    while (w.pos < endPos)
                    {
                        uint32_t at = w.pos, h = w.Read(), n;
                        switch (h >> 30)
                        {
                        case 0: n = ((h >> 16) & 0x3FFF) + 1; break;
                        case 1: n = 2; break;
                        case 2: n = 0; break;
                        default: n = ((h >> 16) & 0x3FFF) + 1; break;
                        }
                        char words[200] = "";
                        for (uint32_t k = 0; k < n && k < 6; k++)
                            std::snprintf(words + strlen(words), sizeof(words) - strlen(words), " %08X", Ld32(r.base + (at + 1 + k) * 4));
                        Log("   +%04X %08X type%u op%02X n=%u:%s", at, h, h >> 30, (h >> 8) & 0x7F, n, words);
                        w.pos += n;
                    }
                }
            }
        }

        struct Hist { uint32_t addr, header; int depth; };
        Hist hist[16] = {};
        uint32_t histN = 0;
        bool dumped = false;
        struct IbHist { uint32_t from, guest, size; int depth; };
        IbHist ibHist[8] = {};
        uint32_t ibHistN = 0;

        void ExecuteType3(Reader& r, uint32_t header, int depth)
        {
            uint32_t opcode = (header >> 8) & 0x7F;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t start = r.pos;
            hist[histN++ % 16] = { r.base + ((start - 1) % r.sizeDwords) * 4, header, depth };
            if (!dumped && count > 4096 && std::getenv("WET_GPU_TRACE"))
            {
                dumped = true;
                Log("GPU TRACE: suspicious packet; last packets (guest address, header, depth):");
                for (uint32_t i = histN > 16 ? histN - 16 : 0; i < histN; i++)
                    Log("   %08X  %08X  d%d", hist[i % 16].addr, hist[i % 16].header, hist[i % 16].depth);
                Log("GPU TRACE: last indirect buffers (from, start, dwords, end, depth); reader base %08X pos %u size %u:",
                    r.base, start, r.sizeDwords);
                for (uint32_t i = ibHistN > 8 ? ibHistN - 8 : 0; i < ibHistN; i++)
                    Log("   %08X -> %08X  %5u  %08X  d%d", ibHist[i % 8].from, ibHist[i % 8].guest, ibHist[i % 8].size,
                        ibHist[i % 8].guest + ibHist[i % 8].size * 4, ibHist[i % 8].depth);
            }
            if (packetsSeen[opcode]++ == 0)
                Log("GPU: first PM4 packet 0x%02X (%u dwords)", opcode, count);

            switch (opcode)
            {
            case 0x3F: // INDIRECT_BUFFER
            case 0x37: // INDIRECT_BUFFER_PFD
            {
                uint32_t addr = r.Read(), rawSize = r.Read(), size = rawSize & 0xFFFFF;
                uint32_t guest = GpuAddr(addr);
                static int ibLogged = 0;
                ibHist[ibHistN++ % 8] = { r.base + ((start - 1) % r.sizeDwords) * 4, guest, size, depth };
                if (std::getenv("WET_GPU_TRACE") && ibLogged++ < 0)
                    Log("GPU TRACE: IB from %08X: addr %08X -> %08X, raw size %08X (%u dwords, ends %08X), depth %d",
                        r.base + ((start - 1) % r.sizeDwords) * 4, addr, guest, rawSize, size, guest + size * 4, depth);
                if (guest && depth < 4)
                {
                    Reader ib{ guest, 0xFFFFFFFF, 0 };
                    Execute(ib, size, depth + 1);
                }
                else if (!guest && !warnedUnmapped)
                {
                    warnedUnmapped = true;
                    Log("GPU: indirect buffer at unmapped GPU address 0x%08X", addr);
                }
                break;
            }
            case 0x3D: // MEM_WRITE: address, then values
            {
                uint32_t raw = r.Read(), addr = raw & ~3u;
                for (uint32_t i = 1; i < count; i++)
                {
                    uint32_t v = r.Read();
                    if (uint32_t g = GpuAddr(addr + (i - 1) * 4))
                        GpuStore(g, v, raw & 3);
                }
                break;
            }
            case 0x58: // EVENT_WRITE_SHD: initiator, address, value
            {
                uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
                uint32_t data = (initiator >> 31) & 1 ? uint32_t(s_swaps.load()) : value;
                if (uint32_t g = GpuAddr(addr & ~3u))
                    GpuStore(g, data, addr & 3);
                break;
            }
            case 0x45: // COND_WRITE: poll info, poll addr/reg, ref, mask, write addr/reg, value
            {
                uint32_t info = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read();
                uint32_t writeAddr = r.Read(), value = r.Read();
                uint32_t pollMem = (info & 0x10) ? GpuAddr(pollAddr & ~3u) : 0;
                uint32_t cur = (info & 0x10) ? (pollMem ? GpuLoad(pollMem, pollAddr & 3) : 0) : Reg(pollAddr & 0xFFFF);
                bool ok = Compare(info, cur & mask, ref);
                if (ok)
                {
                    if (info & 0x100) { if (uint32_t g = GpuAddr(writeAddr & ~3u)) GpuStore(g, value, writeAddr & 3); }
                    else WriteRegister(writeAddr & 0xFFFF, value);
                }
                break;
            }
            case 0x3C: // WAIT_REG_MEM: info, poll addr/reg, ref, mask, interval
            {
                uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read();
                r.Read();
                uint32_t mem = (info & 0x10) ? GpuAddr(poll & ~3u) : 0;
                if ((info & 0x10) && !mem)
                    break;
                uint64_t startNs = os::MonotonicNs();
                bool warned = false;
                for (;;)
                {
                    uint32_t v = (info & 0x10) ? GpuLoad(mem, poll & 3) : Reg(poll & 0xFFFF);
                    if (Compare(info, v & mask, ref))
                        break;
                    if (!warned && os::MonotonicNs() - startNs > 5000000000ull)
                    {
                        warned = true;
                        Log("GPU: WAIT_REG_MEM still waiting after 5 s for %s %08X & %08X %s %08X (now %08X)",
                            (info & 0x10) ? "memory" : "register", (info & 0x10) ? mem : (poll & 0xFFFF), mask,
                            (const char*[]){ "never", "<", "<=", "==", "!=", ">=", ">", "always" }[info & 7], ref, v & mask);
                        {
                            uint32_t from = hist[(histN - 6) % 16].addr;
                            for (uint32_t a = from; a < hist[(histN - 1) % 16].addr + 0x18; a += 16)
                                Log("   raw %08X: %08X %08X %08X %08X", a, Ld32(a), Ld32(a + 4), Ld32(a + 8), Ld32(a + 12));
                        }
                        for (uint32_t i = histN > 16 ? histN - 16 : 0; i < histN; i++)
                        {
                            uint32_t a = hist[i % 16].addr, h = hist[i % 16].header;
                            Log("   %08X  %08X  d%d  args %08X %08X %08X %08X", a, h, hist[i % 16].depth,
                                Ld32(a + 4), Ld32(a + 8), Ld32(a + 12), Ld32(a + 16));
                        }
                    }
                    ServiceVblank();
                    SyncScratchFromCpu();
                    os::SleepNs(50000);
                }
                break;
            }
            case 0x21: // REG_RMW: reg, and mask, or mask
            {
                uint32_t reg = r.Read() & 0x1FFF, andMask = r.Read(), orMask = r.Read();
                WriteRegister(reg, (Reg(reg) & andMask) | orMask);
                break;
            }
            case 0x54: // INTERRUPT: one command-processor interrupt per CPU in the mask
            {
                uint32_t mask = r.Read();
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                    if (mask & (1u << cpu))
                        DeliverInterrupt(1, cpu);
                break;
            }
            default:
                break;  // draws, state, waits: skipped
            }
            r.pos = start + count; // always honour the packet length
        }
    };

    CommandProcessor s_cp;

    void GpuThreadMain(void*)
    {
        PPCContext& ctx = AdoptHostThread(0x40000);
        s_gpuCtx = &ctx;
        s_gpuThread = CurrentThread();
        SetThreadName(0xFFFFFFFF, "GPU (stand-in)");
        Log("GPU stand-in started (guest thread %u): PM4 bookkeeping + 60 Hz vblank", s_gpuThread->id);

        // Display-controller registers the title reads (values as reported by Xenia).
        SetReg(0x1951, 1);            // D1MODE_VBLANK_STATUS: vblank interrupt pending/enabled
        SetReg(0x1961, 0x050002D0);   // D1MODE_VIEWPORT_SIZE: 1280x720
        SetReg(0x0F00, 0x08100748);   // RB_EDRAM_TIMING
        SetReg(0x0F01, 0x0000200E);   // RB_BC_CONTROL

        s_nextVblank = os::MonotonicNs() + kVblankNs;
        uint32_t rptr = 0;
        uint32_t reported = 0;

        for (;;)
        {
            os::SleepNs(500000);

            s_cp.SyncScratchFromCpu();

            // Command processor: execute everything up to the write pointer.
            uint32_t wptr = Reg(kRegCpRbWptr);
            uint32_t ring = s_ringBase.load() ? GpuAddr(s_ringBase.load()) : 0;
            if (ring && wptr != rptr)
            {
                uint32_t sizeDwords = (1u << (s_ringSizeLog2.load() + 3)) / 4;
                CommandProcessor::Reader rd{ ring, sizeDwords, rptr };
                uint32_t end = wptr;
                if (end < rptr)
                    end += sizeDwords;      // wrapped
                s_cp.Execute(rd, end, 0);
                rptr = wptr;
                SetReg(kRegCpRbRptr, rptr);
                if (uint32_t wb = s_rptrWriteBack.load())
                    if (uint32_t g = GpuAddr(wb))
                        St32(g, rptr);
                if (reported < 3)
                {
                    reported++;
                    Log("GPU: executed ring up to 0x%X", wptr);
                }
            }

            ServiceVblank();
        }
    }

    void ServiceVblank()
    {
        uint64_t now = os::MonotonicNs();
        if (now < s_nextVblank)
            return;
        s_nextVblank += kVblankNs;
        if (now > s_nextVblank + kVblankNs * 4)
            s_nextVblank = now + kVblankNs; // don't try to catch up after a stall
        uint64_t n = ++s_vblanks;
        if (s_interruptCallback.load())
        {
            DeliverInterrupt(0);
            if (n == 1 || n == 60 || (n % 3600) == 0)
                Log("GPU: vblank %llu delivered (%llu frames presented so far)",
                    (unsigned long long)n, (unsigned long long)s_swaps.load());
        }
    }

    void EnsureGpu()
    {
        std::call_once(s_gpuStarted, [] {
            if (!os::StartThread(GpuThreadMain, nullptr, 16 << 20))
                LogFatal("Could not start the GPU thread");
        });
    }
}

// VdSetGraphicsInterruptCallback(callback, user_data)
GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    s_interruptCallback = Arg(ctx, 0);
    s_interruptUser = Arg(ctx, 1);
    Log("VdSetGraphicsInterruptCallback(0x%08X, 0x%08X)", Arg(ctx, 0), Arg(ctx, 1));
    EnsureGpu();
}

// VdInitializeRingBuffer(physical_address, size_log2)
GUEST_IMPORT(VdInitializeRingBuffer)
{
    s_ringBase = Arg(ctx, 0);
    s_ringSizeLog2 = Arg(ctx, 1);
    Log("VdInitializeRingBuffer(0x%08X -> guest 0x%08X, %u bytes)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)),
        1u << (Arg(ctx, 1) + 3));
    EnsureGpu();
}

// VdEnableRingBufferRPtrWriteBack(physical_address, block_size_log2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    s_rptrWriteBack = Arg(ctx, 0);
    Log("VdEnableRingBufferRPtrWriteBack(0x%08X -> guest 0x%08X)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)));
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    Log("VdSetSystemCommandBufferGpuIdentifierAddress(0x%08X)", Arg(ctx, 0));
}

// VdSwap(...) - the title presented a frame.
GUEST_IMPORT(VdSwap)
{
    uint64_t n = ++s_swaps;
    if (n <= 3 || (n & (n - 1)) == 0)
        Log("VdSwap: frame %llu presented", (unsigned long long)n);
    Ret(ctx, 0);
}

uint64_t wet::PresentedFrameCount() { return s_swaps.load(); }

// VdGetSystemCommandBuffer(u32* p0, u32* p1): the title only checks for non-null
// results. (Values as in Xenia.)
GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    if (Arg(ctx, 0))
    {
        Zero(Arg(ctx, 0), 0x94);
        St32(Arg(ctx, 0), 0xBEEF0000);
    }
    if (Arg(ctx, 1))
        St32(Arg(ctx, 1), 0xBEEF0001);
}

// VdPersistDisplay(unk0, u32* out): the title later frees *out with MmFreePhysicalMemory.
GUEST_IMPORT(VdPersistDisplay)
{
    if (uint32_t out = Arg(ctx, 1))
    {
        uint32_t phys = g_mem.physical.Alloc(0x1000, 0x1000);
        uint32_t addr = phys ? PhysicalToView(phys, 0xE0000000) : 0;
        if (phys)
            RegisterPhysicalAllocation(phys, 0x1000, 0xE0000000);
        St32(out, addr);
    }
    Ret(ctx, 1);
}

GUEST_IMPORT(VdSetDisplayMode) { Ret(ctx, 0); }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { Ret(ctx, 0); }
