#include "gen/common.h"
#include "extra/psxdiscextractor.h"
#include "p3d/byteread.h"
#include "p3d/fileio.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <filesystem>

PsxDiscExtractor* g_psxDiscExtractor = nullptr;

bool PsxDiscExtractor::AreAssetsPresent() {
    return p3d::io::DirExists("xc") && p3d::io::DirExists("fe") && p3d::io::DirExists("tim")
        && p3d::io::FileExists("license.tim");
}

enum class RawKind { None, Raw2352, Raw2336 };

struct SectorLayout {
    const char* name;
    s32 sectorSize;
    s32 dataOffset;
    s32 dataSize;
    RawKind rawKind;
};

static const SectorLayout kLayoutCandidates[] = {
    { "2048",       2048, 0,  2048, RawKind::None },
    { "2352_mode1", 2352, 16, 2048, RawKind::Raw2352 },
    { "2352_xa",    2352, 24, 2048, RawKind::Raw2352 },
    { "2336_xa",    2336, 8,  2048, RawKind::Raw2336 },
};

static const std::vector<std::string> kWhitelistDirs = {
    "fe", "rchars", "rtarget", "scr", "sound", "tim", "xc"
};

static const std::vector<std::string> kWhitelistRootFiles = {
    "license.tim", "license_data.dat", "loadanim.con",
    "postdemo.tim", "predemo.tim", "runfirst.tim"
};

std::string ToLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Strips the ISO9660 ";1" version suffix and lowercases - must happen before both the
// whitelist comparison and the output path write.
std::string SafeLowerName(std::string name) {
    size_t semi = name.find(';');
    if (semi != std::string::npos) {
        name = name.substr(0, semi);
    }
    while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
    return ToLower(name);
}

bool ShouldExtract(const std::vector<std::string>& pathParts) {
    if (pathParts.empty()) return false;
    if (pathParts.size() == 1) {
        return std::find(kWhitelistRootFiles.begin(), kWhitelistRootFiles.end(), pathParts[0]) != kWhitelistRootFiles.end();
    }
    return std::find(kWhitelistDirs.begin(), kWhitelistDirs.end(), pathParts[0]) != kWhitelistDirs.end();
}

bool ShouldExtractAsRawXa(const std::vector<std::string>& pathParts) {
    if (pathParts.empty()) return false;
    const std::string& name = pathParts.back();
    return name.size() >= 4 && ToLower(name.substr(name.size() - 4)) == ".str";
}

struct DirEntry {
    std::vector<std::string> pathParts;
    bool isDir = false;
    u32 extentLba = 0;
    u32 size = 0;
};

class IsoReader {
public:
    explicit IsoReader(const std::string& path) {
        if (!m_file.Open(path, p3d::io::FileHandle::Mode::Read, /*resolveCase=*/false)) {
            throw std::runtime_error("Could not open disc image: " + path);
        }
        DetectLayout();
    }

    ~IsoReader() = default;

    IsoReader(const IsoReader&) = delete;
    IsoReader& operator=(const IsoReader&) = delete;

    bool CanPreserveRawXa() const { return m_layout.rawKind != RawKind::None; }

    std::vector<u8> ReadLogicalBlock(u32 lba) const {
        return ReadLogicalBlock(lba, m_layout);
    }

    std::vector<u8> ReadExtentLogical(u32 extentLba, u32 size) const {
        const u32 blocks = (size + 2047u) / 2048u;
        std::vector<u8> out;
        out.reserve((size_t)blocks * 2048);
        for (u32 i = 0; i < blocks; i++) {
            std::vector<u8> block = ReadLogicalBlock(extentLba + i);
            out.insert(out.end(), block.begin(), block.end());
        }
        out.resize(size);
        return out;
    }

    std::vector<u8> ReadRawSector(u32 lba) const {
        const long offset = (long)lba * m_layout.sectorSize;
        if (!m_file.Seek(offset, SEEK_SET)) {
            throw std::runtime_error("Failed seeking raw sector");
        }
        std::vector<u8> data((size_t)m_layout.sectorSize);
        if (m_file.Read(data.data(), data.size()) != data.size()) {
            throw std::runtime_error("Failed reading raw sector");
        }
        return data;
    }

    std::vector<u8> RawSectorToXa2336(const std::vector<u8>& rawSector) const {
        if (m_layout.rawKind == RawKind::Raw2352) {
            return std::vector<u8>(rawSector.begin() + 16, rawSector.begin() + 16 + 2336);
        }
        if (m_layout.rawKind == RawKind::Raw2336) {
            return rawSector;
        }
        throw std::runtime_error("Cannot preserve raw XA sectors from a 2048-only image");
    }

    std::vector<u8> ReadExtentRawXa2336BySectorCount(u32 extentLba, u32 sectorCount) const {
        if (!CanPreserveRawXa()) {
            throw std::runtime_error("This layout has no raw XA sectors to preserve");
        }
        std::vector<u8> out;
        out.reserve((size_t)sectorCount * 2336);
        for (u32 i = 0; i < sectorCount; i++) {
            std::vector<u8> raw = ReadRawSector(extentLba + i);
            std::vector<u8> xa = RawSectorToXa2336(raw);
            out.insert(out.end(), xa.begin(), xa.end());
        }
        return out;
    }

private:
    std::vector<u8> ReadLogicalBlock(u32 lba, const SectorLayout& layout) const {
        const long offset = (long)lba * layout.sectorSize + layout.dataOffset;
        if (!m_file.Seek(offset, SEEK_SET)) {
            throw std::runtime_error("Failed seeking logical block");
        }
        std::vector<u8> data((size_t)layout.dataSize);
        if (m_file.Read(data.data(), data.size()) != data.size()) {
            throw std::runtime_error("Failed reading logical block");
        }
        return data;
    }

    void DetectLayout() {
        for (const SectorLayout& layout : kLayoutCandidates) {
            std::vector<u8> pvd;
            try {
                pvd = ReadLogicalBlock(16, layout);
            } catch (...) {
                continue;
            }

            if (pvd.size() >= 7 && pvd[0] == 0x01 &&
                memcmp(pvd.data() + 1, "CD001", 5) == 0 && pvd[6] == 0x01) {
                m_layout = layout;
                return;
            }
        }
        throw std::runtime_error("Could not detect a supported PSX disc layout");
    }

    mutable p3d::io::FileHandle m_file;
    SectorLayout m_layout{};
};

// Skips zero-length directory records to the next 2048-byte boundary; ported verbatim
// from the python's parse_dir_records (including its infinite-loop guard).
std::vector<DirEntry> ParseDirRecords(const std::vector<u8>& data, const std::vector<std::string>& parentParts) {
    std::vector<DirEntry> entries;
    size_t i = 0;
    const size_t total = data.size();

    while (i < total) {
        const u8 recordLen = data[i];

        if (recordLen == 0) {
            const size_t nextBlock = ((i / 2048) + 1) * 2048;
            if (nextBlock <= i) break;
            i = nextBlock;
            continue;
        }

        if (i + recordLen > total) break;
        if (recordLen < 34) break;

        const u8* rec = data.data() + i;
        const u32 extentLba = p3dReadU32LE(rec + 2);
        const u32 fileSize = p3dReadU32LE(rec + 10);
        const u8 fileFlags = rec[25];
        const u8 fileIdLen = rec[32];

        if (33u + fileIdLen > recordLen) {
            i += recordLen;
            continue;
        }

        const u8* fileId = rec + 33;
        // Dot entries ("." / "..") are encoded as a single 0x00 or 0x01 byte.
        if (fileIdLen == 1 && (fileId[0] == 0x00 || fileId[0] == 0x01)) {
            i += recordLen;
            continue;
        }

        std::string name((const char*)fileId, fileIdLen);
        name = SafeLowerName(name);

        DirEntry entry;
        entry.pathParts = parentParts;
        entry.pathParts.push_back(name);
        entry.isDir = (fileFlags & 0x02) != 0;
        entry.extentLba = extentLba;
        entry.size = fileSize;
        entries.push_back(std::move(entry));

        i += recordLen;
    }

    return entries;
}

DirEntry ReadRoot(const IsoReader& reader) {
    std::vector<u8> pvd = reader.ReadLogicalBlock(16);
    if (pvd.size() < 190 || pvd[0] != 1 || memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        throw std::runtime_error("Primary Volume Descriptor not found");
    }

    const u8 rootRecordLen = pvd[156];
    if (rootRecordLen < 34) {
        throw std::runtime_error("Invalid root directory record");
    }

    const u8* rootRec = pvd.data() + 156;
    DirEntry root;
    root.isDir = true;
    root.extentLba = p3dReadU32LE(rootRec + 2);
    root.size = p3dReadU32LE(rootRec + 10);
    return root;
}

void WalkTree(const IsoReader& reader, const DirEntry& dir, std::vector<DirEntry>& outAll) {
    std::vector<u8> dirData = reader.ReadExtentLogical(dir.extentLba, dir.size);
    std::vector<DirEntry> children = ParseDirRecords(dirData, dir.pathParts);

    for (const DirEntry& child : children) {
        outAll.push_back(child);
        if (child.isDir) {
            WalkTree(reader, child, outAll);
        }
    }
}

std::string JoinPath(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += "/";
        out += parts[i];
    }
    return out;
}

void WriteFile(const std::string& path, const std::vector<u8>& data) {
    std::filesystem::path outPath(path);
    if (outPath.has_parent_path()) {
        p3d::io::CreateDirectories(outPath.parent_path().string());
    }
    if (!p3d::io::WriteFile(path, data)) {
        throw std::runtime_error("Failed writing file: " + path);
    }
}

static constexpr const char* kExpectedSerial = "SLUS-00684";

// Reduces a boot executable name like "SLUS_006.84" (or "slus_006.84;1") to "SLUS-00684"
// so it can be compared directly against kExpectedSerial.
std::string NormalizeSerial(std::string name) {
    size_t semi = name.find(';');
    if (semi != std::string::npos) {
        name = name.substr(0, semi);
    }
    std::string out;
    for (char c : name) {
        if (c == '.') continue;
        out += (c == '_') ? '-' : (char)std::toupper((unsigned char)c);
    }
    return out;
}

// SYSTEM.CNF is the standard PSX boot descriptor at the disc root, e.g.:
//   BOOT=cdrom:\SLUS_006.84;1
//   TCB=4
bool TryReadBootSerial(const IsoReader& reader, const std::vector<DirEntry>& allEntries, std::string& outSerial) {
    const DirEntry* cnfEntry = nullptr;
    for (const DirEntry& e : allEntries) {
        if (!e.isDir && e.pathParts.size() == 1 && e.pathParts[0] == "system.cnf") {
            cnfEntry = &e;
            break;
        }
    }
    if (!cnfEntry) {
        return false;
    }

    std::vector<u8> data = reader.ReadExtentLogical(cnfEntry->extentLba, cnfEntry->size);
    std::string text((const char*)data.data(), data.size());

    size_t bootPos = text.find("BOOT");
    if (bootPos == std::string::npos) {
        bootPos = text.find("boot");
    }
    if (bootPos == std::string::npos) {
        return false;
    }

    size_t eqPos = text.find('=', bootPos + 4);
    if (eqPos == std::string::npos) {
        return false;
    }

    size_t valueStart = eqPos + 1;
    while (valueStart < text.size() && std::isspace((unsigned char)text[valueStart])) valueStart++;

    size_t lineEnd = text.find_first_of("\r\n", valueStart);
    if (lineEnd == std::string::npos) {
        lineEnd = text.size();
    }
    std::string value = text.substr(valueStart, lineEnd - valueStart);

    size_t lastSep = value.find_last_of("\\/:");
    std::string bootName = (lastSep != std::string::npos) ? value.substr(lastSep + 1) : value;

    while (!bootName.empty() && std::isspace((unsigned char)bootName.back())) bootName.pop_back();
    if (bootName.empty()) {
        return false;
    }

    outSerial = NormalizeSerial(bootName);
    return true;
}

void PsxDiscExtractor::Init() {
    m_state = State::Idle;
    m_error.clear();
    m_candidates.clear();
}

void PsxDiscExtractor::Shutdown() {
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

float PsxDiscExtractor::GetProgress() const {
    const s64 total = m_bytesTotal;
    if (total <= 0) return 0.0f;
    return (float)((double)m_bytesDone / (double)total);
}

void PsxDiscExtractor::ScanAsync() {
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_state = State::Scanning;
    m_error.clear();
    m_workerThread = std::thread(&PsxDiscExtractor::ScanThreadFunc, this);
}

void PsxDiscExtractor::StartExtractAsync() {
    if (m_candidates.empty()) return;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_state = State::Extracting;
    m_bytesDone = 0;
    m_bytesTotal = 0;
    m_error.clear();
    m_workerThread = std::thread(&PsxDiscExtractor::ExtractThreadFunc, this, m_candidates[0]);
}

void PsxDiscExtractor::ScanThreadFunc() {
    p3d::io::CreateDirectories(kDiscImageDir);

    m_candidates.clear();
    if (p3d::io::DirExists(kDiscImageDir)) {
        for (const p3d::io::DirEntryInfo& entry : p3d::io::ListDirectory(kDiscImageDir, /*recursive=*/false)) {
            if (entry.isDirectory) continue;
            const size_t dot = entry.name.find_last_of('.');
            const std::string ext = (dot != std::string::npos) ? ToLower(entry.name.substr(dot)) : std::string();
            if (ext == ".bin" || ext == ".iso") {
                m_candidates.push_back(entry.fullPath);
            }
        }
    }

    if (m_candidates.empty()) {
        m_state = State::ScanNoneFound;
    }
    else if (m_candidates.size() > 1) {
        m_state = State::ScanMultipleFound;
    }
    else {
        m_state = State::ScanFoundOne;
    }
}

void PsxDiscExtractor::ExtractThreadFunc(std::string imagePath) {
    try {
        IsoReader reader(imagePath);

        DirEntry root = ReadRoot(reader);
        std::vector<DirEntry> allEntries;
        WalkTree(reader, root, allEntries);

        std::string serial;
        if (!TryReadBootSerial(reader, allEntries, serial)) {
            throw std::runtime_error("Could not verify the disc version (SYSTEM.CNF not found or unreadable). Refusing to extract.");
        }
        if (serial != kExpectedSerial) {
            throw std::runtime_error("Wrong disc detected (" + serial + "). This tool only supports the NTSC-U US release ("
                                     + std::string(kExpectedSerial) + "); PAL and other region discs are not supported.");
        }
        LOG_ALWAYS("[PsxDiscExtractor] Verified disc serial: %s", serial.c_str());

        std::vector<DirEntry> fileEntries;
        for (const DirEntry& e : allEntries) {
            if (!e.isDir) fileEntries.push_back(e);
        }
        std::sort(fileEntries.begin(), fileEntries.end(), [](const DirEntry& a, const DirEntry& b) {
            return a.extentLba < b.extentLba;
        });

        // Next-file LBA boundary per entry, for the .str raw-sector span trick. The last
        // file has no "next" - fall back to its own logical block count (never zero).
        std::vector<u32> nextExtentLba(fileEntries.size());
        for (size_t i = 0; i < fileEntries.size(); i++) {
            if (i + 1 < fileEntries.size()) {
                nextExtentLba[i] = fileEntries[i + 1].extentLba;
            }
            else {
                const u32 blocks = (fileEntries[i].size + 2047u) / 2048u;
                nextExtentLba[i] = fileEntries[i].extentLba + std::max(blocks, 1u);
            }
        }

        // Pass 1: total bytes to extract, for a real progress fraction.
        s64 totalBytes = 0;
        for (size_t i = 0; i < fileEntries.size(); i++) {
            const DirEntry& e = fileEntries[i];
            if (!ShouldExtract(e.pathParts)) continue;
            if (ShouldExtractAsRawXa(e.pathParts) && reader.CanPreserveRawXa()) {
                const u32 sectorCount = nextExtentLba[i] - e.extentLba;
                totalBytes += (s64)sectorCount * 2336;
            }
            else {
                totalBytes += e.size;
            }
        }
        m_bytesTotal = (totalBytes > 0) ? totalBytes : 1;

        // Pass 2: actual extraction.
        for (size_t i = 0; i < fileEntries.size(); i++) {
            const DirEntry& e = fileEntries[i];
            if (!ShouldExtract(e.pathParts)) continue;

            const std::string outPath = JoinPath(e.pathParts);

            if (ShouldExtractAsRawXa(e.pathParts) && reader.CanPreserveRawXa()) {
                const u32 sectorCount = nextExtentLba[i] - e.extentLba;
                std::vector<u8> data = reader.ReadExtentRawXa2336BySectorCount(e.extentLba, sectorCount);
                WriteFile(outPath, data);
                m_bytesDone += (s64)data.size();
            }
            else {
                std::vector<u8> data = reader.ReadExtentLogical(e.extentLba, e.size);
                WriteFile(outPath, data);
                m_bytesDone += (s64)data.size();
            }
        }

        p3d::io::InvalidateResolveCache();
        LOG_ALWAYS("[PsxDiscExtractor] Extraction done: %lld bytes", (long long)m_bytesDone);
        m_state = State::Done;
    } catch (const std::exception& ex) {
        LOG_ALWAYS("[PsxDiscExtractor] Extraction failed: %s", ex.what());
        m_error = ex.what();
        m_state = State::Error;
    }
}
