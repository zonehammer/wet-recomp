#include "module.h"
#include "kernel/kernel.h"

#include <iterator>
#include <fstream>
#include <image.h>
#include <xex.h>

namespace wet
{
    ModuleInfo g_module;

    namespace
    {
        enum ExportKind { kFunction, kVariable };
        struct ExportDef { uint32_t ordinal; const char* name; ExportKind kind; };

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { ORDINAL, #NAME, TYPE }
        const ExportDef kKernelExports[] = {
#include "xbox/xboxkrnl_table.inc"
        };
        const ExportDef kXamExports[] = {
#include "xbox/xam_table.inc"
        };
#undef XE_EXPORT

        const ExportDef* FindExport(const std::string& lib, uint32_t ordinal)
        {
            if (lib == "xboxkrnl.exe")
            {
                for (auto& e : kKernelExports)
                    if (e.ordinal == ordinal) return &e;
            }
            else if (lib == "xam.xex")
            {
                for (auto& e : kXamExports)
                    if (e.ordinal == ordinal) return &e;
            }
            return nullptr;
        }

        uint32_t HeaderValue(const uint8_t* xex, uint32_t key, uint32_t def = 0)
        {
            auto* p = static_cast<const be<uint32_t>*>(getOptHeaderPtr(xex, key));
            return p ? uint32_t(*p) : def;
        }

        uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

        uint32_t AllocWideString(const std::u16string& s)
        {
            uint32_t a = SystemAlloc(uint32_t(s.size() + 1) * 2);
            WriteWideCString(a, s);
            return a;
        }
    }

    uint32_t XexHeaderField(uint32_t guestHeader, uint32_t key)
    {
        uint32_t count = Ld32(guestHeader + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t entry = guestHeader + 0x18 + i * 8;
            if (Ld32(entry) != key)
                continue;
            switch (key & 0xFF)
            {
            case 0x00: return Ld32(entry + 4);          // the value itself
            case 0x01: return entry + 4;                // pointer to the value
            default:   return guestHeader + Ld32(entry + 4); // pointer to the data
            }
        }
        return 0;
    }

    bool LoadTitleModule(const std::filesystem::path& xexPath)
    {
        std::ifstream f(xexPath, std::ios::binary);
        if (!f)
        {
            LogFatal("Cannot open %s", xexPath.string().c_str());
            return false;
        }
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (file.size() < 0x100 || std::memcmp(file.data(), "XEX2", 4) != 0)
        {
            LogFatal("%s is not an XEX2 file", xexPath.string().c_str());
            return false;
        }

        Image image = Xex2LoadImage(file.data(), file.size());
        if (!image.data || image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
        {
            LogFatal("XEX image mismatch: base 0x%zX size 0x%X (recompiled code expects base 0x%llX size <= 0x%llX).\n"
                     "Is this the same default.xex the code was generated from?",
                     image.base, image.size, (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE);
            return false;
        }

        // ---- Image + function table ----
        g_mem.Commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        std::memcpy(HostPtr(PPC_IMAGE_BASE), image.data.get(), image.size);

        uint32_t tableBase = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
        g_mem.Commit(tableBase, uint32_t(PPC_CODE_SIZE * 2));
        size_t mapped = 0;
        for (auto* m = PPCFuncMappings; m->host != nullptr; ++m, ++mapped)
            PPC_LOOKUP_FUNC(g_mem.base, m->guest) = m->host;

        const uint8_t* xex = file.data();
        auto* hdr = reinterpret_cast<const Xex2Header*>(xex);
        g_module.imageBase = uint32_t(image.base);
        g_module.imageSize = image.size;
        g_module.entryPoint = uint32_t(image.entry_point);
        g_module.stackSize = HeaderValue(xex, XEX_HEADER_DEFAULT_STACK_SIZE, 0x40000);
        g_module.systemFlags = HeaderValue(xex, XEX_HEADER_SYSTEM_FLAGS);

        if (auto* ei = static_cast<const uint8_t*>(getOptHeaderPtr(xex, XEX_HEADER_EXECUTION_INFO)))
        {
            g_module.mediaId = BE32(ei + 0);
            g_module.version = BE32(ei + 4);
            g_module.baseVersion = BE32(ei + 8);
            g_module.titleId = BE32(ei + 12);
            g_module.discNumber = ei[18];
            g_module.discCount = ei[19];
        }
        if (auto* tls = static_cast<const uint8_t*>(getOptHeaderPtr(xex, XEX_HEADER_TLS_INFO)))
        {
            g_module.tlsSlotCount = BE32(tls + 0);
            g_module.tlsRawAddr = BE32(tls + 4);
            g_module.tlsDataSize = BE32(tls + 8);
            g_module.tlsRawSize = BE32(tls + 12);
        }

        // ---- Imports ----
        if (auto* ih = static_cast<const uint8_t*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES)))
        {
            uint32_t strSize = BE32(ih + 4), libCount = BE32(ih + 8);
            std::vector<std::string> names;
            const char* strs = reinterpret_cast<const char*>(ih + 12);
            for (uint32_t i = 0, off = 0; i < libCount; i++)
            {
                names.emplace_back(strs + off);
                off += (uint32_t(names.back().size()) + 1 + 3) & ~3u;
            }
            const uint8_t* lib = ih + 12 + strSize;
            for (uint32_t l = 0; l < libCount; l++)
            {
                uint32_t size = BE32(lib);
                std::string libName = names[BE16(lib + 0x24)];
                uint16_t count = BE16(lib + 0x26);
                for (uint16_t i = 0; i < count; i++)
                {
                    uint32_t slot = BE32(lib + 0x28 + i * 4);
                    // XenonUtils byte-swapped every record to host order while loading,
                    // and replaced function thunks with code; the ordinal survives in the
                    // low 16 bits of data records. Decide variable-ness from the export table.
                    uint32_t rec;
                    std::memcpy(&rec, image.data.get() + (slot - image.base), 4);
                    uint16_t ordinal = uint16_t(rec & 0xFFFF);
                    const ExportDef* e = FindExport(libName, ordinal);
                    if (!e || e->kind != kVariable || (rec >> 24) != 0)
                        continue;
                    g_module.imports.push_back({ libName, e->name, ordinal, slot, true });
                }
                lib += size;
            }
        }

        // ---- XEX header copy, execution id, loader entry ----
        g_module.guestHeaderSize = hdr->headerSize;
        g_module.guestHeader = SystemAlloc(g_module.guestHeaderSize, 0x1000);
        std::memcpy(HostPtr(g_module.guestHeader), xex, g_module.guestHeaderSize);
        g_module.executionId = XexHeaderField(g_module.guestHeader, XEX_HEADER_EXECUTION_INFO);

        // X_LDR_DATA_TABLE_ENTRY (Xenia layout)
        uint32_t ldr = SystemAlloc(0x80);
        uint32_t name = AllocWideString(u"default.xex");
        St32(ldr + 0x00, ldr); St32(ldr + 0x04, ldr);           // in_load_order_links
        St32(ldr + 0x08, ldr + 8); St32(ldr + 0x0C, ldr + 8);   // in_memory_order_links
        St32(ldr + 0x10, ldr + 16); St32(ldr + 0x14, ldr + 16); // in_initialization_order_links
        St32(ldr + 0x18, g_module.imageBase);                   // dll_base
        St32(ldr + 0x1C, g_module.imageBase);                   // image_base
        St32(ldr + 0x20, g_module.imageSize);                   // image_size
        St16(ldr + 0x24, 22); St16(ldr + 0x26, 24); St32(ldr + 0x28, name); // full_dll_name
        St16(ldr + 0x2C, 22); St16(ldr + 0x2E, 24); St32(ldr + 0x30, name); // base_dll_name
        St32(ldr + 0x34, 0);                                    // flags
        St32(ldr + 0x38, g_module.imageSize);                   // full_image_size
        St32(ldr + 0x3C, g_module.entryPoint);                  // entry_point
        St16(ldr + 0x40, 1);                                    // load_count
        St32(ldr + 0x58, g_module.guestHeader);                 // xex_header_base
        g_module.ldrEntry = ldr;

        Log("Loaded %s: title %08X v%u disc %u/%u, image 0x%08X+0x%X, entry 0x%08X, %zu functions mapped, "
            "%zu kernel variables, stack 0x%X",
            xexPath.filename().string().c_str(), g_module.titleId, g_module.version, g_module.discNumber,
            g_module.discCount, g_module.imageBase, g_module.imageSize, g_module.entryPoint, mapped,
            g_module.imports.size(), g_module.stackSize);
        return true;
    }
}
