#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace p3d::io {
    std::string NormalizeSeparators(const std::string& path);
    std::string JoinPath(const std::string& a, const std::string& b);

    // Returns the on-disk spelling of each existing path segment. This preserves
    // the original game's case-insensitive asset lookup on case-sensitive
    // filesystems (Linux, and eventually Switch's SD card/RomFS).
    std::string ResolvePath(const char* path);
    std::string ResolvePath(const std::string& path);

    // Call after any write that adds/renames/removes files under `dir` (mod
    // install/uninstall, save writes, asset export) so the next ResolvePath call
    // under that directory re-scans instead of serving a stale cached listing.
    // dir == "" clears the entire cache.
    void InvalidateResolveCache(const std::string& dir = std::string());

    bool FileExists(const std::string& path);
    bool DirExists(const std::string& path);
    bool CreateDirectories(const std::string& path);

    struct DirEntryInfo {
        std::string name;
        std::string fullPath;
        bool isDirectory = false;
    };

    std::vector<DirEntryInfo> ListDirectory(const std::string& path, bool recursive = false);

    std::optional<std::vector<uint8_t>> ReadFile(const std::string& path);
    bool WriteFile(const std::string& path, const void* data, size_t size);
    bool WriteFile(const std::string& path, const std::vector<uint8_t>& data);

    std::optional<std::string> ReadTextFile(const std::string& path);
    bool WriteTextFile(const std::string& path, const std::string& text);

    class FileHandle {
    public:
        enum class Mode { Read, Write, Append };

        FileHandle() = default;
        ~FileHandle();
        FileHandle(FileHandle&& other) noexcept;
        FileHandle& operator=(FileHandle&& other) noexcept;
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;

        // resolveCase should be false for Write/Append opens (never case-resolve
        // a path that's about to be created).
        bool Open(const std::string& path, Mode mode, bool resolveCase = true);
        void Close();
        bool IsOpen() const { return m_file != nullptr; }

        int64_t Size() const { return m_size; }
        int64_t Tell() const;
        bool Seek(int64_t offset, int whence); // whence: SEEK_SET/SEEK_CUR/SEEK_END
        size_t Read(void* dst, size_t bytes);
        size_t Write(const void* src, size_t bytes);
        void Flush();

    private:
        void* m_file = nullptr; // FILE*
        int64_t m_size = -1;
    };

    std::string GetExecutablePath();
    std::string GetExecutableDir();
    std::string GetTempDir();

#if defined(RC_PLATFORM_PSP)
    // The folder holding EBOOT.PBP ("ms0:/PSP/GAME/RECHAN" by default);
    // relative paths resolve under its data/ folder.
    void SetPspGameDir(const char* dir);
    const char* GetPspGameDir();
#endif

}
