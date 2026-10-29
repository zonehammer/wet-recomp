#include "memory.h"
#include "log.h"
#include "os.h"

#include <iterator>
#include <cstring>

namespace wet
{
    Memory g_mem;

    static constexpr uint32_t kHostPage = 0x1000;

    static uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

    RangeAllocator::RangeAllocator(const char* name, uint32_t start, uint32_t end, uint32_t granularity, CommitMode mode)
        : m_name(name), m_start(start), m_end(end), m_granularity(granularity), m_mode(mode)
    {
        m_free[start] = end - start;
    }

    void RangeAllocator::CommitBlock(uint32_t addr, uint32_t size)
    {
        if (m_mode == CommitMode::None)
            return;
        uint32_t first = addr & ~(kHostPage - 1);
        uint32_t last = uint32_t(AlignUp(uint64_t(addr) + size, kHostPage));
        g_mem.Commit(first, last - first);
        if (m_mode == CommitMode::ZeroOnAlloc)
            std::memset(g_mem.Host(addr), 0, size);
    }

    uint32_t RangeAllocator::Alloc(uint32_t size, uint32_t align, uint32_t minAddr, uint32_t maxAddr)
    {
        if (size == 0)
            size = 1;
        uint64_t sz = AlignUp(size, m_granularity);
        uint64_t al = align < m_granularity ? m_granularity : align;

        std::lock_guard lock(m_mutex);
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            uint64_t blockStart = it->first, blockEnd = uint64_t(it->first) + it->second;
            uint64_t s = AlignUp(blockStart > minAddr ? blockStart : minAddr, al);
            if (s + sz > blockEnd || s + sz - 1 > maxAddr)
                continue;

            m_free.erase(it);
            if (s > blockStart)
                m_free[uint32_t(blockStart)] = uint32_t(s - blockStart);
            if (s + sz < blockEnd)
                m_free[uint32_t(s + sz)] = uint32_t(blockEnd - (s + sz));
            m_used[uint32_t(s)] = uint32_t(sz);
            CommitBlock(uint32_t(s), uint32_t(sz));
            return uint32_t(s);
        }
        return 0;
    }

    bool RangeAllocator::AllocAt(uint32_t addr, uint32_t size)
    {
        uint64_t sz = AlignUp(size ? size : 1, m_granularity);
        std::lock_guard lock(m_mutex);
        auto it = m_free.upper_bound(addr);
        if (it == m_free.begin())
            return false;
        --it;
        uint64_t blockStart = it->first, blockEnd = uint64_t(it->first) + it->second;
        if (addr < blockStart || addr + sz > blockEnd)
            return false;
        m_free.erase(it);
        if (addr > blockStart)
            m_free[uint32_t(blockStart)] = uint32_t(addr - blockStart);
        if (addr + sz < blockEnd)
            m_free[uint32_t(addr + sz)] = uint32_t(blockEnd - (addr + sz));
        m_used[addr] = uint32_t(sz);
        CommitBlock(addr, uint32_t(sz));
        return true;
    }

    bool RangeAllocator::Free(uint32_t addr, uint32_t* freedSize)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_used.find(addr);
        if (it == m_used.end())
            return false;
        uint32_t size = it->second;
        m_used.erase(it);
        if (freedSize)
            *freedSize = size;
        if (m_mode == CommitMode::DecommitOnFree)
            g_mem.Decommit(addr, size);

        uint64_t start = addr, end = uint64_t(addr) + size;
        auto next = m_free.lower_bound(addr);
        if (next != m_free.end() && next->first == end)
        {
            end += next->second;
            next = m_free.erase(next);
        }
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (uint64_t(prev->first) + prev->second == start)
            {
                start = prev->first;
                m_free.erase(prev);
            }
        }
        m_free[uint32_t(start)] = uint32_t(end - start);
        return true;
    }

    bool RangeAllocator::Query(uint32_t addr, uint32_t* allocBase, uint32_t* allocSize)
    {
        std::lock_guard lock(m_mutex);
        auto it = m_used.upper_bound(addr);
        if (it == m_used.begin())
            return false;
        --it;
        if (addr >= uint64_t(it->first) + it->second)
            return false;
        if (allocBase) *allocBase = it->first;
        if (allocSize) *allocSize = it->second;
        return true;
    }

    uint64_t RangeAllocator::FreeBytes()
    {
        std::lock_guard lock(m_mutex);
        uint64_t total = 0;
        for (auto& [a, s] : m_free)
            total += s;
        return total;
    }

    bool Memory::Init()
    {
        // Preferred: the console's exact layout. The 0xE0000000 view starts one page
        // early (0xDFFFF000) so that 0xE0000000 shows physical 0x1000; the 0xC0000000
        // view stops one page short to make room.
        const os::AliasView exact[] = { { 0xA0000000, 0x20000000 }, { 0xC0000000, 0x1FFFF000 }, { 0xDFFFF000, 0x20000000 } };
        // Fallback for 64 KB-granular mapping: all views start on their window.
        const os::AliasView aligned[] = { { 0xA0000000, 0x20000000 }, { 0xC0000000, 0x20000000 }, { 0xE0000000, 0x20000000 } };

        const uint64_t total = kGuestSize + 0x10000; // slack so wide accesses at the top can't fault
        const char* why = "";
        base = os::ReserveWithAliases(total, exact, 3, 0x20000000, &why);
        eViewOffset = 0x1000;
        if (!base)
        {
            Log("Physical memory: exact console layout unavailable (%s); using 64 KB-aligned views", why);
            base = os::ReserveWithAliases(total, aligned, 3, 0x20000000, &why);
            eViewOffset = 0;
        }
        if (!base)
        {
            LogFatal("Could not set up guest memory: %s", why);
            return false;
        }
        Log("Guest memory reserved at host %p (physical views aliased, 0xE0000000 offset 0x%X)", (void*)base, eViewOffset);
        Commit(kMmioBase, kMmioSize);
        return true;
    }

    bool Memory::SelfTest()
    {
        uint32_t phys = physical.Alloc(0x20000, 0x10000);
        if (!phys)
            return false;
        uint32_t a = PhysicalToView(phys, 0xA0000000), c = PhysicalToView(phys, 0xC0000000), e = PhysicalToView(phys, 0xE0000000);
        bool ok = true;
        for (uint32_t i = 0; i < 0x20000; i += 0x1000)
        {
            *Host<uint32_t>(e + i) = 0x5EED0000u + i;
            ok &= *Host<uint32_t>(a + i) == 0x5EED0000u + i && *Host<uint32_t>(c + i) == 0x5EED0000u + i;
            *Host<uint32_t>(a + i) = 0xA11A5000u + i;
            ok &= *Host<uint32_t>(e + i) == 0xA11A5000u + i;
        }
        ok &= ViewToPhysical(e) == phys && ViewToPhysical(a) == phys && ViewToPhysical(c) == phys;
        // Ordinary (non-view) memory still commits normally.
        uint32_t v = virt4k.Alloc(0x3000);
        if (v) { *Host<uint32_t>(v + 0x2000) = 0x1234; ok &= *Host<uint32_t>(v + 0x2000) == 0x1234; virt4k.Free(v); }
        else ok = false;
        physical.Free(phys);
        Log("Memory self-test: %s (views A=0x%08X C=0x%08X E=0x%08X for physical 0x%08X)", ok ? "PASS" : "FAIL", a, c, e, phys);
        return ok;
    }

    void Memory::Commit(uint32_t addr, uint32_t size)
    {
        if (addr >= 0xA0000000)
            return; // physical views are always backed
        uint32_t first = addr & ~(kHostPage - 1);
        uint64_t last = AlignUp(uint64_t(addr) + size, kHostPage);
        if (!os::Commit(base + first, size_t(last - first)))
        {
            LogFatal("Failed to commit guest memory 0x%08X-0x%08llX (out of memory/page file?)",
                     first, (unsigned long long)last);
            os::ExitProcess(1);
        }
    }

    void Memory::Decommit(uint32_t addr, uint32_t size)
    {
        if (addr >= 0xA0000000)
            return;
        uint32_t first = uint32_t(AlignUp(addr, kHostPage));
        uint64_t last = (uint64_t(addr) + size) & ~uint64_t(kHostPage - 1);
        if (last > first)
            os::Decommit(base + first, size_t(last - first));
    }

    uint32_t SystemAlloc(uint32_t size, uint32_t align)
    {
        uint32_t a = g_mem.system.Alloc(size, align);
        if (!a)
        {
            LogFatal("System heap exhausted (request 0x%X bytes)", size);
            os::ExitProcess(1);
        }
        return a;
    }

    void SystemFree(uint32_t addr)
    {
        if (addr)
            g_mem.system.Free(addr);
    }

    uint32_t PhysicalViewBase(uint32_t protect)
    {
        if (protect & 0x20000000) return 0xA0000000; // MEM_LARGE_PAGES (64 KB)
        if (protect & 0x80000000) return 0xC0000000; // MEM_16MB_PAGES
        return 0xE0000000;                           // 4 KB pages
    }

    // The 0xE0000000 view is offset by one page on the console.
    uint32_t PhysicalToView(uint32_t phys, uint32_t viewBase)
    {
        return viewBase + phys - (viewBase == 0xE0000000 ? g_mem.eViewOffset : 0);
    }

    uint32_t ViewToPhysical(uint32_t guest)
    {
        return (guest & 0x1FFFFFFF) + (guest >= 0xE0000000 ? g_mem.eViewOffset : 0);
    }

    bool IsPhysicalView(uint32_t guest) { return guest >= 0xA0000000; }

    static std::mutex s_physMapLock;
    static std::map<uint32_t, std::pair<uint32_t, uint32_t>> s_physMap; // phys base -> (size, view)

    void RegisterPhysicalAllocation(uint32_t physBase, uint32_t size, uint32_t viewBase)
    {
        std::lock_guard lock(s_physMapLock);
        s_physMap[physBase] = { size, viewBase };
    }

    void UnregisterPhysicalAllocation(uint32_t physBase)
    {
        std::lock_guard lock(s_physMapLock);
        s_physMap.erase(physBase);
    }

    uint32_t PhysicalToGuest(uint32_t phys)
    {
        phys &= 0x1FFFFFFF;
        std::lock_guard lock(s_physMapLock);
        auto it = s_physMap.upper_bound(phys);
        if (it == s_physMap.begin())
            return 0;
        --it;
        if (phys >= it->first + it->second.first)
            return 0;
        return PhysicalToView(phys, it->second.second);
    }
}
