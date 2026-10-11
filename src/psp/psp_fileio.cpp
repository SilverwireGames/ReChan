#include "p3d/fileio.h"
#include "psp/psp_perf.h"

#include <pspiofilemgr.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// The folder holding EBOOT.PBP; relative paths resolve under its data/.
char g_gameDir[256] = "ms0:/PSP/GAME/RECHAN";

static std::string ToPspPath(const std::string& input) {
    std::string path = input;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty()) return path;
    if (path.find(":/") == std::string::npos) {
        return std::string(g_gameDir) + "/data/" + path;
    }
    return path;
}

static bool IsDirectory(const SceIoStat& stat) {
    return (stat.st_mode & FIO_S_IFDIR) != 0;
}

} // namespace

namespace p3d::io {

void SetPspGameDir(const char* dir) {
    if (!dir || (std::strncmp(dir, "ms0:/", 5) != 0 && std::strncmp(dir, "ef0:/", 5) != 0)) return;
    size_t len = std::strlen(dir);
    while (len > 5 && dir[len - 1] == '/') len--;
    if (len >= sizeof(g_gameDir)) return;
    std::memcpy(g_gameDir, dir, len);
    g_gameDir[len] = '\0';
}

const char* GetPspGameDir() { return g_gameDir; }

std::string NormalizeSeparators(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return NormalizeSeparators(b);
    if (b.empty()) return NormalizeSeparators(a);
    return NormalizeSeparators(a + (a.back() == '/' ? "" : "/") + b);
}

std::string ResolvePath(const char* path) { return path ? ResolvePath(std::string(path)) : std::string(); }
std::string ResolvePath(const std::string& path) { return ToPspPath(path); }
void InvalidateResolveCache(const std::string&) {}

bool FileExists(const std::string& path) {
    SceIoStat stat{};
    const u32 ioStart = PspPerf::IoClock();
    const bool found = sceIoGetstat(ToPspPath(path).c_str(), &stat) >= 0;
    PspPerf::RecordIo(ioStart, 0, path.c_str());
    return found && !IsDirectory(stat);
}

bool DirExists(const std::string& path) {
    SceIoStat stat{};
    const u32 ioStart = PspPerf::IoClock();
    const bool found = sceIoGetstat(ToPspPath(path).c_str(), &stat) >= 0;
    PspPerf::RecordIo(ioStart, 0, path.c_str());
    return found && IsDirectory(stat);
}

bool CreateDirectories(const std::string& path) {
    const std::string normalized = ToPspPath(path);
    if (normalized.empty() || DirExists(normalized)) return true;
    std::string current;
    const size_t colon = normalized.find(":/");
    if (colon != std::string::npos) current = normalized.substr(0, colon + 2);
    size_t begin = colon == std::string::npos ? 0 : colon + 2;
    while (begin < normalized.size()) {
        const size_t slash = normalized.find('/', begin);
        const std::string segment = normalized.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (!segment.empty()) {
            current += segment;
            if (!DirExists(current) && sceIoMkdir(current.c_str(), 0777) < 0 && !DirExists(current)) return false;
        }
        if (slash == std::string::npos) break;
        current += '/';
        begin = slash + 1;
    }
    return DirExists(normalized);
}

std::vector<DirEntryInfo> ListDirectory(const std::string& path, bool recursive) {
    std::vector<DirEntryInfo> result;
    const std::string root = ToPspPath(path);
    const SceUID handle = sceIoDopen(root.c_str());
    if (handle < 0) return result;
    SceIoDirent entry{};
    while (sceIoDread(handle, &entry) > 0) {
        if (!entry.d_name[0] || std::strcmp(entry.d_name, ".") == 0 || std::strcmp(entry.d_name, "..") == 0) {
            std::memset(&entry, 0, sizeof(entry));
            continue;
        }
        const std::string full = JoinPath(root, entry.d_name);
        const bool directory = IsDirectory(entry.d_stat);
        result.push_back({entry.d_name, full, directory});
        if (recursive && directory) {
            auto children = ListDirectory(full, true);
            result.insert(result.end(), children.begin(), children.end());
        }
        std::memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(handle);
    return result;
}

std::optional<std::vector<uint8_t>> ReadFile(const std::string& path) {
    FileHandle file;
    if (!file.Open(path, FileHandle::Mode::Read)) return std::nullopt;
    std::vector<uint8_t> data(static_cast<size_t>(std::max<int64_t>(0, file.Size())));
    if (!data.empty() && file.Read(data.data(), data.size()) != data.size()) return std::nullopt;
    return data;
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    FileHandle file;
    if (!file.Open(path, FileHandle::Mode::Write, false)) return false;
    return file.Write(data, size) == size;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    return WriteFile(path, data.data(), data.size());
}

std::optional<std::string> ReadTextFile(const std::string& path) {
    const auto bytes = ReadFile(path);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

bool WriteTextFile(const std::string& path, const std::string& text) {
    return WriteFile(path, text.data(), text.size());
}

FileHandle::~FileHandle() { Close(); }

FileHandle::FileHandle(FileHandle&& other) noexcept
    : m_file(other.m_file), m_size(other.m_size) {
    other.m_file = nullptr;
    other.m_size = -1;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        Close();
        m_file = other.m_file;
        m_size = other.m_size;
        other.m_file = nullptr;
        other.m_size = -1;
    }
    return *this;
}

bool FileHandle::Open(const std::string& path, Mode mode, bool) {
    Close();
    int flags = PSP_O_RDONLY;
    if (mode == Mode::Write) flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC;
    if (mode == Mode::Append) flags = PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND;
    const u32 ioStart = PspPerf::IoClock();
    const SceUID handle = sceIoOpen(ToPspPath(path).c_str(), flags, 0777);
    if (handle < 0) {
        PspPerf::RecordIo(ioStart, 0, path.c_str());
        return false;
    }
    m_file = reinterpret_cast<void*>(static_cast<intptr_t>(handle));
    if (mode == Mode::Read) {
        m_size = sceIoLseek(handle, 0, PSP_SEEK_END);
        sceIoLseek(handle, 0, PSP_SEEK_SET);
    }
    else m_size = 0;
    PspPerf::RecordIo(ioStart, 0, path.c_str());
    return true;
}

void FileHandle::Close() {
    if (m_file) {
        sceIoClose(static_cast<SceUID>(reinterpret_cast<intptr_t>(m_file)));
        m_file = nullptr;
    }
    m_size = -1;
}

int64_t FileHandle::Tell() const {
    if (!m_file) return -1;
    return sceIoLseek(static_cast<SceUID>(reinterpret_cast<intptr_t>(m_file)), 0, PSP_SEEK_CUR);
}

bool FileHandle::Seek(int64_t offset, int whence) {
    if (!m_file) return false;
    int origin = PSP_SEEK_SET;
    if (whence == SEEK_CUR) origin = PSP_SEEK_CUR;
    else if (whence == SEEK_END) origin = PSP_SEEK_END;
    const u32 ioStart = PspPerf::IoClock();
    const bool ok = sceIoLseek(static_cast<SceUID>(reinterpret_cast<intptr_t>(m_file)), offset, origin) >= 0;
    PspPerf::RecordIo(ioStart, 0);
    return ok;
}

size_t FileHandle::Read(void* dst, size_t bytes) {
    if (!m_file) return 0;
    const u32 ioStart = PspPerf::IoClock();
    const int result = sceIoRead(static_cast<SceUID>(reinterpret_cast<intptr_t>(m_file)), dst, bytes);
    PspPerf::RecordIo(ioStart, result > 0 ? static_cast<u32>(result) : 0u);
    return result > 0 ? static_cast<size_t>(result) : 0;
}

size_t FileHandle::Write(const void* src, size_t bytes) {
    if (!m_file) return 0;
    const int result = sceIoWrite(static_cast<SceUID>(reinterpret_cast<intptr_t>(m_file)), src, bytes);
    if (result > 0) m_size = std::max<int64_t>(m_size, Tell());
    return result > 0 ? static_cast<size_t>(result) : 0;
}

void FileHandle::Flush() {
    // sceIo writes are synchronous for the memory-stick filesystem; avoid an
    // undocumented per-file ioctl and keep the hot save path portable.
}

std::string GetExecutablePath() { return std::string(g_gameDir) + "/EBOOT.PBP"; }
std::string GetExecutableDir() { return std::string(g_gameDir) + "/data"; }
std::string GetTempDir() { return std::string(g_gameDir) + "/data/userfiles/tmp"; }

} // namespace p3d::io
