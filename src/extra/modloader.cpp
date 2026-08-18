#include "extra/modloader.h"
#include "gen/config.h"

#ifdef MOD_LOADER

#include "pc/log.h"
#include "pc/apppaths.h"
#include "p3d/hash.h"
#include "p3d/fileio.h"
#include "vendor/stb/stb_image.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <sstream>

static ModLoader s_instance;

ModLoader& ModLoader::Instance() {
    return s_instance;
}

static constexpr const char* kModsDir = apppaths::kModsDir;
static constexpr const char* kIniPath = apppaths::kModsIniPath;

static u32 HashAssetBytes(const u8* data, size_t size) {
    u32 hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static u32 ReadBE32(const u8* data) {
    return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16)
        | (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
}

static bool IsUnmodifiedRechanPng(const std::string& path, const u8* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return false;
    auto pngData = p3d::io::ReadFile(p3d::io::ResolvePath(path));
    if (!pngData) return false;
    const std::vector<u8>& png = *pngData;
    if (png.size() < 12) return false;
    size_t pos = 8;
    while (pos + 12 <= png.size()) {
        const u32 length = ReadBE32(png.data() + pos);
        if (pos + 12u + length > png.size()) break;
        const u8* type = png.data() + pos + 4;
        const u8* payload = png.data() + pos + 8;
        if (std::memcmp(type, "tEXt", 4) == 0) {
            static constexpr char key[] = "rechanBaseline";
            if (length >= sizeof(key) + 8 && std::memcmp(payload, key, sizeof(key)) == 0) {
                char text[9] = {};
                std::memcpy(text, payload + sizeof(key), 8);
                char* end = nullptr;
                const unsigned long expected = std::strtoul(text, &end, 16);
                if (end && *end == '\0') {
                    return static_cast<u32>(expected) == HashAssetBytes(
                        rgba, static_cast<size_t>(width) * height * 4);
                }
            }
        }
        pos += 12u + length;
    }
    return false;
}

static bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

static bool LessIgnoreCase(const std::string& lhs, const std::string& rhs) {
    const size_t count = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < count; ++i) {
        const int a = std::tolower(static_cast<unsigned char>(lhs[i]));
        const int b = std::tolower(static_cast<unsigned char>(rhs[i]));
        if (a != b) return a < b;
    }
    return lhs.size() < rhs.size();
}

static std::string BuildAssetKey(const std::filesystem::path& relativePath,
                                 const std::string& extension) {
    std::vector<std::string> parts;
    for (const auto& part : relativePath) {
        std::string value = part.string();
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        parts.push_back(std::move(value));
    }
    if (parts.empty()) return {};
    const std::string stem = std::filesystem::path(parts.back()).stem().string();
    if (stem.empty() || (extension == ".json" && stem == "mod")) return {};
    if (parts.size() >= 3 && (parts[0] == "levels" || parts[0] == "characters" || parts[0] == "sounds")) {
        return parts[1] + "/" + stem;
    }
    return stem;
}

#ifdef DEBUG
static void RunAssetDiscoverySelfTest() {
    struct Case { const char* path; const char* ext; const char* expected; };
    static const Case cases[] = {
        { "textures/WALL.PNG", ".png", "wall" },
        { "levels/LEV01/geometry/Barrel.glb", ".glb", "lev01/barrel" },
        { "characters/Jackie/textures/Face.png", ".png", "jackie/face" },
        { "sounds/RS0000/sample00.wav", ".wav", "rs0000/sample00" },
        { "sounds/dialog/c01_d010_v00.wav", ".wav", "dialog/c01_d010_v00" },
        { "levels/lev02/parameters_petal00.json", ".json", "lev02/parameters_petal00" },
        { "mod.json", ".json", "" },
    };
    for (const Case& test : cases) {
        const std::string actual = BuildAssetKey(test.path, test.ext);
        if (actual != test.expected) {
            LOG("[ModLoader] Discovery self-test failed: '%s' -> '%s' (expected '%s')",
                test.path, actual.c_str(), test.expected);
        }
    }
}
#endif

static std::string TrimWhitespace(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

struct ModLoaderConfig {
    bool enabled = true;
    bool verbose = false;
    std::vector<std::string> loadOrder;
    std::vector<std::string> blacklist;
};

static bool ParseBool(const std::string& text, bool defaultValue) {
    if (EqualsIgnoreCase(text, "true") || text == "1" || EqualsIgnoreCase(text, "yes") || EqualsIgnoreCase(text, "on")) {
        return true;
    }
    if (EqualsIgnoreCase(text, "false") || text == "0" || EqualsIgnoreCase(text, "no") || EqualsIgnoreCase(text, "off")) {
        return false;
    }
    return defaultValue;
}

static void WriteDefaultIni(const std::string& path) {
    static constexpr const char* kDefaultIni =
        "; Mod Loader settings\n"
        ";\n"
        "; Every folder placed under mods/ is loaded automatically - no extra\n"
        "; metadata or manifest is required. By default mods load in alphabetical\n"
        "; order by folder name, and a mod's assets override any earlier mod's\n"
        "; assets with the same name (last loaded wins).\n"
        "\n"
        "[general]\n"
        "; Set to false to disable the entire mod system without removing files.\n"
        "enabled = true\n"
        "; Set to true to log every asset override (which mod replaced which).\n"
        "verbose = false\n"
        "\n"
        "[load_order]\n"
        "; List mod folder names below, top to bottom, to control load priority.\n"
        "; Listed mods load in that exact order; any mod not listed loads after\n"
        "; them, alphabetically. A mod lower in this list overrides one above it.\n"
        "\n"
        "[blacklist]\n"
        "; List mod folder names below to disable them without deleting them.\n";
    p3d::io::WriteTextFile(path, kDefaultIni);
    p3d::io::InvalidateResolveCache(kModsDir);
}

static ModLoaderConfig ReadConfig(const std::string& path) {
    ModLoaderConfig config;
    auto text = p3d::io::ReadTextFile(p3d::io::ResolvePath(path));
    if (!text) return config;
    std::istringstream file(*text);

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = TrimWhitespace(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }

        if (trimmed.front() == '[') {
            size_t close = trimmed.find(']');
            if (close != std::string::npos) {
                currentSection = TrimWhitespace(trimmed.substr(1, close - 1));
            }
            continue;
        }

        if (EqualsIgnoreCase(currentSection, "general")) {
            size_t eq = trimmed.find('=');
            if (eq == std::string::npos) continue;
            std::string key = TrimWhitespace(trimmed.substr(0, eq));
            std::string value = TrimWhitespace(trimmed.substr(eq + 1));
            if (EqualsIgnoreCase(key, "enabled")) {
                config.enabled = ParseBool(value, true);
            }
            else if (EqualsIgnoreCase(key, "verbose")) {
                config.verbose = ParseBool(value, false);
            }
            continue;
        }

        if (EqualsIgnoreCase(currentSection, "load_order")) {
            config.loadOrder.push_back(trimmed);
            continue;
        }

        if (EqualsIgnoreCase(currentSection, "blacklist")) {
            config.blacklist.push_back(trimmed);
        }
    }
    return config;
}

static bool IsBlacklisted(const ModLoaderConfig& config, const std::string& modName) {
    for (const std::string& entry : config.blacklist) {
        if (EqualsIgnoreCase(entry, modName)) return true;
    }
    return false;
}

// Explicit [load_order] entries come first, in the order listed (skipping
// names that don't match an actual mod folder); any remaining mod folders
// follow afterward, sorted alphabetically. Scan order = priority: a mod
// scanned later overrides same-named assets from a mod scanned earlier.
static std::vector<std::string> BuildLoadOrder(
    const ModLoaderConfig& config,
    const std::vector<std::string>& allModNames) {
    std::vector<bool> used(allModNames.size(), false);
    std::vector<std::string> ordered;

    for (const std::string& wanted : config.loadOrder) {
        for (size_t i = 0; i < allModNames.size(); i++) {
            if (!used[i] && EqualsIgnoreCase(allModNames[i], wanted)) {
                ordered.push_back(allModNames[i]);
                used[i] = true;
                break;
            }
        }
    }

    std::vector<std::string> remaining;
    for (size_t i = 0; i < allModNames.size(); i++) {
        if (!used[i]) remaining.push_back(allModNames[i]);
    }
    std::sort(remaining.begin(), remaining.end(), [](const std::string& a, const std::string& b) {
        return LessIgnoreCase(a, b);
    });

    ordered.insert(ordered.end(), remaining.begin(), remaining.end());
    return ordered;
}

void ModLoader::Init() {
    if (m_initialized)
        return;

#ifdef DEBUG
    RunAssetDiscoverySelfTest();
#endif

    m_textureIndex.clear();
    m_modelIndex.clear();
    m_soundIndex.clear();
    m_dataIndex.clear();
    m_subtitleIndex.clear();
    m_mods.clear();

    if (!std::filesystem::is_directory(kModsDir)) {
        return;
    }

    if (!std::filesystem::exists(kIniPath)) {
        WriteDefaultIni(kIniPath);
        LOG("[ModLoader] Created default %s", kIniPath);
    }
    ModLoaderConfig config = ReadConfig(kIniPath);
    m_enabled = config.enabled;
    m_verbose = config.verbose;

    if (!m_enabled) {
        LOG("[ModLoader] Disabled via %s ([general] enabled = false)", kIniPath);
        m_initialized = true;
        return;
    }

    std::vector<std::string> modNames;
    for (const auto& entry : std::filesystem::directory_iterator(kModsDir)) {
        if (entry.is_directory()) {
            modNames.push_back(entry.path().filename().string());
        }
    }

    std::vector<std::string> orderedNames = BuildLoadOrder(config, modNames);

    int priority = 0;
    for (const std::string& modName : orderedNames) {
        if (IsBlacklisted(config, modName)) {
            ModInfo info;
            info.name = modName;
            info.folder = modName;
            info.priority = priority++;
            info.enabled = false;
            m_mods.push_back(std::move(info));
            LOG("[ModLoader] Skipping blacklisted mod: %s", modName.c_str());
            continue;
        }
        ScanModFolder(std::string(kModsDir) + "/" + modName, modName, priority++);
    }

    LOG("[ModLoader] Initialized: %zu mod(s), %zu texture(s), %zu model(s), %zu sound(s), %zu data file(s)",
        m_mods.size(),
        m_textureIndex.size(),
        m_modelIndex.size(),
        m_soundIndex.size(),
        m_dataIndex.size());

    m_initialized = true;
}

void ModLoader::Reload() {
    m_initialized = false;
    Init();
    LOG("[ModLoader] Hot-reloaded");
}

void ModLoader::Shutdown() {
    m_textureIndex.clear();
    m_modelIndex.clear();
    m_soundIndex.clear();
    m_dataIndex.clear();
    m_subtitleIndex.clear();
    m_mods.clear();
    m_enabled = true;
    m_verbose = false;
    m_initialized = false;
}

// Directory scanning

void ModLoader::ScanModFolder(const std::string& folderPath, const std::string& modName, int priority) {
    ModInfo info;
    info.folder = modName;
    info.priority = priority;
    info.enabled = true;

    // A mod is intentionally identified by its folder name. Asset discovery is
    // entirely manifest-free; JSON files are reserved for actual game data.
    info.name = modName;

    // Scan the entire mod folder recursively. No subfolder structure is required.
    // Asset identity = filename stem only; any nesting depth is supported.
    info.textureCount = ScanCategory(folderPath, modName, ".png", m_textureIndex);
    info.modelCount = ScanCategory(folderPath, modName, ".glb", m_modelIndex);
    info.soundCount = ScanCategory(folderPath, modName, ".wav", m_soundIndex);
    info.dataCount = ScanCategory(folderPath, modName, ".json", m_dataIndex);
    ScanCategory(folderPath, modName, ".srt", m_subtitleIndex);

    m_mods.push_back(std::move(info));
}

int ModLoader::ScanCategory(
    const std::string& categoryPath,
    const std::string& modName,
    const std::string& extension,
    std::unordered_map<u32, std::string>& index) {
    if (!std::filesystem::is_directory(categoryPath)) return 0;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(categoryPath)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return LessIgnoreCase(a.generic_string(), b.generic_string());
    });
    int scanned = 0;
    for (const auto& filePath : files) {

        std::string ext = filePath.extension().string();
        // Case-insensitive extension match
        if (ext.size() != extension.size()) continue;
        bool match = true;
        for (size_t i = 0; i < ext.size(); i++) {
            if (std::tolower(static_cast<unsigned char>(ext[i])) !=
                std::tolower(static_cast<unsigned char>(extension[i]))) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        std::filesystem::path rel = std::filesystem::relative(filePath,
                                                              std::filesystem::path(categoryPath));
        // Category folders and arbitrary nesting beneath them are organizational
        // only; the runtime identity is scope + filename stem.
        const std::string key = BuildAssetKey(rel, extension);
        if (key.empty()) continue;

        std::string absPath = filePath.string();
        std::replace(absPath.begin(), absPath.end(), '\\', '/');
        const u32 crc = p3dHash(key.c_str());

        if (m_verbose) {
            auto existing = index.find(crc);
            if (existing != index.end() && existing->second != absPath) {
                LOG("[ModLoader] '%s' overrides '%s' (was '%s')",
                    modName.c_str(), key.c_str(), existing->second.c_str());
            }
        }

        index[crc] = absPath;
        ++scanned;
    }
    return scanned;
}

bool ModLoader::EnsureStorage() {
    if (!p3d::io::DirExists(kModsDir)) {
        if (!p3d::io::CreateDirectories(kModsDir)) {
            LOG("[ModLoader] Failed to create %s", kModsDir);
            return false;
        }
        LOG("[ModLoader] Created mod directory: %s", kModsDir);
    }
    if (!p3d::io::FileExists(kIniPath)) {
        WriteDefaultIni(kIniPath);
        if (!p3d::io::FileExists(kIniPath)) {
            LOG("[ModLoader] Failed to create %s", kIniPath);
            return false;
        }
        LOG("[ModLoader] Created default %s", kIniPath);
    }
    return true;
}

static bool WriteConfig(const std::string& path, const ModLoaderConfig& config) {
    std::ostringstream out;
    out << "; Mod Loader settings\n\n"
        << "[general]\n"
        << "enabled = " << (config.enabled ? "true" : "false") << "\n"
        << "verbose = " << (config.verbose ? "true" : "false") << "\n\n"
        << "[load_order]\n";
    for (const std::string& name : config.loadOrder) out << name << "\n";
    out << "\n[blacklist]\n";
    for (const std::string& name : config.blacklist) out << name << "\n";

    const bool ok = p3d::io::WriteTextFile(path, out.str());
    if (ok) {
        p3d::io::InvalidateResolveCache(kModsDir);
    }
    return ok;
}

// Query API

bool ModLoader::HasTexture(u32 crc) const {
    return m_textureIndex.find(crc) != m_textureIndex.end();
}

bool ModLoader::HasModel(u32 crc) const {
    return m_modelIndex.find(crc) != m_modelIndex.end();
}

bool ModLoader::HasSound(u32 crc) const {
    return m_soundIndex.find(crc) != m_soundIndex.end();
}

bool ModLoader::HasData(u32 crc) const {
    return m_dataIndex.find(crc) != m_dataIndex.end();
}

const std::string* ModLoader::GetTexturePath(u32 crc) const {
    auto it = m_textureIndex.find(crc);
    return it != m_textureIndex.end() ? &it->second : nullptr;
}

const std::string* ModLoader::GetTexturePath(const char* scope, const char* name) const {
    if (scope && scope[0] && name && name[0]) {
        std::string key = std::string(scope) + "/" + name;
        auto it = m_textureIndex.find(p3dHash(key.c_str()));
        if (it != m_textureIndex.end()) return &it->second;
    }
    if (name && name[0]) {
        auto it = m_textureIndex.find(p3dHash(name));
        if (it != m_textureIndex.end()) return &it->second;
    }
    return nullptr;
}

const std::string* ModLoader::FindTextureOverridePath(const char* scope, const char* name) const {
    if (!name || !name[0]) return nullptr;

    auto toLower = [](const std::string& text) {
        std::string lower = text;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower;
    };

    if (scope && scope[0]) {
        std::string scopedKey = toLower(std::string(scope) + "/" + name);
        auto it = m_textureIndex.find(p3dHash(scopedKey.c_str()));
        if (it != m_textureIndex.end()) return &it->second;
    }

    std::string lowerName = toLower(name);
    auto it = m_textureIndex.find(p3dHash(lowerName.c_str()));
    return it != m_textureIndex.end() ? &it->second : nullptr;
}

bool ModLoader::GetTextureOverrideRGBA(
    const char* scope, const char* name,
    std::vector<u8>& outRgba, int& outWidth, int& outHeight) const {
    const std::string* path = FindTextureOverridePath(scope, name);
    if (!path) return false;

    int channels;
    const std::string resolvedPath = p3d::io::ResolvePath(*path);
    unsigned char* px = stbi_load(resolvedPath.c_str(), &outWidth, &outHeight, &channels, 4);
    if (!px) return false;
    if (IsUnmodifiedRechanPng(*path, px, outWidth, outHeight)) {
        stbi_image_free(px);
        return false;
    }

    outRgba.assign(px, px + static_cast<size_t>(outWidth) * outHeight * 4);
    stbi_image_free(px);
    return true;
}

bool ModLoader::GetTextureOverridePixels(const char* scope, const char* name, s16 width, s16 height, std::vector<u16>& outPixels) const {
    if (!name || !name[0] || width <= 0 || height <= 0) return false;

    const std::string* path = FindTextureOverridePath(scope, name);
    if (!path) return false;

    int pw, ph, channels;
    const std::string resolvedPath = p3d::io::ResolvePath(*path);
    unsigned char* px = stbi_load(resolvedPath.c_str(), &pw, &ph, &channels, 4);
    if (!px) return false;
    if (IsUnmodifiedRechanPng(*path, px, pw, ph)) {
        stbi_image_free(px);
        return false;
    }

    outPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    const int copyW = (pw < width) ? pw : width;
    const int copyH = (ph < height) ? ph : height;
    for (int y = 0; y < copyH; y++) {
        for (int x = 0; x < copyW; x++) {
            const unsigned char* src = px + (static_cast<size_t>(y) * pw + x) * 4;
            const u8 r = src[0], g = src[1], b = src[2], a = src[3];
            const u16 abgr1555 = a <= 127 ? 0 : static_cast<u16>(
                ((a > 127 ? 1u : 0u) << 15) |
                (((u16)(b >> 3)) << 10) |
                (((u16)(g >> 3)) << 5) |
                ((u16)(r >> 3)));
            outPixels[static_cast<size_t>(y) * width + x] = abgr1555;
        }
    }
    stbi_image_free(px);
    return true;
}

bool ModLoader::GetIndexedTextureOverride(
    const char* scope, const char* name,
    s16 indexRectWordW, s16 indexRectH, s16 paletteColorCount,
    std::vector<u16>& outClutColors, std::vector<u16>& outIndexWords) const {
    if (!name || !name[0] || indexRectWordW <= 0 || indexRectH <= 0) return false;
    if (paletteColorCount != 16 && paletteColorCount != 256) return false;

    const std::string* path = FindTextureOverridePath(scope, name);
    if (!path) return false;

    int pw, ph, channels;
    const std::string resolvedPath = p3d::io::ResolvePath(*path);
    unsigned char* px = stbi_load(resolvedPath.c_str(), &pw, &ph, &channels, 4);
    if (!px) return false;
    if (IsUnmodifiedRechanPng(*path, px, pw, ph)) {
        stbi_image_free(px);
        return false;
    }

    const int bppDivisor = (paletteColorCount == 16) ? 4 : 2;
    const int realW = indexRectWordW * bppDivisor;
    const int realH = indexRectH;
    const int copyW = (pw < realW) ? pw : realW;
    const int copyH = (ph < realH) ? ph : realH;

    // Build a palette from the most frequent colors in the source image.
    std::unordered_map<u32, int> histogram;
    bool hasTransparency = false;
    for (int y = 0; y < copyH; y++) {
        for (int x = 0; x < copyW; x++) {
            const unsigned char* src = px + (static_cast<size_t>(y) * pw + x) * 4;
            if (src[3] <= 127) {
                hasTransparency = true;
                continue;
            }
            const u32 key = ((u32)src[0] << 16) | ((u32)src[1] << 8) | (u32)src[2];
            histogram[key]++;
        }
    }

    std::vector<std::pair<u32, int>> sortedColors(histogram.begin(), histogram.end());
    std::sort(sortedColors.begin(), sortedColors.end(),
              [](const std::pair<u32, int>& a, const std::pair<u32, int>& b) { return a.second > b.second; });

    std::vector<u8> paletteR(paletteColorCount, 0), paletteG(paletteColorCount, 0), paletteB(paletteColorCount, 0);
    const int firstOpaque = hasTransparency ? 1 : 0;
    for (int i = firstOpaque; i < paletteColorCount && i - firstOpaque < (int)sortedColors.size(); i++) {
        const u32 key = sortedColors[i - firstOpaque].first;
        paletteR[i] = (u8)((key >> 16) & 0xFF);
        paletteG[i] = (u8)((key >> 8) & 0xFF);
        paletteB[i] = (u8)(key & 0xFF);
    }

    outClutColors.assign(paletteColorCount, 0);
    for (int i = firstOpaque; i < paletteColorCount; i++) {
        outClutColors[i] = static_cast<u16>(
            (1u << 15) | (((u16)(paletteB[i] >> 3)) << 10) | (((u16)(paletteG[i] >> 3)) << 5) | (paletteR[i] >> 3));
    }

    outIndexWords.assign(static_cast<size_t>(indexRectWordW) * indexRectH, 0);
    for (int y = 0; y < realH; y++) {
        for (int x = 0; x < realW; x++) {
            u8 r = 0, g = 0, b = 0;
            u8 a = 0;
            if (y < copyH && x < copyW) {
                const unsigned char* src = px + (static_cast<size_t>(y) * pw + x) * 4;
                r = src[0]; g = src[1]; b = src[2]; a = src[3];
            }

            int bestIdx = (hasTransparency && a <= 127) ? 0 : firstOpaque;
            int bestDist = 0x7fffffff;
            for (int i = firstOpaque; a > 127 && i < paletteColorCount; i++) {
                const int dr = (int)r - (int)paletteR[i];
                const int dg = (int)g - (int)paletteG[i];
                const int db = (int)b - (int)paletteB[i];
                const int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    bestIdx = i;
                }
            }

            const int wordIdx = y * indexRectWordW + (x / bppDivisor);
            if (paletteColorCount == 16) {
                const int shift = (x % 4) * 4;
                outIndexWords[wordIdx] = static_cast<u16>(outIndexWords[wordIdx] | ((bestIdx & 0xF) << shift));
            }
            else {
                const int shift = (x % 2) * 8;
                outIndexWords[wordIdx] = static_cast<u16>(outIndexWords[wordIdx] | ((bestIdx & 0xFF) << shift));
            }
        }
    }

    stbi_image_free(px);
    return true;
}

const std::string* ModLoader::GetModelPath(u32 crc) const {
    auto it = m_modelIndex.find(crc);
    return it != m_modelIndex.end() ? &it->second : nullptr;
}

const std::string* ModLoader::GetSoundPath(u32 crc) const {
    auto it = m_soundIndex.find(crc);
    return it != m_soundIndex.end() ? &it->second : nullptr;
}

const std::string* ModLoader::GetDataPath(u32 crc) const {
    auto it = m_dataIndex.find(crc);
    return it != m_dataIndex.end() ? &it->second : nullptr;
}

bool ModLoader::IsUnmodifiedTextureDump(const char* path) const {
    if (!path || !path[0]) return false;
    int width = 0, height = 0, channels = 0;
    const std::string resolvedPath = p3d::io::ResolvePath(path);
    unsigned char* rgba = stbi_load(resolvedPath.c_str(), &width, &height, &channels, 4);
    if (!rgba) return false;
    const bool unchanged = IsUnmodifiedRechanPng(path, rgba, width, height);
    stbi_image_free(rgba);
    return unchanged;
}

static const std::string* FindScopedPath(
    const std::unordered_map<u32, std::string>& index,
    const char* scope,
    const char* name) {
    if (!name || !name[0]) return nullptr;
    auto lower = [](std::string value) {
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    if (scope && scope[0]) {
        const std::string key = lower(std::string(scope) + "/" + name);
        auto it = index.find(p3dHash(key.c_str()));
        if (it != index.end()) return &it->second;
    }
    const std::string key = lower(name);
    auto it = index.find(p3dHash(key.c_str()));
    return it != index.end() ? &it->second : nullptr;
}

const std::string* ModLoader::FindModelOverridePath(const char* scope, const char* name) const {
    return FindScopedPath(m_modelIndex, scope, name);
}

const std::string* ModLoader::FindDataOverridePath(const char* scope, const char* name) const {
    return FindScopedPath(m_dataIndex, scope, name);
}

const std::string* ModLoader::FindSubtitleOverridePath(const char* name) const {
    if (!name || !name[0]) return nullptr;
    std::string lowerName(name);
    for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = m_subtitleIndex.find(p3dHash(lowerName.c_str()));
    return it != m_subtitleIndex.end() ? &it->second : nullptr;
}

const std::string* ModLoader::FindSoundOverridePath(const char* scope, const char* name) const {
    if (!name || !name[0]) return nullptr;

    auto toLower = [](const std::string& text) {
        std::string lower = text;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower;
    };

    if (scope && scope[0]) {
        std::string scopedKey = toLower(std::string(scope) + "/" + name);
        auto it = m_soundIndex.find(p3dHash(scopedKey.c_str()));
        if (it != m_soundIndex.end()) return &it->second;
    }

    std::string lowerName = toLower(name);
    auto it = m_soundIndex.find(p3dHash(lowerName.c_str()));
    return it != m_soundIndex.end() ? &it->second : nullptr;
}

// Minimal RIFF/WAVE reader for 16-bit PCM (mono or stereo) -- the exact
// format AssetExporter::WritePCMWav produces, and what any common WAV editor
// exports by default.
static bool ReadWavPCM16(const std::string& path, std::vector<s16>& outPcm, u32& outSampleRate, u32& outChannels) {
    auto fileData = p3d::io::ReadFile(p3d::io::ResolvePath(path));
    if (!fileData) return false;
    std::vector<u8> data = std::move(*fileData);
    if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) != 0 || memcmp(data.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    u16 audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    u32 sampleRate = 0;
    const u8* dataChunk = nullptr;
    u32 dataSize = 0;
    bool haveFmt = false;

    u32 pos = 12;
    while (pos + 8 <= data.size()) {
        const u8* chunkId = data.data() + pos;
        u32 chunkSize;
        memcpy(&chunkSize, data.data() + pos + 4, 4);
        const u32 chunkStart = pos + 8;
        if (chunkStart + chunkSize > data.size()) break;

        if (memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            memcpy(&audioFormat, data.data() + chunkStart + 0, 2);
            memcpy(&numChannels, data.data() + chunkStart + 2, 2);
            memcpy(&sampleRate, data.data() + chunkStart + 4, 4);
            memcpy(&bitsPerSample, data.data() + chunkStart + 14, 2);
            haveFmt = true;
        }
        else if (memcmp(chunkId, "data", 4) == 0) {
            dataChunk = data.data() + chunkStart;
            dataSize = chunkSize;
        }

        pos = chunkStart + chunkSize + (chunkSize & 1); // chunks are word-aligned
    }

    if (!haveFmt || !dataChunk || audioFormat != 1 || bitsPerSample != 16 || numChannels == 0) {
        return false;
    }

    const u32 numSamples = dataSize / 2;
    const s16* src = reinterpret_cast<const s16*>(dataChunk);
    outPcm.assign(src, src + numSamples);
    outSampleRate = sampleRate;
    outChannels = numChannels;
    return true;
}

bool ModLoader::GetSoundOverridePCM(
    const char* scope, const char* name,
    std::vector<s16>& outPcm, u32& outSampleRate, u32& outChannels) const {
    const std::string* path = FindSoundOverridePath(scope, name);
    if (!path) return false;

    return ReadWavPCM16(*path, outPcm, outSampleRate, outChannels);
}

const std::vector<ModInfo>& ModLoader::GetMods() const {
    return m_mods;
}

bool ModLoader::IsEnabled() const {
    return m_enabled;
}

bool ModLoader::SetModEnabled(const std::string& folder, bool enabled) {
    ModLoaderConfig config = ReadConfig(kIniPath);
    auto it = std::find_if(config.blacklist.begin(), config.blacklist.end(),
        [&](const std::string& entry) { return EqualsIgnoreCase(entry, folder); });

    if (enabled) {
        if (it != config.blacklist.end()) config.blacklist.erase(it);
    }
    else if (it == config.blacklist.end()) {
        config.blacklist.push_back(folder);
    }

    if (!WriteConfig(kIniPath, config)) {
        LOG("[ModLoader] Failed to save enabled state for '%s'", folder.c_str());
        return false;
    }

    Reload();
    return true;
}

#endif // MOD_LOADER
