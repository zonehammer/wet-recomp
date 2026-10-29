// The title module: default.xex loaded into guest memory plus the loader-side
// structures (XEX header copy, LDR_DATA_TABLE_ENTRY) the kernel hands to the game.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wet
{
    struct ImportEntry
    {
        std::string library;    // "xboxkrnl.exe" / "xam.xex"
        std::string name;       // export name, or "<lib>_<ordinal>" if unknown
        uint16_t ordinal = 0;
        uint32_t slot = 0;      // guest address of the import record
        bool variable = false;
    };

    struct ModuleInfo
    {
        uint32_t imageBase = 0, imageSize = 0, entryPoint = 0;
        uint32_t stackSize = 0x40000;
        uint32_t systemFlags = 0;
        uint32_t titleId = 0, mediaId = 0, version = 0, baseVersion = 0;
        uint8_t discNumber = 1, discCount = 1;
        uint32_t tlsSlotCount = 0, tlsRawAddr = 0, tlsDataSize = 0, tlsRawSize = 0;

        uint32_t guestHeader = 0;   // copy of the XEX header in guest memory
        uint32_t guestHeaderSize = 0;
        uint32_t ldrEntry = 0;      // X_LDR_DATA_TABLE_ENTRY
        uint32_t executionId = 0;   // XEX_EXECUTION_ID (XamGetExecutionId)

        std::vector<ImportEntry> imports;
    };

    extern ModuleInfo g_module;

    // Loads (decrypting/decompressing via XenonUtils) and maps the XEX, fills the
    // function table and builds the loader structures.
    bool LoadTitleModule(const std::filesystem::path& xexPath);

    // Optional-header lookup on the guest copy of the header, with
    // RtlImageXexHeaderField semantics. Returns 0 if absent.
    uint32_t XexHeaderField(uint32_t guestHeader, uint32_t key);
}
