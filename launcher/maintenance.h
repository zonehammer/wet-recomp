// Disk space clean-up shared by Setup and the launcher.
//
// An install is <root>\wet-recomp (the source, the extracted disc in
// game\extracted, saves in game\hdd, the build in build) plus, for Setup
// installs, <root>\tools (Git, CMake, Ninja, Clang, Python). Once built, the
// game only needs build\runtime's programs and game\: the compiler's object
// files and libraries, the generated C++ and the tools are only needed to
// build again. Deleting them is safe; the next update downloads the tools and
// rebuilds from scratch (the slow first build again).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace wet::maintenance
{
    namespace fs = std::filesystem;

    struct Part
    {
        std::wstring what;
        std::vector<fs::path> paths;   // files or folders to delete
        uint64_t bytes = 0;
    };

    inline uint64_t SizeOf(const fs::path& p)
    {
        std::error_code ec;
        if (fs::is_regular_file(p, ec))
            return fs::file_size(p, ec);
        uint64_t total = 0;
        for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec))
            if (it->is_regular_file(ec))
                total += it->file_size(ec);
        return total;
    }

    inline bool Lower(const fs::path& p, const wchar_t* ext)
    {
        std::wstring e = p.extension().wstring();
        for (auto& c : e)
            c = wchar_t(towlower(c));
        return e == ext;
    }

    // Build leftovers in a source folder: object files, libraries, precompiled
    // headers and the link map under build\, and the generated C++.
    inline Part Leftovers(const fs::path& repo)
    {
        Part part{ L"Build leftovers (object files, libraries, generated C++)" };
        std::error_code ec;
        fs::path build = repo / L"build";
        if (fs::is_directory(build, ec))
            for (fs::recursive_directory_iterator it(build, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const fs::path& p = it->path();
                if (Lower(p, L".obj") || Lower(p, L".o") || Lower(p, L".a") || Lower(p, L".lib") ||
                    Lower(p, L".pch") || Lower(p, L".map") || Lower(p, L".gch"))
                {
                    part.bytes += it->file_size(ec);
                    part.paths.push_back(p);
                }
            }
        fs::path generated = repo / L"generated" / L"ppc";
        if (fs::is_directory(generated, ec))
        {
            part.bytes += SizeOf(generated);
            part.paths.push_back(generated);
        }
        return part;
    }

    // Setup's downloaded tools (<root>\tools, next to the source folder).
    inline Part Tools(const fs::path& repo)
    {
        Part part{ L"Build tools Setup downloaded (Git, CMake, Ninja, Clang, Python)" };
        std::error_code ec;
        fs::path tools = repo.parent_path() / L"tools";
        // Only the folder Setup made: it holds its tool folders.
        if (fs::is_directory(tools / L"Clang", ec) || fs::is_directory(tools / L"CMake", ec) ||
            fs::is_directory(tools / L"Python", ec))
        {
            part.bytes = SizeOf(tools);
            part.paths.push_back(tools);
        }
        return part;
    }

    // F8 frame captures and the like (bug report material), next to the game.
    inline Part Captures(const fs::path& runtimeDir)
    {
        Part part{ L"F8 frame captures (frames folder)" };
        std::error_code ec;
        fs::path frames = runtimeDir / L"frames";
        if (fs::is_directory(frames, ec))
        {
            part.bytes = SizeOf(frames);
            part.paths.push_back(frames);
        }
        return part;
    }

    // Deletes a part; false if something couldn't be deleted (in use).
    inline bool Delete(const Part& part)
    {
        bool ok = true;
        for (const auto& p : part.paths)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
            if (ec)
                ok = false;
        }
        return ok;
    }

    inline std::wstring Size(uint64_t bytes)
    {
        wchar_t text[32];
        if (bytes >= (1ull << 30))
            swprintf(text, 32, L"%.1f GB", bytes / double(1ull << 30));
        else
            swprintf(text, 32, L"%.0f MB", bytes / double(1ull << 20));
        return text;
    }
}
