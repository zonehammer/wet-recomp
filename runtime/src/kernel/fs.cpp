// Virtual file system + Nt*File imports.
//
// Guest paths look like "game:\media\foo.lu", "d:\...", "\Device\Cdrom0\..." or
// "\??\cache:\...". Drive-style prefixes go through a symbolic-link table, then
// device prefixes map onto host folders:
//   \Device\Cdrom0                  -> game/extracted (read-only)
//   \Device\Harddisk0\Partition1    -> game/hdd       (read/write)
//   \Device\Harddisk0\Cache0        -> game/hdd/cache (read/write)
#include "fs.h"
#include "kernel.h"
#include "objects.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>

namespace fs = std::filesystem;
using namespace wet;

namespace
{
    constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x01;
    constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
    constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;
    constexpr uint32_t STATUS_BUFFER_OVERFLOW = 0x80000005;

    struct Device
    {
        std::string prefix;     // lower-case, e.g. "\device\cdrom0"
        fs::path root;
        bool writable;
        const char* fsName;
    };

    std::mutex s_vfsLock;
    std::vector<Device> s_devices;
    std::map<std::string, std::string> s_symlinks; // "game:" -> "\device\cdrom0"
    uint64_t s_fixedFileTime = 0;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
        return s;
    }

    std::string NormalizeGuest(std::string p)
    {
        std::replace(p.begin(), p.end(), '/', '\\');
        if (p.rfind("\\??\\", 0) == 0)
            p = p.substr(4);
        // Collapse duplicate separators
        std::string r;
        for (char c : p)
            if (!(c == '\\' && !r.empty() && r.back() == '\\'))
                r.push_back(c);
        return r;
    }

    // Resolve one component against a host directory, case-insensitively.
    fs::path MatchComponent(const fs::path& dir, const std::string& name)
    {
        std::error_code ec;
        fs::path exact = dir / name;
        if (fs::exists(exact, ec))
            return exact;
        std::string want = Lower(name);
        for (auto& e : fs::directory_iterator(dir, ec))
            if (Lower(e.path().filename().string()) == want)
                return e.path();
        return exact;
    }

    struct Resolved
    {
        fs::path host;
        const Device* device = nullptr;
        bool ok = false;
    };

    Resolved ResolveGuestPath(const std::string& guestIn)
    {
        std::string p = NormalizeGuest(guestIn);
        std::string lp = Lower(p);

        std::lock_guard lock(s_vfsLock);
        // Drive / symbolic link ("game:\x")
        size_t colon = lp.find(':');
        if (colon != std::string::npos && lp.find('\\') > colon)
        {
            auto it = s_symlinks.find(lp.substr(0, colon + 1));
            if (it == s_symlinks.end())
                return {};
            p = it->second + p.substr(colon + 1);
            lp = Lower(p);
        }

        const Device* best = nullptr;
        for (auto& d : s_devices)
        {
            if (lp.rfind(d.prefix, 0) == 0 && (lp.size() == d.prefix.size() || lp[d.prefix.size()] == '\\'))
                if (!best || d.prefix.size() > best->prefix.size())
                    best = &d;
        }
        if (!best)
            return {};

        fs::path host = best->root;
        std::string rest = p.substr(best->prefix.size());
        size_t i = 0;
        while (i < rest.size())
        {
            size_t j = rest.find('\\', i);
            if (j == std::string::npos) j = rest.size();
            std::string comp = rest.substr(i, j - i);
            i = j + 1;
            if (comp.empty() || comp == ".")
                continue;
            if (comp == "..")
                return {}; // no escaping the device root
            host = MatchComponent(host, comp);
        }
        return { host, best, true };
    }

    struct KFile : KObject
    {
        KFile() : KObject(ObjType::File) {}
        ~KFile() override { if (fp) std::fclose(fp); }

        std::string guestPath;
        fs::path hostPath;
        const Device* device = nullptr;
        bool isDirectory = false;
        bool deleteOnClose = false;
        std::FILE* fp = nullptr;
        uint64_t position = 0;
        std::mutex lock;

        // Directory enumeration
        bool listed = false;
        std::vector<fs::directory_entry> entries;
        size_t next = 0;
        std::string pattern;
    };

    bool SeekTo(std::FILE* fp, uint64_t pos)
    {
#ifdef _WIN32
        return _fseeki64(fp, int64_t(pos), SEEK_SET) == 0;
#else
        return fseeko(fp, off_t(pos), SEEK_SET) == 0;
#endif
    }

    uint64_t FileSize(const KFile& f)
    {
        std::error_code ec;
        if (f.isDirectory)
            return 0;
        if (f.fp)
            std::fflush(f.fp);
        auto s = fs::file_size(f.hostPath, ec);
        return ec ? 0 : uint64_t(s);
    }

    uint32_t Attributes(const KFile& f)
    {
        if (f.isDirectory)
            return FILE_ATTRIBUTE_DIRECTORY;
        return f.device && !f.device->writable ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    }

    // FILE_NETWORK_OPEN_INFORMATION (0x38)
    void WriteNetworkOpenInfo(uint32_t p, uint64_t size, uint32_t attrs)
    {
        for (int i = 0; i < 4; i++)
            St64(p + i * 8, s_fixedFileTime);
        St64(p + 0x20, (size + 0xFFF) & ~0xFFFull);
        St64(p + 0x28, size);
        St32(p + 0x30, attrs);
        St32(p + 0x34, 0);
    }

    void CompleteIo(PPCContext& ctx, uint32_t ioStatus, uint32_t eventHandle, uint32_t apc, uint32_t status, uint32_t info)
    {
        if (ioStatus)
        {
            St32(ioStatus, status);
            St32(ioStatus + 4, info);
        }
        if (eventHandle)
        {
            if (auto e = LookupHandleAs<KEvent>(eventHandle, ObjType::Event))
            {
                {
                    std::lock_guard lock(DispatchLock());
                    e->Set(true);
                }
                DispatchNotify();
            }
        }
        if (apc)
            TraceCall("NtReadFile/NtWriteFile", ctx, "APC completion routines are not supported yet");
    }

    std::string PathFromObjectAttributes(uint32_t objAttr)
    {
        uint32_t root = Ld32(objAttr + 0);
        std::string name = ReadAnsiString(Ld32(objAttr + 4));
        if (root)
        {
            auto rootFile = LookupHandleAs<KFile>(root, ObjType::File);
            if (rootFile)
                name = rootFile->guestPath + "\\" + name;
        }
        return name;
    }

    bool WildcardMatch(const std::string& pat, const std::string& s)
    {
        size_t p = 0, i = 0, star = std::string::npos, mark = 0;
        while (i < s.size())
        {
            if (p < pat.size() && (pat[p] == '?' || tolower(pat[p]) == tolower(s[i]))) { p++; i++; }
            else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
            else if (star != std::string::npos) { p = star + 1; i = ++mark; }
            else return false;
        }
        while (p < pat.size() && pat[p] == '*') p++;
        return p == pat.size();
    }

    // Shared by NtCreateFile/NtOpenFile.
    uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t objAttr, uint32_t ioStatus,
                      uint32_t disposition, uint32_t options)
    {
        constexpr uint32_t FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40;
        enum { SUPERSEDE, OPEN, CREATE, OPEN_IF, OVERWRITE, OVERWRITE_IF };

        std::string guest = PathFromObjectAttributes(objAttr);
        Resolved r = ResolveGuestPath(guest);
        auto fail = [&](uint32_t status) {
            Log("open \"%s\" -> 0x%08X", guest.c_str(), status);
            if (ioStatus) { St32(ioStatus, status); St32(ioStatus + 4, 0); }
            return status;
        };
        if (!r.ok)
            return fail(STATUS_OBJECT_PATH_NOT_FOUND);

        std::error_code ec;
        bool exists = fs::exists(r.host, ec);
        bool isDir = exists && fs::is_directory(r.host, ec);
        bool wantsWrite = disposition != OPEN || (access & 0x40000000 /* GENERIC_WRITE */) || (access & 0x2 /* FILE_WRITE_DATA */);
        uint32_t info = 1; // FILE_OPENED

        if (isDir && (options & FILE_NON_DIRECTORY_FILE))
            return fail(STATUS_FILE_IS_A_DIRECTORY);
        if (exists && !isDir && (options & FILE_DIRECTORY_FILE))
            return fail(STATUS_NOT_A_DIRECTORY);
        if (!exists && (disposition == OPEN || disposition == OVERWRITE))
            return fail(fs::exists(r.host.parent_path(), ec) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND);
        if (exists && disposition == CREATE)
            return fail(STATUS_OBJECT_NAME_COLLISION);
        if (!r.device->writable && (!exists || (wantsWrite && !isDir && disposition != OPEN && disposition != OPEN_IF)))
            return fail(STATUS_ACCESS_DENIED);

        auto f = std::make_shared<KFile>();
        f->guestPath = NormalizeGuest(guest);
        f->hostPath = r.host;
        f->device = r.device;

        if (isDir || (!exists && (options & FILE_DIRECTORY_FILE)))
        {
            if (!exists)
            {
                if (!fs::create_directories(r.host, ec))
                    return fail(STATUS_ACCESS_DENIED);
                info = 2; // FILE_CREATED
            }
            f->isDirectory = true;
        }
        else
        {
            const char* mode = "rb";
            if (!exists) { mode = "w+b"; info = 2; }
            else if (disposition == SUPERSEDE || disposition == OVERWRITE || disposition == OVERWRITE_IF)
            { mode = "w+b"; info = disposition == SUPERSEDE ? 0 : 3; }
            else if (r.device->writable) mode = "r+b";
            if (!exists)
                fs::create_directories(r.host.parent_path(), ec);
            f->fp = std::fopen(r.host.string().c_str(), mode);
            if (!f->fp && std::string(mode) == "r+b")
                f->fp = std::fopen(r.host.string().c_str(), "rb");
            if (!f->fp)
                return fail(STATUS_ACCESS_DENIED);
        }

        St32(handlePtr, CreateHandle(f));
        if (ioStatus) { St32(ioStatus, STATUS_SUCCESS); St32(ioStatus + 4, info); }
        Log("open \"%s\" -> %s%s", guest.c_str(), r.host.string().c_str(), f->isDirectory ? " (dir)" : "");
        return STATUS_SUCCESS;
    }
}

namespace wet
{
    void InitFileSystem(const fs::path& gameDir, const fs::path& hddDir)
    {
        std::error_code ec;
        fs::create_directories(hddDir / "cache", ec);
        s_fixedFileTime = CurrentFileTime();
        std::lock_guard lock(s_vfsLock);
        s_devices = {
            { "\\device\\cdrom0", gameDir, false, "GDFX" },
            { "\\device\\harddisk0\\partition1", hddDir, true, "FATX" },
            { "\\device\\harddisk0\\cache0", hddDir / "cache", true, "FATX" },
        };
        s_symlinks = {
            { "game:", "\\device\\cdrom0" },
            { "d:", "\\device\\cdrom0" },
            { "hdd:", "\\device\\harddisk0\\partition1" },
            { "cache:", "\\device\\harddisk0\\cache0" },
        };
        Log("VFS: game: -> %s (read-only), hdd: -> %s", gameDir.string().c_str(), hddDir.string().c_str());
    }
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------

// NtCreateFile(handle*, access, obj_attr*, io_status*, allocation_size*, file_attributes,
//              share_access, creation_disposition, create_options)
GUEST_IMPORT(NtCreateFile)
{
    SetCurrentImport("NtCreateFile");
    Ret(ctx, OpenFile(Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3), Arg(ctx, 7), Arg(ctx, 8)));
}

// NtOpenFile(handle*, access, obj_attr*, io_status*, share_access, open_options)
GUEST_IMPORT(NtOpenFile)
{
    SetCurrentImport("NtOpenFile");
    Ret(ctx, OpenFile(Arg(ctx, 0), Arg(ctx, 1), Arg(ctx, 2), Arg(ctx, 3), 1 /* FILE_OPEN */, Arg(ctx, 5)));
}

// NtReadFile(handle, event, apc_routine, apc_context, io_status*, buffer, length, byte_offset*)
GUEST_IMPORT(NtReadFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f || !f->fp)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t buffer = Arg(ctx, 5), length = Arg(ctx, 6), offPtr = Arg(ctx, 7);
    size_t got;
    {
        std::lock_guard lock(f->lock);
        if (offPtr)
        {
            uint64_t off = Ld64(offPtr);
            if (off != 0xFFFFFFFFFFFFFFFEull) // FILE_USE_FILE_POINTER_POSITION
                f->position = off;
        }
        SeekTo(f->fp, f->position);
        got = std::fread(HostPtr(buffer), 1, length, f->fp);
        f->position += got;
    }
    uint32_t status = (got == 0 && length > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    CompleteIo(ctx, Arg(ctx, 4), Arg(ctx, 1), Arg(ctx, 2), status, uint32_t(got));
    Ret(ctx, status);
}

// NtReadFileScatter(handle, event, apc, apc_ctx, io_status*, FILE_SEGMENT_ELEMENT[], length, byte_offset*)
// Each segment is one 4 KB page; elements are 8 bytes (64-bit pointer, low half used).
GUEST_IMPORT(NtReadFileScatter)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f || !f->fp)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t segs = Arg(ctx, 5), length = Arg(ctx, 6), offPtr = Arg(ctx, 7);
    size_t total = 0;
    {
        std::lock_guard lock(f->lock);
        if (offPtr && Ld64(offPtr) != 0xFFFFFFFFFFFFFFFEull)
            f->position = Ld64(offPtr);
        SeekTo(f->fp, f->position);
        for (uint32_t i = 0; total < length; i++)
        {
            uint32_t page = Ld32(segs + i * 8 + 4);
            size_t want = std::min<size_t>(0x1000, length - total);
            size_t got = std::fread(HostPtr(page), 1, want, f->fp);
            total += got;
            if (got < want)
                break;
        }
        f->position += total;
    }
    TraceCall("NtReadFileScatter", ctx, "segment layout unverified");
    uint32_t status = (total == 0 && length > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    CompleteIo(ctx, Arg(ctx, 4), Arg(ctx, 1), Arg(ctx, 2), status, uint32_t(total));
    Ret(ctx, status);
}

// NtWriteFile(handle, event, apc_routine, apc_context, io_status*, buffer, length, byte_offset*)
GUEST_IMPORT(NtWriteFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f || !f->fp)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    if (!f->device->writable)
        return Ret(ctx, STATUS_ACCESS_DENIED);
    uint32_t buffer = Arg(ctx, 5), length = Arg(ctx, 6), offPtr = Arg(ctx, 7);
    size_t put;
    {
        std::lock_guard lock(f->lock);
        if (offPtr && Ld64(offPtr) != 0xFFFFFFFFFFFFFFFEull)
            f->position = Ld64(offPtr);
        SeekTo(f->fp, f->position);
        put = std::fwrite(HostPtr(buffer), 1, length, f->fp);
        f->position += put;
    }
    CompleteIo(ctx, Arg(ctx, 4), Arg(ctx, 1), Arg(ctx, 2), STATUS_SUCCESS, uint32_t(put));
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (f && f->fp)
        std::fflush(f->fp);
    if (Arg(ctx, 1)) { St32(Arg(ctx, 1), STATUS_SUCCESS); St32(Arg(ctx, 1) + 4, 0); }
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryInformationFile(handle, io_status*, info*, length, class)
GUEST_IMPORT(NtQueryInformationFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t io = Arg(ctx, 1), info = Arg(ctx, 2), cls = Arg(ctx, 4);
    uint64_t size = FileSize(*f);
    uint32_t written;
    switch (cls)
    {
    case 4: // FileBasicInformation
        for (int i = 0; i < 4; i++) St64(info + i * 8, s_fixedFileTime);
        St32(info + 0x20, Attributes(*f));
        St32(info + 0x24, 0);
        written = 0x28;
        break;
    case 5: // FileStandardInformation
        St64(info + 0x00, (size + 0xFFF) & ~0xFFFull);
        St64(info + 0x08, size);
        St32(info + 0x10, 1);
        St8(info + 0x14, f->deleteOnClose);
        St8(info + 0x15, f->isDirectory);
        St16(info + 0x16, 0);
        written = 0x18;
        break;
    case 6: // FileInternalInformation
        St64(info, std::hash<std::string>{}(f->hostPath.string()));
        written = 8;
        break;
    case 14: // FilePositionInformation
        St64(info, f->position);
        written = 8;
        break;
    case 20: // FileEndOfFileInformation
        St64(info, size);
        written = 8;
        break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(info, size, Attributes(*f));
        written = 0x38;
        break;
    default:
        Log("NtQueryInformationFile: unsupported class %u on \"%s\"", cls, f->guestPath.c_str());
        return Ret(ctx, STATUS_INVALID_INFO_CLASS);
    }
    if (io) { St32(io, STATUS_SUCCESS); St32(io + 4, written); }
    Ret(ctx, STATUS_SUCCESS);
}

// NtSetInformationFile(handle, io_status*, info*, length, class)
GUEST_IMPORT(NtSetInformationFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t io = Arg(ctx, 1), info = Arg(ctx, 2), cls = Arg(ctx, 4);
    std::error_code ec;
    switch (cls)
    {
    case 14: // FilePositionInformation
    {
        std::lock_guard lock(f->lock);
        f->position = Ld64(info);
        break;
    }
    case 19: // FileAllocationInformation
    case 20: // FileEndOfFileInformation
        if (!f->device->writable)
            return Ret(ctx, STATUS_ACCESS_DENIED);
        if (f->fp) std::fflush(f->fp);
        fs::resize_file(f->hostPath, Ld64(info), ec);
        break;
    case 13: // FileDispositionInformation
        f->deleteOnClose = Ld8(info) != 0;
        if (f->deleteOnClose)
            TraceCall("NtSetInformationFile", ctx, "delete-on-close requested (ignored)");
        break;
    case 4:  // FileBasicInformation (timestamps/attributes): ignore
        break;
    default:
        Log("NtSetInformationFile: unsupported class %u on \"%s\"", cls, f->guestPath.c_str());
        return Ret(ctx, STATUS_INVALID_INFO_CLASS);
    }
    if (io) { St32(io, STATUS_SUCCESS); St32(io + 4, 0); }
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryFullAttributesFile(obj_attr*, FILE_NETWORK_OPEN_INFORMATION*)
GUEST_IMPORT(NtQueryFullAttributesFile)
{
    std::string guest = PathFromObjectAttributes(Arg(ctx, 0));
    Resolved r = ResolveGuestPath(guest);
    std::error_code ec;
    if (!r.ok || !fs::exists(r.host, ec))
        return Ret(ctx, STATUS_OBJECT_NAME_NOT_FOUND);
    bool dir = fs::is_directory(r.host, ec);
    uint64_t size = dir ? 0 : uint64_t(fs::file_size(r.host, ec));
    WriteNetworkOpenInfo(Arg(ctx, 1), size,
                         dir ? FILE_ATTRIBUTE_DIRECTORY : (r.device->writable ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY));
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryVolumeInformationFile(handle, io_status*, info*, length, class)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t io = Arg(ctx, 1), info = Arg(ctx, 2), cls = Arg(ctx, 4), written;
    switch (cls)
    {
    case 1: // FileFsVolumeInformation
        St64(info, s_fixedFileTime);
        St32(info + 8, 0x12345678);   // serial
        St32(info + 12, 0);           // label length
        St8(info + 16, 0);            // supports objects
        written = 0x14;
        break;
    case 3: // FileFsSizeInformation: 16 KB clusters
        St64(info + 0, 0x40000);      // total units (4 GB)
        St64(info + 8, 0x20000);      // free units (2 GB)
        St32(info + 16, 32);          // sectors per unit
        St32(info + 20, 512);         // bytes per sector
        written = 0x18;
        break;
    case 5: // FileFsAttributeInformation
    {
        const char* name = f->device->fsName;
        St32(info + 0, 0);
        St32(info + 4, 255);
        St32(info + 8, uint32_t(strlen(name)));
        std::memcpy(HostPtr(info + 12), name, strlen(name));
        written = 12 + uint32_t(strlen(name));
        break;
    }
    default:
        Log("NtQueryVolumeInformationFile: unsupported class %u", cls);
        return Ret(ctx, STATUS_INVALID_INFO_CLASS);
    }
    if (io) { St32(io, STATUS_SUCCESS); St32(io + 4, written); }
    Ret(ctx, STATUS_SUCCESS);
}

// NtQueryDirectoryFile(handle, event, apc, apc_ctx, io_status*, info*, length, file_name*, restart_scan)
// Returns one FILE_DIRECTORY_INFORMATION entry per call (ANSI names on the 360).
GUEST_IMPORT(NtQueryDirectoryFile)
{
    auto f = LookupHandleAs<KFile>(Arg(ctx, 0), ObjType::File);
    if (!f || !f->isDirectory)
        return Ret(ctx, STATUS_INVALID_HANDLE);
    uint32_t io = Arg(ctx, 4), info = Arg(ctx, 5), length = Arg(ctx, 6), namePtr = Arg(ctx, 7);
    bool restart = Arg(ctx, 8) != 0;

    std::lock_guard lock(f->lock);
    if (!f->listed || restart)
    {
        std::error_code ec;
        f->entries.clear();
        for (auto& e : fs::directory_iterator(f->hostPath, ec))
            f->entries.push_back(e);
        std::sort(f->entries.begin(), f->entries.end(),
                  [](auto& a, auto& b) { return a.path().filename().string() < b.path().filename().string(); });
        f->pattern = namePtr ? ReadAnsiString(namePtr) : "*";
        if (f->pattern.empty())
            f->pattern = "*";
        f->next = 0;
        f->listed = true;
    }

    while (f->next < f->entries.size())
    {
        auto& e = f->entries[f->next];
        std::string name = e.path().filename().string();
        if (!WildcardMatch(f->pattern, name))
        {
            f->next++;
            continue;
        }
        if (length < 0x40 + name.size())
            return Ret(ctx, STATUS_BUFFER_OVERFLOW);
        f->next++;
        std::error_code ec;
        bool dir = e.is_directory(ec);
        uint64_t size = dir ? 0 : uint64_t(e.file_size(ec));
        St32(info + 0x00, 0);                    // next entry offset
        St32(info + 0x04, uint32_t(f->next));    // file index
        for (int i = 0; i < 4; i++)
            St64(info + 0x08 + i * 8, s_fixedFileTime);
        St64(info + 0x28, size);
        St64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
        St32(info + 0x38, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
        St32(info + 0x3C, uint32_t(name.size()));
        std::memcpy(HostPtr(info + 0x40), name.data(), name.size());
        if (io) { St32(io, STATUS_SUCCESS); St32(io + 4, 0x40 + uint32_t(name.size())); }
        return Ret(ctx, STATUS_SUCCESS);
    }
    if (io) { St32(io, STATUS_NO_MORE_FILES); St32(io + 4, 0); }
    Ret(ctx, STATUS_NO_MORE_FILES);
}

// ObCreateSymbolicLink(ANSI_STRING* link, ANSI_STRING* target)
GUEST_IMPORT(ObCreateSymbolicLink)
{
    std::string link = Lower(NormalizeGuest(ReadAnsiString(Arg(ctx, 0))));
    std::string target = Lower(NormalizeGuest(ReadAnsiString(Arg(ctx, 1))));
    if (!link.empty() && link.back() == '\\')
        link.pop_back();
    while (!target.empty() && target.back() == '\\')
        target.pop_back();
    {
        std::lock_guard lock(s_vfsLock);
        s_symlinks[link] = target;
    }
    Log("ObCreateSymbolicLink %s -> %s", link.c_str(), target.c_str());
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(ObDeleteSymbolicLink)
{
    std::string link = Lower(NormalizeGuest(ReadAnsiString(Arg(ctx, 0))));
    std::lock_guard lock(s_vfsLock);
    s_symlinks.erase(link);
    Ret(ctx, STATUS_SUCCESS);
}

GUEST_IMPORT(IoDismountVolume) { Ret(ctx, STATUS_SUCCESS); }
GUEST_IMPORT(IoDismountVolumeByFileHandle) { Ret(ctx, STATUS_SUCCESS); }
