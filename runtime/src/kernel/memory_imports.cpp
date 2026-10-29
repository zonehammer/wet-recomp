// NtAllocateVirtualMemory & friends, physical memory, pools.
#include "kernel.h"

#include <mutex>
#include <unordered_map>

using namespace wet;

namespace
{
    constexpr uint32_t MEM_COMMIT = 0x1000;
    constexpr uint32_t MEM_RESERVE = 0x2000;
    constexpr uint32_t MEM_DECOMMIT = 0x4000;
    constexpr uint32_t MEM_RELEASE = 0x8000;
    constexpr uint32_t MEM_FREE = 0x10000;
    constexpr uint32_t MEM_PRIVATE = 0x20000;
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t PAGE_NOACCESS = 0x01;
    constexpr uint32_t PAGE_READWRITE = 0x04;

    std::mutex s_protLock;
    std::unordered_map<uint32_t, uint32_t> s_protect; // allocation base -> protect

    RangeAllocator* VirtualHeapFor(uint32_t addr)
    {
        if (g_mem.virt4k.Contains(addr)) return &g_mem.virt4k;
        if (g_mem.virt64k.Contains(addr)) return &g_mem.virt64k;
        return nullptr;
    }

    // Find the allocation containing addr in any heap. Returns false if none.
    bool QueryAny(uint32_t addr, uint32_t* base, uint32_t* size)
    {
        if (IsPhysicalView(addr))
        {
            uint32_t viewBase = addr & 0xE0000000, pbase;
            if (!g_mem.physical.Query(ViewToPhysical(addr), &pbase, size))
                return false;
            *base = PhysicalToView(pbase, viewBase);
            return true;
        }
        for (RangeAllocator* h : { &g_mem.virt4k, &g_mem.virt64k, &g_mem.system })
            if (h->Contains(addr))
                return h->Query(addr, base, size);
        return false;
    }
}

// NtAllocateVirtualMemory(base_address*, region_size*, allocation_type, protect, debug_memory)
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t basePtr = Arg(ctx, 0), sizePtr = Arg(ctx, 1), type = Arg(ctx, 2), protect = Arg(ctx, 3);
    uint32_t want = Ld32(basePtr), size = Ld32(sizePtr);
    if (!size)
        return Ret(ctx, STATUS_INVALID_PARAMETER);

    if (want)
    {
        // Committing (part of) a region we already handed out: everything we give out is committed.
        uint32_t b, s;
        if (QueryAny(want, &b, &s))
        {
            St32(basePtr, want);
            return Ret(ctx, STATUS_SUCCESS);
        }
        RangeAllocator* heap = VirtualHeapFor(want);
        uint32_t aligned = want & ~(heap ? heap->Granularity() - 1 : 0xFFF);
        if (!heap || !heap->AllocAt(aligned, size + (want - aligned)))
        {
            Log("NtAllocateVirtualMemory: fixed address 0x%08X (0x%X bytes) unavailable", want, size);
            return Ret(ctx, STATUS_NO_MEMORY);
        }
        St32(basePtr, aligned);
        heap->Query(aligned, nullptr, &size);
        St32(sizePtr, size);
        std::lock_guard lock(s_protLock);
        s_protect[aligned] = protect;
        return Ret(ctx, STATUS_SUCCESS);
    }

    RangeAllocator& heap = (type & MEM_LARGE_PAGES) ? g_mem.virt64k : g_mem.virt4k;
    uint32_t addr = heap.Alloc(size);
    if (!addr && &heap == &g_mem.virt4k)
        addr = g_mem.virt64k.Alloc(size);
    if (!addr)
    {
        Log("NtAllocateVirtualMemory: out of memory (0x%X bytes)", size);
        return Ret(ctx, STATUS_NO_MEMORY);
    }
    uint32_t actual;
    QueryAny(addr, &addr, &actual);
    St32(basePtr, addr);
    St32(sizePtr, actual);
    {
        std::lock_guard lock(s_protLock);
        s_protect[addr] = protect;
    }
    Ret(ctx, STATUS_SUCCESS);
}

// NtFreeVirtualMemory(base_address*, region_size*, free_type, debug_memory)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    uint32_t basePtr = Arg(ctx, 0), sizePtr = Arg(ctx, 1), type = Arg(ctx, 2);
    uint32_t addr = Ld32(basePtr);
    if (type & MEM_DECOMMIT)
        return Ret(ctx, STATUS_SUCCESS); // keep it committed; the region stays reserved
    RangeAllocator* heap = VirtualHeapFor(addr);
    uint32_t freed = 0;
    if (!heap || !heap->Free(addr, &freed))
        return Ret(ctx, STATUS_INVALID_PARAMETER);
    St32(sizePtr, freed);
    std::lock_guard lock(s_protLock);
    s_protect.erase(addr);
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryVirtualMemory(base_address, MEMORY_BASIC_INFORMATION*)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    uint32_t addr = Arg(ctx, 0), mbi = Arg(ctx, 1);
    uint32_t allocBase, size;
    Zero(mbi, 0x1C);
    if (QueryAny(addr, &allocBase, &size))
    {
        uint32_t prot = PAGE_READWRITE;
        {
            std::lock_guard lock(s_protLock);
            auto it = s_protect.find(allocBase);
            if (it != s_protect.end())
                prot = it->second;
        }
        St32(mbi + 0x00, addr & ~0xFFFu);   // BaseAddress
        St32(mbi + 0x04, allocBase);             // AllocationBase
        St32(mbi + 0x08, prot);             // AllocationProtect
        St32(mbi + 0x0C, allocBase + size - (addr & ~0xFFFu)); // RegionSize
        St32(mbi + 0x10, MEM_COMMIT);       // State
        St32(mbi + 0x14, prot);             // Protect
        St32(mbi + 0x18, MEM_PRIVATE);      // Type
    }
    else
    {
        St32(mbi + 0x00, addr & ~0xFFFu);
        St32(mbi + 0x0C, 0x1000);
        St32(mbi + 0x10, MEM_FREE);
        St32(mbi + 0x14, PAGE_NOACCESS);
    }
    Ret(ctx, STATUS_SUCCESS);
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min_address, max_address, alignment) -> address
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = Arg(ctx, 1), protect = Arg(ctx, 2);
    uint32_t minA = Arg(ctx, 3), maxA = Arg(ctx, 4), align = Arg(ctx, 5);
    uint32_t viewBase = PhysicalViewBase(protect);
    uint32_t pageSize = viewBase == 0xA0000000 ? 0x10000 : (viewBase == 0xC0000000 ? 0x1000000 : 0x1000);
    if (align < pageSize)
        align = pageSize;
    size = (size + pageSize - 1) & ~(pageSize - 1);

    uint32_t phys = g_mem.physical.Alloc(size, align, minA, maxA ? maxA : 0xFFFFFFFF);
    if (!phys)
    {
        Log("MmAllocatePhysicalMemoryEx: out of physical memory (0x%X bytes)", size);
        return Ret(ctx, 0);
    }
    uint32_t addr = PhysicalToView(phys, viewBase);
    g_mem.Commit(addr, size);
    Zero(addr, size);
    Ret(ctx, addr);
}

// MmFreePhysicalMemory(type, base_address)
GUEST_IMPORT(MmFreePhysicalMemory)
{
    uint32_t addr = Arg(ctx, 1), size = 0;
    if (g_mem.physical.Free(ViewToPhysical(addr), &size))
        g_mem.Decommit(addr, size);
}

GUEST_IMPORT(MmGetPhysicalAddress)
{
    uint32_t a = Arg(ctx, 0);
    Ret(ctx, IsPhysicalView(a) ? ViewToPhysical(a) : (a & 0x1FFFFFFF));
}

GUEST_IMPORT(MmQueryAddressProtect)
{
    uint32_t allocBase, size;
    if (!QueryAny(Arg(ctx, 0), &allocBase, &size))
    {
        // The XEX image and other fixed regions
        uint32_t a = Arg(ctx, 0);
        return Ret(ctx, (a >= PPC_IMAGE_BASE && a < PPC_IMAGE_BASE + PPC_IMAGE_SIZE) ? PAGE_READWRITE : 0);
    }
    std::lock_guard lock(s_protLock);
    auto it = s_protect.find(allocBase);
    Ret(ctx, it != s_protect.end() ? it->second : PAGE_READWRITE);
}

// MmSetAddressProtect(base, size, protect): protection isn't enforced on the host.
GUEST_IMPORT(MmSetAddressProtect)
{
    uint32_t allocBase, size;
    if (QueryAny(Arg(ctx, 0), &allocBase, &size))
    {
        std::lock_guard lock(s_protLock);
        s_protect[allocBase] = Arg(ctx, 2);
    }
}

GUEST_IMPORT(MmQueryAllocationSize)
{
    uint32_t allocBase, size;
    Ret(ctx, QueryAny(Arg(ctx, 0), &allocBase, &size) ? size : 0);
}

// MmQueryStatistics(MM_STATISTICS*): size field must be pre-set by the caller (0x68).
GUEST_IMPORT(MmQueryStatistics)
{
    uint32_t s = Arg(ctx, 0);
    if (Ld32(s) != 0x68)
        return Ret(ctx, STATUS_INVALID_PARAMETER);
    uint32_t freePages = uint32_t(g_mem.physical.FreeBytes() / 0x1000);
    Zero(s + 4, 0x64);
    St32(s + 0x04, 0x20000);        // total physical pages (512 MB)
    St32(s + 0x08, 0x100);          // kernel pages
    St32(s + 0x0C, freePages);      // title: available pages
    St32(s + 0x10, 0x2FFF0000);     // title: total virtual bytes
    St32(s + 0x14, 0);              // title: reserved virtual bytes
    St32(s + 0x18, 0x20000 - freePages); // title: physical pages
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(MmMapIoSpace)
{
    TraceCall("MmMapIoSpace", ctx, "returning the physical address unchanged");
    Ret(ctx, Arg(ctx, 0));
}

// ---- Pools ----
// ExAllocatePoolTypeWithTag(size, tag, pool_type)
GUEST_IMPORT(ExAllocatePoolTypeWithTag)
{
    uint32_t size = Arg(ctx, 0);
    uint32_t a = g_mem.system.Alloc(size ? size : 1, size >= 0x1000 ? 0x1000 : 0x10);
    if (!a)
        Log("ExAllocatePoolTypeWithTag: out of memory (0x%X)", size);
    Ret(ctx, a);
}

GUEST_IMPORT(ExFreePool)
{
    SystemFree(Arg(ctx, 0));
}

// XamAlloc(flags, size, out_ptr*)
GUEST_IMPORT(XamAlloc)
{
    uint32_t size = Arg(ctx, 1);
    uint32_t a = g_mem.system.Alloc(size ? size : 1, 0x10);
    St32(Arg(ctx, 2), a);
    Ret(ctx, a ? ERROR_SUCCESS : 0x8007000E /* E_OUTOFMEMORY */);
}

GUEST_IMPORT(XamFree)
{
    SystemFree(Arg(ctx, 0));
    Ret(ctx, ERROR_SUCCESS);
}
