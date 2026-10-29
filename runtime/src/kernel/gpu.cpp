// Stand-in GPU: just enough of the Xenos interface for the game to get past
// graphics initialisation and into its frame loop. Nothing is drawn.
//
// - GPU registers live at 0x7FC80000 (plain memory, see memory.h).
// - A command processor walks the PM4 packets the game submits and performs the
//   bookkeeping ones: register writes (with scratch-register write-back),
//   MEM_WRITE / EVENT_WRITE_SHD fences, INDIRECT_BUFFER and INTERRUPT. Everything
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

    PPCContext* s_gpuCtx = nullptr;     // the GPU thread's guest context
    GuestThread* s_gpuThread = nullptr;

    void DeliverInterrupt(uint32_t source)
    {
        uint32_t cb = s_interruptCallback.load();
        if (!cb || !s_gpuCtx)
            return;
        PPCContext& ctx = *s_gpuCtx;
        ctx.r1.u64 = s_gpuThread->stackBase - 0x80;
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

    // GPU (physical) address -> guest address; 0 if unmapped.
    uint32_t GpuAddr(uint32_t a)
    {
        return PhysicalToGuest(a & 0x1FFFFFFF);
    }

    // ---------------------------------------------------------------------
    // Command processor
    // ---------------------------------------------------------------------
    struct CommandProcessor
    {
        uint32_t packetsSeen[128] = {};
        bool warnedUnmapped = false;

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= 0x10000)
                return;
            SetReg(index, value);
            if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8)
            {
                uint32_t n = index - kRegScratchReg0;
                if (Reg(kRegScratchUmsk) & (1u << n))
                {
                    uint32_t mem = GpuAddr(Reg(kRegScratchAddr) + n * 4);
                    if (mem)
                        St32(mem, value);
                }
            }
        }

        // Reads dwords from guest memory (big-endian), handling ring wrap.
        struct Reader
        {
            uint32_t base, sizeDwords, pos; // pos in dwords
            uint32_t Read() { uint32_t v = Ld32(base + (pos % sizeDwords) * 4); pos++; return v; }
        };

        void Execute(Reader& r, uint32_t endPos, int depth)
        {
            while (r.pos != endPos)
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
        }

        void ExecuteType3(Reader& r, uint32_t header, int depth)
        {
            uint32_t opcode = (header >> 8) & 0x7F;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            uint32_t start = r.pos;
            if (packetsSeen[opcode]++ == 0)
                Log("GPU: first PM4 packet 0x%02X (%u dwords)", opcode, count);

            switch (opcode)
            {
            case 0x3F: // INDIRECT_BUFFER
            case 0x37: // INDIRECT_BUFFER_PFD
            {
                uint32_t addr = r.Read(), size = r.Read() & 0xFFFFF;
                uint32_t guest = GpuAddr(addr);
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
                uint32_t addr = r.Read() & ~3u;
                for (uint32_t i = 1; i < count; i++)
                {
                    uint32_t v = r.Read();
                    if (uint32_t g = GpuAddr(addr + (i - 1) * 4))
                        St32(g, v);
                }
                break;
            }
            case 0x58: // EVENT_WRITE_SHD: initiator, address, value
            {
                uint32_t initiator = r.Read(), addr = r.Read(), value = r.Read();
                uint32_t data = (initiator >> 31) & 1 ? uint32_t(s_swaps.load()) : value;
                if (uint32_t g = GpuAddr(addr & ~3u))
                {
                    uint32_t raw = GpuSwap(data, addr & 3);
                    std::memcpy(HostPtr(g), &raw, 4);
                }
                break;
            }
            case 0x45: // COND_WRITE: poll info, poll addr/reg, ref, mask, write addr/reg, value
            {
                uint32_t info = r.Read(), pollAddr = r.Read(), ref = r.Read(), mask = r.Read();
                uint32_t writeAddr = r.Read(), value = r.Read();
                uint32_t cur = (info & 0x10) ? (GpuAddr(pollAddr & ~3u) ? GpuSwap(Ld32(GpuAddr(pollAddr & ~3u)), pollAddr & 3) : 0)
                                             : Reg(pollAddr & 0xFFFF);
                bool ok;
                switch (info & 7)
                {
                case 0: ok = false; break;
                case 1: ok = (cur & mask) < ref; break;
                case 2: ok = (cur & mask) <= ref; break;
                case 3: ok = (cur & mask) == ref; break;
                case 4: ok = (cur & mask) != ref; break;
                case 5: ok = (cur & mask) >= ref; break;
                case 6: ok = (cur & mask) > ref; break;
                default: ok = true; break;
                }
                if (ok)
                {
                    if (info & 0x100) { if (uint32_t g = GpuAddr(writeAddr & ~3u)) St32(g, value); }
                    else WriteRegister(writeAddr & 0xFFFF, value);
                }
                break;
            }
            case 0x21: // REG_RMW: reg, and mask, or mask
            {
                uint32_t reg = r.Read() & 0x1FFF, andMask = r.Read(), orMask = r.Read();
                WriteRegister(reg, (Reg(reg) & andMask) | orMask);
                break;
            }
            case 0x54: // INTERRUPT: cpu mask
                r.Read();
                DeliverInterrupt(1);
                break;
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

        constexpr uint64_t kVblankNs = 16666667;
        uint64_t nextVblank = os::MonotonicNs() + kVblankNs;
        uint32_t rptr = 0;
        uint32_t reported = 0;

        for (;;)
        {
            os::SleepNs(500000);

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

            // Vertical blank.
            uint64_t now = os::MonotonicNs();
            if (now >= nextVblank)
            {
                nextVblank += kVblankNs;
                if (now > nextVblank + kVblankNs * 4)
                    nextVblank = now + kVblankNs; // don't try to catch up after a stall
                uint64_t n = ++s_vblanks;
                if (s_interruptCallback.load())
                {
                    DeliverInterrupt(0);
                    if (n == 1 || n == 60 || (n % 3600) == 0)
                        Log("GPU: vblank %llu delivered (%llu frames presented so far)",
                            (unsigned long long)n, (unsigned long long)s_swaps.load());
                }
            }
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
