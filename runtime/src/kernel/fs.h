#pragma once
#include <filesystem>

namespace wet
{
    // gameDir: the extracted disc (read-only). hddDir: writable save/cache storage.
    void InitFileSystem(const std::filesystem::path& gameDir, const std::filesystem::path& hddDir);
}
