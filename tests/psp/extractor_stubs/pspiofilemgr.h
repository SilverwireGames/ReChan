#pragma once
// Host shim: the sceIo calls the disc extractor uses, mapped to POSIX.
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef int64_t SceOff;
typedef unsigned int SceMode;

#define PSP_O_RDONLY 0x0001
#define PSP_SEEK_SET 0
#define FIO_S_IFDIR 0x1000

struct SceIoStat {
    SceMode st_mode;
    unsigned int st_attr;
    SceOff st_size;
};

// Free space reported by sceIoDevctl, in bytes (-1: the call fails).
inline int64_t g_testFreeBytes = int64_t(1) << 40;

inline SceUID sceIoOpen(const char* path, int, SceMode) {
    return open(path, O_RDONLY);
}

inline int sceIoRead(SceUID fd, void* data, SceSize size) {
    SceSize done = 0;
    while (done < size) {
        const ssize_t n = read(fd, static_cast<char*>(data) + done, size - done);
        if (n <= 0) break;
        done += static_cast<SceSize>(n);
    }
    return static_cast<int>(done);
}

inline SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) {
    return lseek(fd, offset, whence);
}

inline int sceIoClose(SceUID fd) { return close(fd); }

inline int sceIoGetstat(const char* path, SceIoStat* stat) {
    struct stat st {};
    if (::stat(path, &st) != 0) return -1;
    stat->st_mode = S_ISDIR(st.st_mode) ? FIO_S_IFDIR : 0;
    stat->st_attr = 0;
    stat->st_size = st.st_size;
    return 0;
}

inline int sceIoRemove(const char* path) { return unlink(path); }

inline int sceIoDevctl(const char*, unsigned int, void* in, int, void*, int) {
    if (g_testFreeBytes < 0) return -1;
    struct Info { uint32_t maxClusters, freeClusters, maxSectors, sectorSize, sectorsPerCluster; };
    Info* info = *static_cast<Info**>(in);
    info->sectorSize = 512;
    info->sectorsPerCluster = 64;
    info->freeClusters = static_cast<uint32_t>(g_testFreeBytes / (512 * 64));
    return 0;
}
