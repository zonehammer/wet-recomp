// Stand-in GPU: just enough of the Xenos interface for the game to get past
// graphics initialisation and into its frame loop. Nothing is drawn.
//
// - GPU registers live at 0x7FC80000 (plain memory, see memory.h).
// - A command processor walks the PM4 packets the game submits and performs the
//   bookkeeping ones: register writes (with scratch-register write-back),
//   MEM_WRITE / EVENT_WRITE_SHD / COND_WRITE fences, WAIT_REG_MEM (the CP
//   blocks until the CPU sets a flag), INDIRECT_BUFFER (fetched in full when
//   started, as the hardware does) and INTERRUPT. Everything
//   else (draws, state) is skipped by length. CP_RB_RPTR and the read-pointer
//   write-back then follow CP_RB_WPTR.
// - A 60 Hz vertical-blank interrupt is delivered to the game's graphics
//   interrupt callback (source 0), on a dedicated thread with its own guest stack.
//
// VdSwap fills the 64-dword slot the title reserves for it, and
// VdInitializeScalerCommandBuffer reports the length of the buffer it fills: both
// end up inside the command stream, so getting them wrong desynchronises the CP.
//
// With the renderer (gpu/xenia_bridge.cpp, the default when built in), Xenia's
// command processor executes the ring instead: this file then only forwards the
// Vd* calls and polls the GPU registers the title writes through MMIO.
#include "kernel.h"
#include "module.h"
#include "gpu/renderer.h"
#include "os.h"
#include "threads.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

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
    // Swaps the command processor has executed (Xenia's counter_). EVENT_WRITE_SHD
    // reports this; the title uses it as a fence to recycle command and vertex
    // memory, so it must not run ahead of the CP the way VdSwap calls do.
    std::atomic<uint64_t> s_cpSwaps{ 0 };
    // Timing of the CP's work, for the overrun report.
    uint64_t s_lastWaitNs = 0, s_lastWaitEndNs = 0, s_batchStartNs = 0;
    uint32_t s_batchFrom = 0, s_batchTo = 0, s_lastWaitAt = 0, s_curRingPos = 0, s_lastWaitRingPos = 0;
    uint32_t s_lastWaitArgs[4] = {}, s_lastWaitStartV = 0, s_lastWaitEndV = 0;
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

    // Diagnostics: fingerprints of indirect buffers taken when they are first seen in
    // the ring, compared when the CP actually executes them.
    std::unordered_map<uint64_t, uint64_t> s_ibPrint; // (addr<<32|size) -> hash
    uint64_t HashIb(uint32_t guest, uint32_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; i++)
            h = (h ^ Ld32(guest + i * 4)) * 1099511628211ull;
        return h;
    }
    uint32_t s_ringScanPos = 0;   // ring dword index scanned so far (absolute, may exceed size)
    // Always on (cheap): the first dwords of each IB when the scanner first saw it.
    struct IbSeen { uint32_t first[4]; uint64_t ns; uint32_t ringPos; };
    std::unordered_map<uint32_t, IbSeen> s_ibSeen;
    void ScanRingAhead();

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
        // Tiled-rendering predication (SET_BIN_MASK/SELECT): a type-3 packet with
        // bit 0 of its header set only runs if (bin_select & bin_mask) != 0.
        uint64_t binMask = ~0ull, binSelect = ~0ull;
        uint64_t predicatedSkipped = 0;

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
            if ((index == kRegScratchAddr || index == kRegScratchUmsk) && Reg(index) != value)
            {
                static int logged = 0;
                if (logged++ < 20)
                {
                    Log("GPU: %s changed %08X -> %08X (packet at %08X, depth %d, buffer %08X, end +%X)",
                        index == kRegScratchAddr ? "SCRATCH_ADDR" : "SCRATCH_UMSK", Reg(index), value, curPacket,
                        curDepth, curBase, curEnd);
                    for (uint32_t a = curPacket - 32; a < curPacket + 32; a += 16)
                        Log("   %08X: %08X %08X %08X %08X", a, Ld32(a), Ld32(a + 4), Ld32(a + 8), Ld32(a + 12));
                }
            }
            SetReg(index, value);
            if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8)
                ScratchWriteBack(index - kRegScratchReg0, value);
        }

        // Reads command dwords (big-endian). The ring is read from guest memory as it
        // is executed (with wrap); indirect buffers are read from a copy taken when the
        // CP starts them, like the hardware's IB prefetch. The title relies on that:
        // once a buffer has started (signalled through a scratch register), it reuses
        // the buffer's tail for the next submission.
        struct Reader
        {
            uint32_t base, sizeDwords, pos; // pos in dwords; base = guest address (for logs)
            const uint32_t* copy = nullptr; // raw big-endian dwords, or null for live memory
            uint32_t copySize = 0;
            uint32_t Read()
            {
                uint32_t v;
                if (copy)
                    v = pos < copySize ? __builtin_bswap32(copy[pos]) : 0x80000000u; // type-2 no-op past the end
                else
                    v = Ld32(base + (pos % sizeDwords) * 4);
                pos++;
                return v;
            }
            uint32_t Peek(uint32_t at) const
            {
                if (copy)
                    return at < copySize ? __builtin_bswap32(copy[at]) : 0;
                return Ld32(base + (at % sizeDwords) * 4);
            }
        };

        uint32_t overruns = 0;
        uint32_t snap[0x4000] = {};
        uint32_t snapAddr = 0, snapSize = 0;
        uint64_t snapNs = 0;

        uint32_t curPacket = 0, curBase = 0, curStart = 0, curEnd = 0;
        int curDepth = 0;

        struct Hist { uint32_t addr, header; int depth; };
        Hist hist[16] = {};
        uint32_t histN = 0;
        bool dumped = false;
        struct IbHist { uint32_t from, guest, size; int depth; };
        IbHist ibHist[8] = {};
        uint32_t ibHistN = 0;

        void Execute(Reader& r, uint32_t endPos, int depth)
        {
            while (r.pos < endPos)
            {
                curPacket = r.base + (r.pos % r.sizeDwords) * 4;
                if (depth == 0)
                    s_curRingPos = r.pos;
                curBase = r.base; curEnd = endPos; curDepth = depth;
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
                if (overruns == 1)
                {
                    uint64_t now = os::MonotonicNs();
                    Log("GPU: batch ring %X..%X started %.2f ms ago; last blocking WAIT at %08X lasted %.2f ms, ended %.2f ms ago",
                        s_batchFrom, s_batchTo, (now - s_batchStartNs) / 1e6, s_lastWaitAt, s_lastWaitNs / 1e6,
                        s_lastWaitEndNs ? (now - s_lastWaitEndNs) / 1e6 : -1.0);
                    Log("GPU: that WAIT was called from ring +%X: info %08X poll %08X ref %08X mask %08X, value %08X -> %08X; this IB from ring +%X",
                        s_lastWaitRingPos, s_lastWaitArgs[0], s_lastWaitArgs[1], s_lastWaitArgs[2], s_lastWaitArgs[3],
                        s_lastWaitStartV, s_lastWaitEndV, s_curRingPos);
                    if (auto it = s_ibSeen.find(r.base); it != s_ibSeen.end())
                        Log("GPU: this buffer was seen in the ring at +%X %.2f ms ago; it began %08X %08X %08X %08X then",
                            it->second.ringPos, (now - it->second.ns) / 1e6, it->second.first[0], it->second.first[1],
                            it->second.first[2], it->second.first[3]);
                    else
                        Log("GPU: this buffer was not seen by the ring scanner beforehand");
                    Log("GPU: buffer %08X (%u dwords); last packets (guest address, header, depth):", r.base, endPos);
                    for (uint32_t i = histN > 16 ? histN - 16 : 0; i < histN; i++)
                        Log("   %08X  %08X  d%d", hist[i % 16].addr, hist[i % 16].header, hist[i % 16].depth);
                    Log("GPU: last indirect buffers (from, start, dwords, depth):");
                    for (uint32_t i = ibHistN > 8 ? ibHistN - 8 : 0; i < ibHistN; i++)
                        Log("   %08X -> %08X  %5u  d%d", ibHist[i % 8].from, ibHist[i % 8].guest, ibHist[i % 8].size, ibHist[i % 8].depth);
                    for (uint32_t k = 0; k < 24 && k < endPos; k += 4)
                        Log("   +%03X: %08X %08X %08X %08X", k, r.Peek(k), r.Peek(k + 1), r.Peek(k + 2), r.Peek(k + 3));
                }
                if (depth && std::getenv("WET_GPU_TRACE"))
                {
                    // Re-walk the buffer printing each packet header.
                    Reader w{ r.base, r.sizeDwords, 0, r.copy, r.copySize };
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
                            std::snprintf(words + strlen(words), sizeof(words) - strlen(words), " %08X", r.Peek(at + 1 + k));
                        Log("   +%04X %08X type%u op%02X n=%u:%s", at, h, h >> 30, (h >> 8) & 0x7F, n, words);
                        w.pos += n;
                    }
                }
            }
        }


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

            if ((header & 1) && ((binSelect & binMask) == 0 || opcode == 0x64))
            {
                if (predicatedSkipped++ == 0)
                    Log("GPU: skipping predicated packets whose bin is not selected (first: 0x%02X)", opcode);
                r.pos = start + count;
                return;
            }

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
                if (guest && depth == 0 && std::getenv("WET_GPU_TRACE"))
                {
                    auto it = s_ibPrint.find((uint64_t(guest) << 32) | size);
                    if (it != s_ibPrint.end())
                    {
                        if (it->second != HashIb(guest, size))
                        {
                            static int warned = 0;
                            if (warned++ < 10)
                                Log("GPU TRACE: IB %08X (%u dwords) was OVERWRITTEN between submission and execution", guest, size);
                        }
                        s_ibPrint.erase(it);
                    }
                }
                if (guest && depth < 4)
                {
                    if (depth == 0 && std::getenv("WET_GPU_TRACE"))
                    {
                        snapAddr = guest;
                        snapSize = std::min<uint32_t>(size + 64, 0x4000);
                        snapNs = os::MonotonicNs();
                        for (uint32_t i = 0; i < snapSize; i++)
                            snap[i] = Ld32(guest + i * 4);
                    }
                    std::vector<uint32_t> fetched;
                    if (std::getenv("WET_GPU_PREFETCH"))
                    {
                        fetched.resize(size);
                        std::memcpy(fetched.data(), HostPtr(guest), size_t(size) * 4);
                    }
                    Reader ib{ guest, 0xFFFFFFFF, 0, fetched.empty() ? nullptr : fetched.data(), size };
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
                uint32_t data = (initiator >> 31) & 1 ? uint32_t(s_cpSwaps.load()) : value;
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
                static int traced = 0;
                bool trace = std::getenv("WET_GPU_TRACE") && traced < 400;
                uint32_t dev = s_interruptUser.load();
                uint32_t here = r.base + (start - 1) * 4;
                uint32_t v0 = (info & 0x10) ? GpuLoad(mem, poll & 3) : Reg(poll & 0xFFFF);
                if (trace && !Compare(info, v0 & mask, ref))
                {
                    traced++;
                    Log("GPU TRACE: WAIT at %08X (+%04X of %08X) for %08X==%08X blocks; CPU write ptr %08X",
                        here, start - 1, r.base, (info & 0x10) ? mem : poll, ref, dev ? Ld32(dev + 48) : 0);
                }
                for (;;)
                {
                    uint32_t v = (info & 0x10) ? GpuLoad(mem, poll & 3) : Reg(poll & 0xFFFF);
                    if (Compare(info, v & mask, ref))
                    {
                        if (!Compare(info, v0 & mask, ref))
                        {
                            s_lastWaitEndNs = os::MonotonicNs();
                            s_lastWaitNs = s_lastWaitEndNs - startNs;
                            s_lastWaitAt = here;
                            s_lastWaitRingPos = s_curRingPos;
                            s_lastWaitArgs[0] = info; s_lastWaitArgs[1] = poll; s_lastWaitArgs[2] = ref; s_lastWaitArgs[3] = mask;
                            s_lastWaitStartV = v0; s_lastWaitEndV = v;
                        }
                        if (trace && !Compare(info, v0 & mask, ref))
                            Log("GPU TRACE: WAIT at %08X released after %.1f ms; CPU write ptr %08X; packet now %08X %08X %08X %08X %08X %08X | %08X %08X",
                                here, double(os::MonotonicNs() - startNs) / 1e6, dev ? Ld32(dev + 48) : 0,
                                Ld32(here), Ld32(here + 4), Ld32(here + 8), Ld32(here + 12), Ld32(here + 16), Ld32(here + 20),
                                Ld32(here + 24), Ld32(here + 28));
                        break;
                    }
                    if (!warned && os::MonotonicNs() - startNs > 5000000000ull)
                    {
                        warned = true;
                        Log("GPU: WAIT_REG_MEM still waiting after 5 s for %s %08X & %08X %s %08X (now %08X)",
                            (info & 0x10) ? "memory" : "register", (info & 0x10) ? mem : (poll & 0xFFFF), mask,
                            (const char*[]){ "never", "<", "<=", "==", "!=", ">=", ">", "always" }[info & 7], ref, v & mask);
                        if (r.copy)
                        {
                            Log("GPU TRACE: fetched buffer %08X (%u dwords), stuck at +%04X; packets:", r.base, r.copySize, start - 1);
                            for (uint32_t at = 0; at < r.copySize;)
                            {
                                uint32_t h = r.Peek(at), n = (h >> 30) == 1 ? 2 : (h >> 30) == 2 ? 0 : ((h >> 16) & 0x3FFF) + 1;
                                char words[160] = "";
                                for (uint32_t k = 0; k < n && k < 5; k++)
                                    std::snprintf(words + strlen(words), sizeof(words) - strlen(words), " %08X", r.Peek(at + 1 + k));
                                Log("   %s+%04X %08X%s", at == start - 1 ? ">" : " ", at, h, words);
                                at += 1 + n;
                            }
                        }
                        if (snapSize)
                        {
                            Log("GPU TRACE: buffer %08X (%u dwords) changed since the GPU entered it %.3f s ago:",
                                snapAddr, snapSize - 64, double(os::MonotonicNs() - snapNs) / 1e9);
                            int shown = 0;
                            for (uint32_t i = 0; i < snapSize && shown < 40; i++)
                            {
                                uint32_t now = Ld32(snapAddr + i * 4);
                                if (now != snap[i])
                                {
                                    Log("   +%04X (%08X): %08X -> %08X", i, snapAddr + i * 4, snap[i], now);
                                    shown++;
                                }
                            }
                        }
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
                    ScanRingAhead();
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
            case 0x60: binMask = (binMask & 0xFFFFFFFF00000000ull) | r.Read(); break;                 // SET_BIN_MASK_LO
            case 0x61: binMask = (binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;        // SET_BIN_MASK_HI
            case 0x62: binSelect = (binSelect & 0xFFFFFFFF00000000ull) | r.Read(); break;             // SET_BIN_SELECT_LO
            case 0x63: binSelect = (binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); break;    // SET_BIN_SELECT_HI
            case 0x50: { uint64_t hi = r.Read(); binMask = (hi << 32) | r.Read(); break; }             // SET_BIN_MASK
            case 0x51: { uint64_t hi = r.Read(); binSelect = (hi << 32) | r.Read(); break; }           // SET_BIN_SELECT
            case 0x64: // XE_SWAP (written by VdSwap): 'SWAP', front buffer, width, height
                if (count >= 1 && r.Peek(start) == 0x53574150)
                    s_cpSwaps++;
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

        // Display-controller registers the title reads (values as reported by Xenia).
        SetReg(0x1951, 1);            // D1MODE_VBLANK_STATUS: vblank interrupt pending/enabled
        SetReg(0x1961, 0x050002D0);   // D1MODE_VIEWPORT_SIZE: 1280x720
        SetReg(0x0F00, 0x08100748);   // RB_EDRAM_TIMING
        SetReg(0x0F01, 0x0000200E);   // RB_BC_CONTROL

        if (renderer::Running())
        {
            // Xenia's command processor and vsync threads do the real work. MMIO is
            // plain memory, so pass the title's register writes on: CP_RB_WPTR kicks
            // the CP, the rest (scratch registers, ...) go into its register file.
            constexpr uint32_t kMirror = 0x2000;
            std::vector<uint32_t> seen(kMirror);
            for (uint32_t i = 0; i < kMirror; i++)
                seen[i] = Reg(i);
            for (uint64_t tick = 0;; tick++)
            {
                os::SleepNs(250000);
                uint32_t wptr = Reg(kRegCpRbWptr);
                if (wptr != seen[kRegCpRbWptr])
                {
                    seen[kRegCpRbWptr] = wptr;
                    renderer::UpdateWritePointer(wptr);
                }
                if ((tick & 3) == 0)
                    for (uint32_t i = 0; i < kMirror; i++)
                    {
                        uint32_t v = Reg(i);
                        if (v != seen[i] && i != kRegCpRbWptr)
                        {
                            seen[i] = v;
                            renderer::WriteRegister(i, v);
                        }
                    }
                // Display registers the title polls.
                SetReg(0x1951, 1);
                SetReg(0x194C, 0x000002D0);
                SetReg(kRegCpRbRptr, renderer::ReadRegister(kRegCpRbRptr));
            }
        }

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
                s_batchStartNs = os::MonotonicNs();
                s_batchFrom = rptr;
                s_batchTo = wptr;
                s_cp.Execute(rd, end, 0);
                rptr = wptr;
                // Anything the scanner had not reached is executed now: forget stale entries.
                s_ringScanPos = rptr;
                if (s_ibSeen.size() > 4096)
                    s_ibSeen.clear();
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

    void ScanRingAhead()
    {
        uint32_t ring = s_ringBase.load() ? GpuAddr(s_ringBase.load()) : 0;
        if (!ring)
            return;
        uint32_t sizeDwords = (1u << (s_ringSizeLog2.load() + 3)) / 4;
        uint32_t wptr = Reg(kRegCpRbWptr);
        uint32_t pos = s_ringScanPos % sizeDwords;
        int guard = 0;
        while (pos != wptr && guard++ < 4096)
        {
            uint32_t h = Ld32(ring + pos * 4);
            uint32_t n = (h >> 30) == 1 ? 2 : (h >> 30) == 2 ? 0 : ((h >> 16) & 0x3FFF) + 1;
            if ((h >> 30) == 3 && (((h >> 8) & 0x7F) == 0x3F || ((h >> 8) & 0x7F) == 0x37))
            {
                uint32_t addr = Ld32(ring + ((pos + 1) % sizeDwords) * 4), size = Ld32(ring + ((pos + 2) % sizeDwords) * 4) & 0xFFFFF;
                if (uint32_t g = GpuAddr(addr))
                {
                    if (std::getenv("WET_GPU_TRACE"))
                        s_ibPrint.emplace((uint64_t(g) << 32) | size, HashIb(g, size));
                    s_ibSeen[g] = { { Ld32(g), Ld32(g + 4), Ld32(g + 8), Ld32(g + 12) }, os::MonotonicNs(), pos };
                }
            }
            pos = (pos + 1 + n) % sizeDwords;
        }
        s_ringScanPos = pos;
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
        // Hardware raises D1MODE_VBLANK_STATUS on every vblank; the title's handler
        // only acts when it is set and acknowledges it, so set it again each time.
        SetReg(0x1951, Reg(0x1951) | 1);
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
            if (renderer::Enabled())
                renderer::Start(g_mem.base, g_module.titleId);
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
    renderer::SetInterruptCallback(Arg(ctx, 0), Arg(ctx, 1));
}

// VdInitializeRingBuffer(physical_address, size_log2)
GUEST_IMPORT(VdInitializeRingBuffer)
{
    s_ringBase = Arg(ctx, 0);
    s_ringSizeLog2 = Arg(ctx, 1);
    Log("VdInitializeRingBuffer(0x%08X -> guest 0x%08X, %u bytes)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)),
        1u << (Arg(ctx, 1) + 3));
    EnsureGpu();
    renderer::InitializeRingBuffer(Arg(ctx, 0), Arg(ctx, 1));
}

// VdEnableRingBufferRPtrWriteBack(physical_address, block_size_log2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    s_rptrWriteBack = Arg(ctx, 0);
    Log("VdEnableRingBufferRPtrWriteBack(0x%08X -> guest 0x%08X)", Arg(ctx, 0), PhysicalToGuest(Arg(ctx, 0)));
    EnsureGpu();
    renderer::EnableReadPointerWriteBack(Arg(ctx, 0), Arg(ctx, 1));
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress)
{
    Log("VdSetSystemCommandBufferGpuIdentifierAddress(0x%08X)", Arg(ctx, 0));
}

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer*, format*, color_space*, width*, height*)
// The title reserves 64 dwords in its command buffer at `buffer` for the swap
// commands: an XE_SWAP packet { 'SWAP', front buffer, width, height, the front
// buffer's texture fetch constant (address made physical) } then type-2 no-ops.
// (Xenia writes the fetch constant into fetch constant 0 instead, which clobbers
// textures the title keeps bound there; the patched XE_SWAP handler restores it.)
constexpr uint32_t kPm4XeSwap = 0x64;

GUEST_IMPORT(VdSwap)
{
    uint32_t buffer = Arg(ctx, 0), fetch = Arg(ctx, 1);
    uint32_t frontPtr = Arg(ctx, 5), widthPtr = Arg(ctx, 8), heightPtr = Arg(ctx, 9);
    uint32_t front = frontPtr ? Ld32(frontPtr) : 0;
    uint32_t width = widthPtr ? Ld32(widthPtr) : 0, height = heightPtr ? Ld32(heightPtr) : 0;
    uint32_t frontPhys = ViewToPhysical(front);
    if (buffer)
    {
        uint32_t o = buffer;
        uint32_t count = fetch ? 10 : 4;
        St32(o, (3u << 30) | ((count - 1) << 16) | (kPm4XeSwap << 8)); o += 4;
        St32(o, 0x53574150); o += 4; // 'SWAP'
        St32(o, frontPhys); o += 4;
        St32(o, width); o += 4;
        St32(o, height); o += 4;
        if (fetch)
            for (uint32_t i = 0; i < 6; i++, o += 4)
            {
                uint32_t v = Ld32(fetch + i * 4);
                if (i == 1)
                    v = (v & 0xFFF) | (ViewToPhysical(v & ~0xFFFu) & ~0xFFFu);   // base address -> physical
                St32(o, v);
            }
        for (; o < buffer + 64 * 4; o += 4)
            St32(o, 0x80000000);    // type-2 no-op
    }
    uint64_t n = ++s_swaps;
    if (n <= 3 || (n & (n - 1)) == 0)
        Log("VdSwap: frame %llu presented (front buffer %08X, %ux%u)", (unsigned long long)n, front, width, height);
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

// VdInitializeScalerCommandBuffer(source_xy, source_wh, output_xy, output_wh, ..., dest, dest_count)
// -> dwords written to dest. WET passes 12 arguments: the destination buffer is
// argument 10 and its size in dwords argument 11 (it pre-fills it with type-2 no-ops,
// then copies the returned number of dwords into its command buffer). We emit no
// scaler commands, only no-ops, but must report the full length.
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    uint32_t dest = Arg(ctx, 10), count = Arg(ctx, 11);
    if (!dest || count > 0x10000)
    {
        Log("VdInitializeScalerCommandBuffer: unexpected arguments (dest %08X, count %u)", dest, count);
        return Ret(ctx, 0);
    }
    for (uint32_t i = 0; i < count; i++)
        St32(dest + i * 4, 0x80000000);
    Ret(ctx, count);
}
