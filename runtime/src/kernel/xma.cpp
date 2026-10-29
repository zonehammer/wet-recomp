// XMA audio decoder hardware, emulated as a decoder that produces silence.
//
// The title's XAudio2 (linked into the game) plays compressed XMA sounds through
// the console's hardware decoder: it fills a 64-byte context (input packet
// buffers, an output ring buffer) and "kicks" it through the APU registers at
// 0x7FEA0000; the hardware consumes input packets and writes PCM into the ring,
// which XAudio2 drains at the playback rate. Sounds end - and cutscenes waiting
// on a voice line move on - only when the hardware has consumed their input.
//
// There is no sound output yet, so this "decodes" to silence: every XMA frame in
// the input becomes 512 zero samples per channel, as fast as the output ring has
// room. That keeps every sound exactly as long as the real one, which is all the
// game's timing needs. Register and context layouts follow Xenia (apu/
// xma_decoder.cc, xma_context.h).
#include "kernel.h"
#include "xma.h"
#include "apu/apu.h"
#include "gpu/renderer.h"
#include "../memory.h"

#include <bitset>
#include <cstring>
#include <mutex>

using namespace wet;

namespace
{
    constexpr uint32_t kApuBase = 0x7FEA0000;
    constexpr uint32_t kRegContextArrayAddress = 0x600;
    constexpr uint32_t kRegNextContextIndex = 0x607;
    constexpr uint32_t kRegKick0 = 0x650, kRegLock0 = 0x690, kRegClear0 = 0x6A0;
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kBytesPerPacket = 2048;
    constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
    constexpr uint32_t kBytesPerFrameChannel = 512 * 2;     // 512 16-bit samples
    constexpr uint32_t kBytesPerBlock = 256;                // output ring unit

    std::mutex s_lock;
    uint32_t s_arrayPhys = 0, s_arrayGuest = 0;
    std::bitset<kContextCount> s_allocated;
    uint64_t s_kicks = 0, s_framesOut = 0;
    // Real decoding (Xenia's XmaContext with FFmpeg), started at the first
    // XMACreateContext so the renderer's logging is up; else the silent stand-in.
    bool s_realTried = false, s_real = false;

    // Context words (big-endian in guest memory). Bit positions from Xenia's
    // XMA_CONTEXT_DATA, which reads the words as little-endian bitfields: bit 0
    // is the least significant bit of each 32-bit word.
    struct Context
    {
        uint32_t w[16];
        static uint32_t Get(uint32_t v, int pos, int bits) { return (v >> pos) & ((1u << bits) - 1); }
        static void Set(uint32_t& v, int pos, int bits, uint32_t x)
        {
            uint32_t m = ((1u << bits) - 1) << pos;
            v = (v & ~m) | ((x << pos) & m);
        }
        uint32_t PacketCount(int buf) const { return Get(w[buf], 0, 12); }
        bool InputValid(int buf) const { return Get(w[0], 20 + buf, 1); }
        void SetInputValid(int buf, bool v) { Set(w[0], 20 + buf, 1, v); }
        uint32_t OutBlocks() const { return Get(w[0], 22, 5); }
        uint32_t OutWrite() const { return Get(w[0], 27, 5); }
        void SetOutWrite(uint32_t v) { Set(w[0], 27, 5, v); }
        bool Stereo() const { return Get(w[1], 29, 1); }
        bool OutValid() const { return Get(w[1], 31, 1); }
        void SetOutValid(bool v) { Set(w[1], 31, 1, v); }
        uint32_t ReadOffset() const { return Get(w[2], 0, 26); }
        void SetReadOffset(uint32_t v) { Set(w[2], 0, 26, v); }
        uint32_t LoopCount() const { return Get(w[0], 12, 8); }
        void SetLoopCount(uint32_t v) { Set(w[0], 12, 8, v); }
        uint32_t LoopStart() const { return Get(w[3], 0, 26); }
        uint32_t LoopEnd() const { return Get(w[4], 0, 26); }
        int CurrentBuffer() const { return int(Get(w[4], 31, 1)); }
        void SetCurrentBuffer(int b) { Set(w[4], 31, 1, uint32_t(b)); }
        uint32_t InputPtr(int buf) const { return w[5 + buf]; }
        uint32_t OutputPtr() const { return w[7]; }
        uint32_t OutRead() const { return Get(w[9], 0, 5); }
        void SetOutRead(uint32_t v) { Set(w[9], 0, 5, v); }

        void Load(uint32_t guest) { for (int i = 0; i < 16; i++) w[i] = Ld32(guest + i * 4); }
        void Store(uint32_t guest) const { for (int i = 0; i < 16; i++) St32(guest + i * 4, w[i]); }
    };

    uint32_t ContextGuest(uint32_t id) { return s_arrayGuest + id * kContextSize; }
    uint32_t PhysToGuest(uint32_t phys) { return PhysicalToView(phys & 0x1FFFFFFF, 0xA0000000); }

    void SwapInput(Context& c)
    {
        c.SetInputValid(c.CurrentBuffer(), false);
        c.SetCurrentBuffer(c.CurrentBuffer() ^ 1);
        c.SetReadOffset(0);
    }

    // One kick: turn input frames into silent output while the ring has room.
    void Work(uint32_t id)
    {
        uint32_t guest = ContextGuest(id);
        Context c;
        c.Load(guest);
        if (!c.OutValid())
            return;
        if (!c.InputValid(0) && !c.InputValid(1))
        {
            c.SetOutValid(false);
            c.Store(guest);
            return;
        }
        uint32_t ringBlocks = c.OutBlocks();
        if (!ringBlocks)
            return;
        uint32_t frameBlocks = (kBytesPerFrameChannel << (c.Stereo() ? 1 : 0)) / kBytesPerBlock;
        uint32_t write = c.OutWrite(), read = c.OutRead();
        // Free space as Xenia's RingBuffer counts it (read == write is empty).
        uint32_t freeBlocks = read > write ? read - write : ringBlocks - write + read;
        uint32_t outGuest = PhysToGuest(c.OutputPtr());

        for (int guard = 0; freeBlocks > frameBlocks && guard < 64; guard++)
        {
            if (!c.InputValid(0) && !c.InputValid(1))
                break;
            int buf = c.CurrentBuffer();
            if (!c.InputValid(buf))
            {
                c.SetCurrentBuffer(buf ^ 1);
                c.SetReadOffset(0);
                continue;
            }
            uint32_t packets = c.PacketCount(buf);
            uint32_t offset = c.ReadOffset();
            uint32_t packet = offset / kBitsPerPacket;
            if (!packets || packet >= packets)
            {
                SwapInput(c);
                continue;
            }
            uint32_t pktGuest = PhysToGuest(c.InputPtr(buf)) + packet * kBytesPerPacket;
            uint8_t hdr[4];
            std::memcpy(hdr, HostPtr(pktGuest), 4);
            uint32_t frames = hdr[0] >> 2;                       // frames starting in this packet
            uint32_t skip = hdr[3] == 0xFF ? 0 : hdr[3];         // packets of other streams
            uint32_t inPacket = offset % kBitsPerPacket;
            // Frames already taken from this packet, tracked through the offset:
            // header (32 bits) + index * step.
            uint32_t step = frames ? (kBitsPerPacket - 32) / frames : 0;
            uint32_t taken = (inPacket <= 32 || !step) ? 0 : (inPacket - 32) / step;
            if (taken < frames)
            {
                // One frame of silence.
                for (uint32_t b = 0; b < frameBlocks; b++)
                {
                    std::memset(HostPtr(outGuest + write * kBytesPerBlock), 0, kBytesPerBlock);
                    write = (write + 1) % ringBlocks;
                }
                freeBlocks -= frameBlocks;
                s_framesOut++;
                taken++;
            }
            if (taken >= frames)
            {
                uint32_t next = packet + 1 + skip;
                uint32_t nextOffset = next * kBitsPerPacket;
                // Looping: jump back while loops remain (offsets are in bits).
                if (c.LoopCount() && c.LoopStart() < c.LoopEnd() && nextOffset > c.LoopEnd())
                {
                    if (c.LoopCount() != 255)
                        c.SetLoopCount(c.LoopCount() - 1);
                    nextOffset = (c.LoopStart() / kBitsPerPacket) * kBitsPerPacket;
                    next = nextOffset / kBitsPerPacket;
                }
                if (next >= packets)
                    SwapInput(c);
                else
                    c.SetReadOffset(nextOffset);
            }
            else
                c.SetReadOffset(packet * kBitsPerPacket + 32 + taken * step);
        }
        c.SetOutWrite(write);
        c.Store(guest);
    }
}

namespace wet
{
    void InitXma()
    {
        uint32_t size = kContextCount * kContextSize;
        uint32_t phys = g_mem.physical.Alloc(0x10000, 0x10000, 0, 0xFFFFFFFF);
        if (!phys)
        {
            Log("XMA: could not allocate the context array");
            return;
        }
        s_arrayPhys = phys;
        s_arrayGuest = PhysicalToView(phys, 0xA0000000);
        g_mem.Commit(s_arrayGuest, 0x10000);
        Zero(s_arrayGuest, size);
        RegisterPhysicalAllocation(phys, 0x10000, 0xA0000000);
        // The kernel writes the context array's physical address for the title's
        // XAudio2, which reads it back from the APU registers.
        // APU registers are little-endian (the title reads them with lwbrx).
        St32(kApuBase + kRegContextArrayAddress * 4, __builtin_bswap32(phys));
        St32(kApuBase + kRegNextContextIndex * 4, __builtin_bswap32(1u));
        Log("XMA: %u contexts at 0x%08X (physical 0x%08X)",
            kContextCount, s_arrayGuest, phys);
    }

    bool XmaMmioWrite(uint32_t address, uint32_t value)
    {
        if ((address & 0xFFFF0000) != kApuBase)
            return false;
        uint32_t reg = (address & 0xFFFF) / 4;
        std::lock_guard lock(s_lock);
        auto forEach = [&](uint32_t first, auto fn) {
            uint32_t baseId = (reg - first) * 32;
            for (uint32_t i = 0, v = value; v && i < 32; i++, v >>= 1)
                if ((v & 1) && baseId + i < kContextCount)
                    fn(baseId + i);
        };
        if (reg >= kRegKick0 && reg < kRegKick0 + 10)
        {
            if (s_kicks++ == 0)
                Log("XMA: first context kick (0x%08X)", value);
            if ((s_kicks & (s_kicks - 1)) == 0 && s_kicks >= 1024)
                Log("XMA: %llu kicks, %llu silent frames produced", (unsigned long long)s_kicks,
                    (unsigned long long)s_framesOut);
            if (s_real)
                forEach(kRegKick0, [](uint32_t id) { apu::XmaKick(id); });
            else
                forEach(kRegKick0, [](uint32_t id) { Work(id); });
        }
        else if (s_real && reg >= kRegClear0 && reg < kRegClear0 + 10)
            forEach(kRegClear0, [](uint32_t id) { apu::XmaClear(id); });
        else if (s_real && reg >= kRegLock0 && reg < kRegLock0 + 10)
            forEach(kRegLock0, [](uint32_t id) { apu::XmaLock(id); });
        else if (reg >= kRegClear0 && reg < kRegClear0 + 10)
        {
            forEach(kRegClear0, [](uint32_t id) {
                Context c;
                c.Load(ContextGuest(id));
                c.SetInputValid(0, false);
                c.SetInputValid(1, false);
                c.SetOutValid(false);
                c.SetOutRead(0);
                c.SetOutWrite(0);
                c.Store(ContextGuest(id));
            });
        }
        // Lock (0x690+) only stops processing, which never runs in the background here.
        return true;
    }

    uint32_t XmaAllocateContext()
    {
        std::lock_guard lock(s_lock);
        if (!s_arrayGuest)
            return 0;
        if (!s_realTried)
        {
            s_realTried = true;
            s_real = apu::XmaStart(g_mem.base, PhysicalHostView(), s_arrayGuest, kContextCount);
            Log("XMA: %s", s_real ? "decoding with FFmpeg" : "decoding to silence");
        }
        for (uint32_t i = 0; i < kContextCount; i++)
            if (!s_allocated[i])
            {
                s_allocated[i] = true;
                Zero(ContextGuest(i), kContextSize);
                if (s_real)
                    apu::XmaAllocate(i);
                return ContextGuest(i);
            }
        return 0;
    }

    void XmaReleaseContext(uint32_t guest)
    {
        std::lock_guard lock(s_lock);
        if (guest < s_arrayGuest)
            return;
        uint32_t id = (guest - s_arrayGuest) / kContextSize;
        if (id < kContextCount)
        {
            s_allocated[id] = false;
            if (s_real)
                apu::XmaRelease(id);
            Zero(ContextGuest(id), kContextSize);
        }
    }
}

// See include/wet_ppc_hooks.h. `value` is the argument of PPC_STORE_U32 (memory
// gets it big-endian); the devices see it byte-swapped, as Xenia's MMIO
// handlers do.
void WetMmioStore32(uint32_t address, uint32_t value)
{
    uint32_t mem = __builtin_bswap32(value);
    std::memcpy(HostPtr(address), &mem, 4);
    if ((address & 0xFFFF0000) == kApuBase)
        XmaMmioWrite(address, __builtin_bswap32(value));
    else if (address >= 0x7FC80000 && address < 0x7FD00000)
        wet::renderer_host::RequestRegisterSync();
}
