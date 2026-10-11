// Host test for src/psp/psp_psxdiscextractor.cpp.
//
// Synthetic ISO images cover the 2048-byte layout, the serial check, the free
// space check and the "license.tim last" rule. When the user's own disc image
// is present (DISC_IMAGE, default bin/discimage/game.bin) the extraction is
// also compared byte for byte with the desktop extractor's tree (REF_DIR,
// default bin/).
#include "extra/psxdiscextractor.h"
#include "p3d/fileio.h"

#include <pspiofilemgr.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string g_gameDir;
// Bytes FileHandle::Write accepts before failing (-1: unlimited).
long long g_writeBudget = -1;
int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

std::string Host(const std::string& path) {
    if (!path.empty() && path[0] == '/') return path;
    return g_gameDir + "/data/" + path;
}

std::vector<char> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void PutU32Both(std::vector<unsigned char>& s, size_t at, unsigned v) {
    for (int i = 0; i < 4; i++) {
        s[at + i] = (unsigned char)(v >> (8 * i));
        s[at + 7 - i] = (unsigned char)(v >> (8 * i));
    }
}

void DirRecord(std::vector<unsigned char>& s, size_t& at, const char* name, unsigned lba, unsigned size, bool dir) {
    const size_t nameLen = std::strlen(name) ? std::strlen(name) : 1;
    const size_t len = 33 + nameLen + ((33 + nameLen) & 1);
    s[at] = (unsigned char)len;
    PutU32Both(s, at + 2, lba);
    PutU32Both(s, at + 10, size);
    s[at + 25] = dir ? 2 : 0;
    s[at + 32] = (unsigned char)nameLen;
    if (std::strlen(name)) std::memcpy(&s[at + 33], name, nameLen);
    at += len;
}

// A 2048-byte-sector ISO with SYSTEM.CNF, LICENSE.TIM and XC/A.BIN.
void WriteIso(const fs::path& path, const char* bootLine) {
    const unsigned kSectors = 24;
    std::vector<unsigned char> s(kSectors * 2048, 0);
    unsigned char* pvd = &s[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    size_t at = 16 * 2048 + 156;
    DirRecord(s, at, "", 18, 2048, true);

    at = 18 * 2048;
    DirRecord(s, at, "", 18, 2048, true);
    DirRecord(s, at, "\1", 18, 2048, true);
    const std::string cnf = std::string("BOOT = cdrom:\\") + bootLine + ";1\r\nTCB = 4\r\n";
    DirRecord(s, at, "SYSTEM.CNF;1", 20, (unsigned)cnf.size(), false);
    DirRecord(s, at, "LICENSE.TIM;1", 21, 3000, false);
    DirRecord(s, at, "XC", 19, 2048, true);

    at = 19 * 2048;
    DirRecord(s, at, "", 19, 2048, true);
    DirRecord(s, at, "\1", 18, 2048, true);
    DirRecord(s, at, "A.BIN;1", 23, 100, false);

    std::memcpy(&s[20 * 2048], cnf.data(), cnf.size());
    for (unsigned i = 0; i < 3000; i++) s[21 * 2048 + i] = (unsigned char)(i * 7);
    for (unsigned i = 0; i < 100; i++) s[23 * 2048 + i] = (unsigned char)(255 - i);

    std::ofstream(path, std::ios::binary).write((const char*)s.data(), (std::streamsize)s.size());
}

fs::path NewGameDir(const fs::path& base, const char* name) {
    const fs::path dir = base / name;
    fs::create_directories(dir / "data");
    fs::create_directories(dir / "discimage");
    g_gameDir = dir.string();
    return dir;
}

PsxDiscExtractor::State Extract(PsxDiscExtractor& extractor) {
    extractor.Init();
    if (extractor.GetState() != PsxDiscExtractor::State::ScanFoundOne) return extractor.GetState();
    extractor.StartExtractAsync();
    return extractor.GetState();
}

} // namespace

namespace p3d::io {

const char* GetPspGameDir() { return g_gameDir.c_str(); }
std::string ResolvePath(const char* path) { return Host(path ? path : ""); }
std::string ResolvePath(const std::string& path) { return Host(path); }
bool FileExists(const std::string& path) { return fs::is_regular_file(Host(path)); }
bool DirExists(const std::string& path) { return fs::is_directory(Host(path)); }
bool CreateDirectories(const std::string& path) {
    std::error_code ec;
    fs::create_directories(Host(path), ec);
    return fs::is_directory(Host(path));
}

std::vector<DirEntryInfo> ListDirectory(const std::string& path, bool) {
    std::vector<DirEntryInfo> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Host(path), ec)) {
        out.push_back({ entry.path().filename().string(), entry.path().string(), entry.is_directory() });
    }
    return out;
}

FileHandle::~FileHandle() { Close(); }

bool FileHandle::Open(const std::string& path, Mode, bool) {
    Close();
    m_file = std::fopen(Host(path).c_str(), "wb");
    return m_file != nullptr;
}

void FileHandle::Close() {
    if (m_file) std::fclose(static_cast<FILE*>(m_file));
    m_file = nullptr;
}

size_t FileHandle::Write(const void* src, size_t bytes) {
    if (g_writeBudget >= 0) {
        if ((long long)bytes > g_writeBudget) bytes = (size_t)g_writeBudget;
        g_writeBudget -= (long long)bytes;
    }
    return std::fwrite(src, 1, bytes, static_cast<FILE*>(m_file));
}

} // namespace p3d::io

int main() {
    const fs::path base = fs::temp_directory_path() / ("rechan-extractor-" + std::to_string(getpid()));
    fs::remove_all(base);

    {
        const fs::path dir = NewGameDir(base, "iso");
        WriteIso(dir / "discimage" / "game.iso", "SLUS_006.84");
        std::ofstream(dir / "discimage" / "._game.iso") << "AppleDouble";
        PsxDiscExtractor extractor;
        Check(Extract(extractor) == PsxDiscExtractor::State::Done, "2048-byte ISO extracts (._ file ignored)");
        const std::vector<char> lic = ReadAll(dir / "data" / "license.tim");
        bool licOk = lic.size() == 3000;
        for (unsigned i = 0; licOk && i < 3000; i++) licOk = (unsigned char)lic[i] == (unsigned char)(i * 7);
        Check(licOk, "license.tim spans two sectors byte-exact");
        Check(ReadAll(dir / "data" / "xc" / "a.bin").size() == 100, "xc/a.bin extracted");
        Check(!fs::exists(dir / "data" / "system.cnf"), "files outside the whitelist are skipped");
        Check(PsxDiscExtractor::AreAssetsPresent() == false, "no fe/tim: assets not complete");
        Check(extractor.GetProgress() == 1.0f, "progress reaches 100%");
    }
    {
        const fs::path dir = NewGameDir(base, "wrong");
        WriteIso(dir / "discimage" / "game.iso", "SLES_123.45");
        PsxDiscExtractor extractor;
        Check(Extract(extractor) == PsxDiscExtractor::State::Error
              && std::strstr(extractor.GetError(), "SLES-12345"), "PAL serial refused");
        Check(!fs::exists(dir / "data" / "xc"), "nothing written for the wrong disc");
    }
    {
        const fs::path dir = NewGameDir(base, "space");
        WriteIso(dir / "discimage" / "game.iso", "SLUS_006.84");
        g_testFreeBytes = 1024 * 1024;
        PsxDiscExtractor extractor;
        Check(Extract(extractor) == PsxDiscExtractor::State::Error
              && std::strstr(extractor.GetError(), "Not enough free space"), "full card refused before writing");
        g_testFreeBytes = int64_t(1) << 40;
    }
    {
        const fs::path dir = NewGameDir(base, "cut");
        WriteIso(dir / "discimage" / "game.iso", "SLUS_006.84");
        std::ofstream(dir / "data" / "license.tim") << "old";
        g_writeBudget = 50;
        PsxDiscExtractor extractor;
        Check(Extract(extractor) == PsxDiscExtractor::State::Error, "short write reports an error");
        Check(!fs::exists(dir / "data" / "license.tim"), "interrupted extraction leaves no license.tim");
        g_writeBudget = -1;
    }
    {
        const fs::path dir = NewGameDir(base, "none");
        PsxDiscExtractor extractor;
        extractor.Init();
        Check(extractor.GetState() == PsxDiscExtractor::State::ScanNoneFound, "empty discimage: none found");
        WriteIso(dir / "discimage" / "a.iso", "SLUS_006.84");
        WriteIso(dir / "discimage" / "B.BIN", "SLUS_006.84");
        extractor.ScanAsync();
        Check(extractor.GetState() == PsxDiscExtractor::State::ScanMultipleFound, "two images: multiple found");
    }

    const char* discEnv = std::getenv("DISC_IMAGE");
    const char* refEnv = std::getenv("REF_DIR");
    const fs::path disc = discEnv ? discEnv : "bin/discimage/game.bin";
    const fs::path ref = refEnv ? refEnv : "bin";
    if (fs::exists(disc) && fs::is_directory(ref)) {
        const fs::path dir = NewGameDir(base, "real");
        fs::create_symlink(fs::absolute(disc), dir / "discimage" / "game.bin");
        PsxDiscExtractor extractor;
        Check(Extract(extractor) == PsxDiscExtractor::State::Done, "real disc image extracts");
        Check(PsxDiscExtractor::AreAssetsPresent(), "real disc: assets present");
        size_t files = 0, same = 0;
        unsigned long long bytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(dir / "data")) {
            if (!entry.is_regular_file()) continue;
            files++;
            const fs::path rel = fs::relative(entry.path(), dir / "data");
            const std::vector<char> got = ReadAll(entry.path());
            bytes += got.size();
            if (got == ReadAll(ref / rel)) same++;
            else std::printf("  differs: %s\n", rel.string().c_str());
        }
        std::printf("  %zu files, %llu bytes\n", files, bytes);
        Check(files > 100 && same == files, "real disc: every file matches the desktop extraction");
    }
    else {
        std::printf("SKIP real disc comparison (no %s)\n", disc.string().c_str());
    }

    fs::remove_all(base);
    std::printf("%s\n", g_failures ? "disc extractor: FAILED" : "disc extractor: all checks passed");
    return g_failures ? 1 : 0;
}
