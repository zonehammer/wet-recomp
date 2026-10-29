#pragma once
#include <filesystem>

namespace wet
{
    // gameDir: the extracted disc (read-only). hddDir: writable save/cache storage.
    void InitFileSystem(const std::filesystem::path& gameDir, const std::filesystem::path& hddDir);

    std::filesystem::path HddRoot();

    // Mounts a host folder as "<root>:" (XamContentCreate roots such as "save").
    void MountHostFolder(const std::string& root, const std::filesystem::path& host, bool writable);
    bool UnmountHostFolder(const std::string& root);
}
