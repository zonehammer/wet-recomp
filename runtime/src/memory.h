// Guest address space: one 4 GB host reservation; guest address A lives at base + A.
//
// Layout (mirrors the Xbox 360 where it matters):
//   0x10000000-0x3FFFFFFF  virtual heap, 4 KB pages     (NtAllocateVirtualMemory)
//   0x40000000-0x6FFFFFFF  virtual heap, 64 KB pages    (NtAllocateVirtualMemory, MEM_LARGE_PAGES)
//   0x70000000-0x7EFFFFFF  system heap                  (kernel objects, stacks, PCR/TLS, pools)
//   0x7FC80000-0x7FFFFFFF  hardware registers (GPU at 0x7FC80000, audio/XMA at 0x7FEA0000, ...),
//                          plain memory for now
//   0x82000000-...         the XEX image, then XenonRecomp's function table
//   0xA0000000/0xC0000000/0xE0000000  physical memory views (64 KB / 16 MB / 4 KB pages)
//
// Physical memory is one 512 MB block mapped at all three views at once, as on the
// console: 0xA0000000+P, 0xC0000000+P and 0xE0000000+P-0x1000 are the same byte.
// (If the OS can't map at 4 KB granularity, the 0xE0000000 view falls back to
// 0xE0000000+P; see Memory::eViewOffset.)
#pragma once
#include <cstdint>
#include <map>
#include <mutex>

namespace wet
{
    constexpr uint64_t kGuestSize = 0x100000000ull;

    enum class CommitMode
    {
        None,           // caller manages commit
        ZeroOnAlloc,    // commit on alloc, memset on reuse (sub-page granularity heaps)
        DecommitOnFree, // commit on alloc, decommit on free (page granularity heaps)
    };

    class RangeAllocator
    {
    public:
        RangeAllocator(const char* name, uint32_t start, uint32_t end, uint32_t granularity, CommitMode mode);

        // Returns 0 on failure. `align` is raised to the granularity.
        uint32_t Alloc(uint32_t size, uint32_t align = 0, uint32_t minAddr = 0, uint32_t maxAddr = 0xFFFFFFFF);
        // Allocate exactly [addr, addr+size) if free.
        bool AllocAt(uint32_t addr, uint32_t size);
        bool Free(uint32_t addr, uint32_t* freedSize = nullptr);
        bool Query(uint32_t addr, uint32_t* allocBase, uint32_t* allocSize);
        bool Contains(uint32_t addr) const { return addr >= m_start && addr < m_end; }
        uint64_t FreeBytes();
        uint32_t Granularity() const { return m_granularity; }
        const char* Name() const { return m_name; }

    private:
        void CommitBlock(uint32_t addr, uint32_t size);

        const char* m_name;
        uint32_t m_start, m_end, m_granularity;
        CommitMode m_mode;
        std::mutex m_mutex;
        std::map<uint32_t, uint32_t> m_free; // addr -> size
        std::map<uint32_t, uint32_t> m_used; // addr -> size
    };

    struct Memory
    {
        uint8_t* base = nullptr;

        RangeAllocator virt4k  { "virtual4k",  0x10000000, 0x40000000, 0x1000,  CommitMode::DecommitOnFree };
        RangeAllocator virt64k { "virtual64k", 0x40000000, 0x70000000, 0x10000, CommitMode::DecommitOnFree };
        RangeAllocator system  { "system",     0x70000000, 0x7F000000, 0x10,    CommitMode::ZeroOnAlloc };
        RangeAllocator physical{ "physical",   0x00010000, 0x1FFF0000, 0x1000,  CommitMode::None };
        uint32_t eViewOffset = 0x1000;  // 0xE0000000 view: guest = 0xE0000000 + phys - eViewOffset

        bool Init();
        bool SelfTest();    // checks the physical views really alias (used by CI)
        void Commit(uint32_t addr, uint32_t size);
        void Decommit(uint32_t addr, uint32_t size);

        template <typename T = uint8_t>
        T* Host(uint32_t guest) const { return reinterpret_cast<T*>(base + guest); }
        uint32_t Guest(const void* host) const { return uint32_t(static_cast<const uint8_t*>(host) - base); }
    };

    extern Memory g_mem;

    // Zeroed allocation from the system heap. Aborts on exhaustion.
    uint32_t SystemAlloc(uint32_t size, uint32_t align = 16);
    void SystemFree(uint32_t addr);

    // Physical views
    uint32_t PhysicalViewBase(uint32_t protect);  // 0xA0000000 / 0xC0000000 / 0xE0000000
    uint32_t PhysicalToView(uint32_t phys, uint32_t viewBase);
    uint32_t ViewToPhysical(uint32_t guest);
    bool IsPhysicalView(uint32_t guest);

    // Remember which view a physical allocation was handed out through, so physical
    // addresses the game passes to the GPU can be turned back into guest addresses.
    void RegisterPhysicalAllocation(uint32_t physBase, uint32_t size, uint32_t viewBase);
    void UnregisterPhysicalAllocation(uint32_t physBase);
    uint32_t PhysicalToGuest(uint32_t phys);    // 0 if not allocated

    constexpr uint32_t kMmioBase = 0x7FC80000;
    constexpr uint32_t kMmioSize = 0x80000000 - kMmioBase;
    constexpr uint32_t kGpuRegisterBase = 0x7FC80000;

    // ---- Physical-page write watches (the renderer's cache invalidation) ----
    // A watched page is read-only in all guest views of physical memory; the first
    // write to it calls the callback (physical start, length) and unwatches it.
    using PhysicalWriteCallback = void (*)(uint32_t physStart, uint32_t length);
    void SetPhysicalWriteCallback(PhysicalWriteCallback callback);
    void WatchPhysical(uint32_t physStart, uint32_t length);
    // A mapping of physical memory that is never watched (index = physical address).
    uint8_t* PhysicalHostView();
}
