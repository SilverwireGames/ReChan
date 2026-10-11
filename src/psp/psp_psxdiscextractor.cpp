// PSP disc extractor: the same whitelist, layout detection and serial check
// as src/extra/psxdiscextractor.cpp, but each file streams through a batch of
// sectors instead of being loaded whole (the largest movie is 99 MB), on a
// kernel thread so the menu keeps drawing the progress bar.
#include "pc/log.h"
#include "extra/psxdiscextractor.h"
#include "p3d/byteread.h"
#include "p3d/fileio.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

PsxDiscExtractor* g_psxDiscExtractor = nullptr;

namespace {

enum class RawKind { None, Raw2352, Raw2336 };

struct SectorLayout {
    s32 sectorSize;
    s32 dataOffset;
    RawKind rawKind;
};

const SectorLayout kLayouts[] = {
    { 2048, 0,  RawKind::None },
    { 2352, 16, RawKind::Raw2352 },
    { 2352, 24, RawKind::Raw2352 },
    { 2336, 8,  RawKind::Raw2336 },
};

const char* const kWhitelistDirs[] = { "fe", "rchars", "rtarget", "scr", "sound", "tim", "xc" };
const char* const kWhitelistRootFiles[] = {
    "license.tim", "license_data.dat", "loadanim.con", "postdemo.tim", "predemo.tim", "runfirst.tim",
};

constexpr const char* kExpectedSerial = "SLUS-00684";
// AreAssetsPresent() needs this file, so it is written last: an extraction
// cut short by HOME or a flat battery is offered again on the next boot.
constexpr const char* kSentinelFile = "license.tim";
constexpr u32 kLogicalBlock = 2048;
constexpr u32 kXaSectorBytes = 2336;
constexpr u32 kBatchSectors = 64;
// Room left free on the card for the settings, saves and the FAT itself.
constexpr s64 kSpaceMargin = 4 * 1024 * 1024;
constexpr int kThreadPriority = 0x30;
constexpr int kThreadStack = 0x10000;

struct DirEntry {
    std::string path; // lowercase, '/'-separated, without the ";1" suffix
    bool isDir = false;
    u32 lba = 0;
    u32 size = 0;
};

std::string ToLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool ShouldExtract(const std::string& path) {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) {
        for (const char* name : kWhitelistRootFiles) {
            if (path == name) return true;
        }
        return false;
    }
    const std::string top = path.substr(0, slash);
    for (const char* name : kWhitelistDirs) {
        if (top == name) return true;
    }
    return false;
}

std::string DiscImageDir() {
    return std::string(p3d::io::GetPspGameDir()) + "/" + PsxDiscExtractor::kDiscImageDir;
}

s64 FreeBytes(const char* device) {
    struct {
        u32 maxClusters;
        u32 freeClusters;
        u32 maxSectors;
        u32 sectorSize;
        u32 sectorsPerCluster;
    } info{};
    void* arg = &info;
    if (sceIoDevctl(device, 0x02425818, &arg, sizeof(arg), nullptr, 0) < 0) {
        return -1;
    }
    return (s64)info.freeClusters * info.sectorsPerCluster * info.sectorSize;
}

class DiscReader {
public:
    ~DiscReader() {
        if (m_fd >= 0) sceIoClose(m_fd);
    }

    bool Open(const std::string& path, std::string& error) {
        m_fd = sceIoOpen(path.c_str(), PSP_O_RDONLY, 0);
        if (m_fd < 0) {
            error = "Could not open the disc image";
            return false;
        }
        for (const SectorLayout& layout : kLayouts) {
            m_layout = layout;
            m_raw.resize((size_t)layout.sectorSize);
            if (ReadRaw(16, 1)) {
                const u8* pvd = Data(0);
                if (pvd[0] == 0x01 && std::memcmp(pvd + 1, "CD001", 5) == 0 && pvd[6] == 0x01) {
                    m_raw.resize((size_t)kBatchSectors * (size_t)layout.sectorSize);
                    return true;
                }
            }
        }
        error = "Could not detect a supported PSX disc layout";
        return false;
    }

    bool CanPreserveRawXa() const { return m_layout.rawKind != RawKind::None; }

    // Reads `count` (<= kBatchSectors) whole sectors starting at `lba`.
    bool ReadRaw(u32 lba, u32 count) {
        const s32 bytes = (s32)count * m_layout.sectorSize;
        if (sceIoLseek(m_fd, (SceOff)lba * m_layout.sectorSize, PSP_SEEK_SET) < 0) return false;
        return sceIoRead(m_fd, m_raw.data(), (SceSize)bytes) == bytes;
    }

    const u8* Data(u32 i) const {
        return m_raw.data() + (size_t)i * m_layout.sectorSize + m_layout.dataOffset;
    }

    // The 2336-byte Mode 2 payload (subheader + data + EDC), as the movie
    // player streams it.
    const u8* Xa(u32 i) const {
        const size_t skip = (m_layout.rawKind == RawKind::Raw2352) ? 16 : 0;
        return m_raw.data() + (size_t)i * m_layout.sectorSize + skip;
    }

    bool ReadExtent(u32 lba, u32 size, std::vector<u8>& out) {
        out.clear();
        out.reserve(size);
        u32 left = size;
        while (left > 0) {
            const u32 blocks = std::min<u32>((left + kLogicalBlock - 1) / kLogicalBlock, kBatchSectors);
            if (!ReadRaw(lba, blocks)) return false;
            for (u32 i = 0; i < blocks && left > 0; i++) {
                const u32 take = std::min(left, kLogicalBlock);
                out.insert(out.end(), Data(i), Data(i) + take);
                left -= take;
            }
            lba += blocks;
        }
        return true;
    }

private:
    SceUID m_fd = -1;
    SectorLayout m_layout{};
    std::vector<u8> m_raw;
};

// Mirrors the desktop parse_dir_records port, including skipping zero-length
// records to the next 2048-byte boundary.
bool WalkTree(DiscReader& reader, const DirEntry& dir, std::vector<DirEntry>& out, int depth) {
    if (depth > 16) return false;
    std::vector<u8> data;
    if (!reader.ReadExtent(dir.lba, dir.size, data)) return false;

    size_t i = 0;
    while (i < data.size()) {
        const u8 recordLen = data[i];
        if (recordLen == 0) {
            i = ((i / kLogicalBlock) + 1) * kLogicalBlock;
            continue;
        }
        if (i + recordLen > data.size() || recordLen < 34) break;

        const u8* rec = data.data() + i;
        const u8 nameLen = rec[32];
        i += recordLen;
        if (33u + nameLen > recordLen) continue;
        if (nameLen == 1 && (rec[33] == 0x00 || rec[33] == 0x01)) continue;

        std::string name((const char*)rec + 33, nameLen);
        const size_t semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();

        DirEntry entry;
        entry.path = dir.path.empty() ? ToLower(name) : dir.path + "/" + ToLower(name);
        entry.isDir = (rec[25] & 0x02) != 0;
        entry.lba = p3dReadU32LE(rec + 2);
        entry.size = p3dReadU32LE(rec + 10);
        out.push_back(entry);
        if (entry.isDir && !WalkTree(reader, entry, out, depth + 1)) return false;
    }
    return true;
}

// SYSTEM.CNF holds e.g. "BOOT=cdrom:\SLUS_006.84;1"; reduce it to "SLUS-00684".
bool ReadBootSerial(DiscReader& reader, const std::vector<DirEntry>& entries, std::string& serial) {
    for (const DirEntry& e : entries) {
        if (e.isDir || e.path != "system.cnf") continue;
        std::vector<u8> data;
        if (!reader.ReadExtent(e.lba, e.size, data)) return false;
        const std::string text(data.begin(), data.end());
        size_t pos = text.find("BOOT");
        if (pos == std::string::npos) pos = text.find("boot");
        if (pos == std::string::npos) return false;
        const size_t eq = text.find('=', pos + 4);
        if (eq == std::string::npos) return false;
        size_t end = text.find_first_of("\r\n", eq + 1);
        if (end == std::string::npos) end = text.size();
        std::string value = text.substr(eq + 1, end - eq - 1);
        const size_t sep = value.find_last_of("\\/:");
        if (sep != std::string::npos) value = value.substr(sep + 1);
        serial.clear();
        for (char c : value) {
            if (c == ';') break;
            if (c == '.' || std::isspace((unsigned char)c)) continue;
            serial += (c == '_') ? '-' : (char)std::toupper((unsigned char)c);
        }
        return !serial.empty();
    }
    return false;
}

s64 ExistingFileSize(const std::string& path) {
    SceIoStat stat{};
    if (sceIoGetstat(p3d::io::ResolvePath(path).c_str(), &stat) < 0) return 0;
    return (stat.st_mode & FIO_S_IFDIR) ? 0 : (s64)stat.st_size;
}

} // namespace

bool PsxDiscExtractor::AreAssetsPresent() {
    return p3d::io::DirExists("xc") && p3d::io::DirExists("fe")
        && p3d::io::DirExists("tim") && p3d::io::FileExists("license.tim");
}

void PsxDiscExtractor::Init() {
    m_error.clear();
    m_candidates.clear();
    m_state = State::Idle;
    // Scan right away so the page already says whether the image is there.
    if (!AreAssetsPresent()) {
        ScanAsync();
    }
}

void PsxDiscExtractor::Shutdown() {}

float PsxDiscExtractor::GetProgress() const {
    const s64 total = m_bytesTotal;
    if (total <= 0) return 0.0f;
    return (float)((double)m_bytesDone / (double)total);
}

// A directory listing is quick, so the PSP scans on the calling thread.
void PsxDiscExtractor::ScanAsync() {
    m_error.clear();
    m_candidates.clear();
    const std::string dir = DiscImageDir();
    p3d::io::CreateDirectories(dir);
    for (const p3d::io::DirEntryInfo& entry : p3d::io::ListDirectory(dir, /*recursive=*/false)) {
        // macOS leaves "._name" metadata files beside everything it copies
        // to a FAT card; they are not disc images.
        if (entry.isDirectory || entry.name.compare(0, 2, "._") == 0) continue;
        const std::string name = ToLower(entry.name);
        if (EndsWith(name, ".bin") || EndsWith(name, ".iso")) {
            m_candidates.push_back(entry.fullPath);
        }
    }
    if (m_candidates.empty()) {
        m_state = State::ScanNoneFound;
    }
    else {
        m_state = (m_candidates.size() > 1) ? State::ScanMultipleFound : State::ScanFoundOne;
    }
}

int PsxDiscExtractor::ThreadEntry(unsigned int, void* argp) {
    PsxDiscExtractor* self = *static_cast<PsxDiscExtractor**>(argp);
    self->ExtractThreadFunc(self->m_candidates[0]);
    self->m_thread = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}

void PsxDiscExtractor::StartExtractAsync() {
    if (m_candidates.empty() || m_thread >= 0) return;
    m_bytesDone = 0;
    m_bytesTotal = 0;
    m_error.clear();
    m_state = State::Extracting;
    m_thread = sceKernelCreateThread("ReChainExtract", ThreadEntry, kThreadPriority, kThreadStack,
                                     PSP_THREAD_ATTR_USER, nullptr);
    if (m_thread < 0) {
        m_thread = -1;
        m_error = "Could not start the extraction thread";
        m_state = State::Error;
        return;
    }
    PsxDiscExtractor* self = this;
    sceKernelStartThread(m_thread, sizeof(self), &self);
}

void PsxDiscExtractor::ScanThreadFunc() {}

void PsxDiscExtractor::ExtractThreadFunc(std::string imagePath) {
    const auto fail = [this](const std::string& message) {
        LOG("[PsxDiscExtractor] %s", message.c_str());
        m_error = message;
        m_state = State::Error;
    };

    DiscReader reader;
    std::string error;
    if (!reader.Open(imagePath, error)) {
        fail(error);
        return;
    }

    std::vector<u8> pvd;
    if (!reader.ReadExtent(16, kLogicalBlock, pvd) || pvd[156] < 34) {
        fail("Primary Volume Descriptor not found");
        return;
    }
    DirEntry root;
    root.isDir = true;
    root.lba = p3dReadU32LE(pvd.data() + 158);
    root.size = p3dReadU32LE(pvd.data() + 166);

    std::vector<DirEntry> entries;
    if (!WalkTree(reader, root, entries, 0)) {
        fail("Could not read the disc directory tree");
        return;
    }

    std::string serial;
    if (!ReadBootSerial(reader, entries, serial)) {
        fail("Could not verify the disc version (SYSTEM.CNF)");
        return;
    }
    if (serial != kExpectedSerial) {
        fail("Wrong disc (" + serial + "). Only the NTSC-U release " + kExpectedSerial + " is supported");
        return;
    }
    LOG("[PsxDiscExtractor] Verified disc serial: %s", serial.c_str());

    std::vector<DirEntry> files;
    for (const DirEntry& e : entries) {
        if (!e.isDir) files.push_back(e);
    }
    std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) { return a.lba < b.lba; });

    // Movies keep their raw Mode 2 sectors up to the next file, exactly like
    // the desktop extractor; the last file falls back to its own block count.
    struct Job {
        const DirEntry* entry;
        bool rawXa;
        u32 sectors;
        u32 bytes;
    };
    std::vector<Job> jobs;
    const Job* sentinel = nullptr;
    s64 total = 0;
    s64 needed = kSpaceMargin;
    for (size_t i = 0; i < files.size(); i++) {
        const DirEntry& e = files[i];
        if (!ShouldExtract(e.path)) continue;
        Job job{ &e, EndsWith(e.path, ".str") && reader.CanPreserveRawXa(), 0, 0 };
        if (job.rawXa) {
            const u32 blocks = std::max<u32>((e.size + kLogicalBlock - 1) / kLogicalBlock, 1);
            const u32 next = (i + 1 < files.size()) ? files[i + 1].lba : e.lba + blocks;
            job.sectors = next - e.lba;
            job.bytes = job.sectors * kXaSectorBytes;
        }
        else {
            job.sectors = (e.size + kLogicalBlock - 1) / kLogicalBlock;
            job.bytes = e.size;
        }
        total += job.bytes;
        needed += job.bytes - ExistingFileSize(e.path);
        jobs.push_back(job);
    }
    std::stable_partition(jobs.begin(), jobs.end(), [](const Job& j) { return j.entry->path != kSentinelFile; });
    for (const Job& j : jobs) {
        if (j.entry->path == kSentinelFile) sentinel = &j;
    }
    if (!sentinel) {
        fail("The disc image has no license.tim");
        return;
    }

    const std::string device = std::string(p3d::io::GetPspGameDir()).substr(0, 4);
    const s64 freeBytes = FreeBytes(device.c_str());
    if (freeBytes >= 0 && freeBytes < needed) {
        char message[128];
        std::snprintf(message, sizeof(message), "Not enough free space: %d MB needed, %d MB free",
                      (int)(needed >> 20) + 1, (int)(freeBytes >> 20));
        fail(message);
        return;
    }
    m_bytesTotal = (total > 0) ? total : 1;

    sceIoRemove(p3d::io::ResolvePath(kSentinelFile).c_str());

    std::vector<u8> out((size_t)kBatchSectors * kXaSectorBytes);
    for (const Job& job : jobs) {
        const std::string& path = job.entry->path;
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            p3d::io::CreateDirectories(path.substr(0, slash));
        }
        p3d::io::FileHandle file;
        if (!file.Open(path, p3d::io::FileHandle::Mode::Write, /*resolveCase=*/false)) {
            fail("Failed writing " + path);
            return;
        }
        u32 left = job.bytes;
        for (u32 s = 0; s < job.sectors && left > 0; s += kBatchSectors) {
            const u32 count = std::min(kBatchSectors, job.sectors - s);
            if (!reader.ReadRaw(job.entry->lba + s, count)) {
                fail("Failed reading the disc image at " + path);
                return;
            }
            size_t bytes = 0;
            for (u32 i = 0; i < count && left > 0; i++) {
                const u32 take = std::min(left, job.rawXa ? kXaSectorBytes : kLogicalBlock);
                std::memcpy(out.data() + bytes, job.rawXa ? reader.Xa(i) : reader.Data(i), take);
                bytes += take;
                left -= take;
            }
            if (file.Write(out.data(), bytes) != bytes) {
                fail("Failed writing " + path + " (Memory Stick full?)");
                return;
            }
            m_bytesDone = m_bytesDone + (s64)bytes;
        }
        file.Close();
    }

    LOG("[PsxDiscExtractor] Extracted %d files, %d bytes", (int)jobs.size(), (int)total);
    m_state = State::Done;
}
