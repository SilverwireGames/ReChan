#pragma once
#include "core.h"
#include "gen/config.h"
#include <string>
#include <unordered_map>
#include <vector>

#ifdef MOD_LOADER

struct ModInfo {
    std::string name;
    std::string folder;
    std::string version;
    std::string author;
    int textureCount = 0;
    int modelCount = 0;
    int soundCount = 0;
    int dataCount = 0;
    int priority = 0;
    bool enabled = false;
};

class ModLoader {
public:
    ModLoader() = default;
    ~ModLoader() = default;

    static ModLoader& Instance();

    void Init();
    void Reload();
    void Shutdown();
    bool EnsureStorage();

    [[nodiscard]] bool HasTexture(u32 crc) const;
    [[nodiscard]] bool HasModel(u32 crc) const;
    [[nodiscard]] bool HasSound(u32 crc) const;
    [[nodiscard]] bool HasData(u32 crc) const;

    [[nodiscard]] const std::string* GetTexturePath(u32 crc) const;
    [[nodiscard]] const std::string* GetModelPath(u32 crc) const;
    [[nodiscard]] const std::string* GetSoundPath(u32 crc) const;
    [[nodiscard]] const std::string* GetDataPath(u32 crc) const;

    // Manifest-free scoped lookup. Files under levels/<level>/... or
    // characters/<character>/... are indexed as "<scope>/<filename>".
    // A file outside a recognized scope is a global fallback by filename.
    [[nodiscard]] const std::string* FindModelOverridePath(const char* scope, const char* name) const;
    [[nodiscard]] const std::string* FindDataOverridePath(const char* scope, const char* name) const;
    [[nodiscard]] const std::string* FindSubtitleOverridePath(const char* name) const;

    // Two-pass texture lookup: tries "{scope}/{name}" first, then "{name}" alone.
    // Use scope = character/level name so jackie/face.png wins over face.png for Jackie,
    // but a flat face.png still matches any character that lacks a specific override.
    [[nodiscard]] const std::string* GetTexturePath(const char* scope, const char* name) const;

    // For named direct-color (15bpp) VRAM-rect texture chunks: looks up a mod PNG by
    // resource name (case-insensitive), trying "{scope}/{name}" first then "{name}"
    // alone (same convention as GetTexturePath(scope, name)), and decodes it into a
    // PSX-format ABGR1555 pixel buffer of exactly width*height pixels, cropping/padding
    // as needed so it can be handed straight to World::UploadToVRAM.
    // Returns false if no override is found.
    [[nodiscard]] bool GetTextureOverridePixels(const char* scope, const char* name, s16 width, s16 height, std::vector<u16>& outPixels) const;

    // For named CLUT-indexed (4bpp/8bpp) VRAM-rect texture pairs: looks up a mod PNG by
    // resource name and re-quantizes it into a palette of paletteColorCount entries
    // (16 or 256) plus matching packed index words, using the exact bit layout
    // PsxVRAM::DecodePage expects (4 indices/word at 4bpp, 2 indices/word at 8bpp).
    // indexRectWordW/indexRectH are the index chunk's VRAM rect size in 16-bit words.
    // Returns false if no override is found.
    [[nodiscard]] bool GetIndexedTextureOverride(
        const char* scope, const char* name,
        s16 indexRectWordW, s16 indexRectH, s16 paletteColorCount,
        std::vector<u16>& outClutColors, std::vector<u16>& outIndexWords) const;

    [[nodiscard]] const std::string* FindTextureOverridePath(const char* scope, const char* name) const;
    [[nodiscard]] bool IsUnmodifiedTextureDump(const char* path) const;

    // Two-pass sound lookup (same "{scope}/{name}" then "{name}" convention as
    // textures), decoded into 16-bit PCM via a small built-in WAV reader.
    // Matches the asset exporter's own output layout exactly, so a dumped and
    // re-encoded WAV can be dropped straight back in as a mod:
    //   WAX bank sample:  scope="rs0000", name="sample00"  (rs0000/sample00.wav)
    //   FAG music track:  scope=nullptr,  name="title"     (title.wav)
    // Returns false if no override is found or the WAV isn't 16-bit PCM.
    [[nodiscard]] bool GetSoundOverridePCM(
        const char* scope, const char* name,
        std::vector<s16>& outPcm, u32& outSampleRate, u32& outChannels) const;

    [[nodiscard]] const std::string* FindSoundOverridePath(const char* scope, const char* name) const;

    // Like GetTextureOverridePixels, but returns the override PNG's raw RGBA8
    // pixels at its own native resolution -- no cropping/padding/quantization
    // to a PSX VRAM rect. outWidth/outHeight receive the PNG's actual size.
    // Used by the real-texture rendering path (see REAL_TEXTURE_RENDERING).
    // Returns false if no override is found.
    [[nodiscard]] bool GetTextureOverrideRGBA(
        const char* scope, const char* name,
        std::vector<u8>& outRgba, int& outWidth, int& outHeight) const;

    [[nodiscard]] const std::vector<ModInfo>& GetMods() const;
    [[nodiscard]] bool IsEnabled() const;
    bool SetModEnabled(const std::string& folder, bool enabled);

    ModLoader(const ModLoader&) = delete;
    ModLoader& operator=(const ModLoader&) = delete;

    void ScanModFolder(const std::string& folderPath, const std::string& modName, int priority);
    int ScanCategory(
        const std::string& categoryPath,
        const std::string& modName,
        const std::string& extension,
        std::unordered_map<u32, std::string>& index);

    bool m_initialized = false;
    bool m_enabled = true;
    bool m_verbose = false;
    std::unordered_map<u32, std::string> m_textureIndex;
    std::unordered_map<u32, std::string> m_modelIndex;
    std::unordered_map<u32, std::string> m_soundIndex;
    std::unordered_map<u32, std::string> m_dataIndex;
    std::unordered_map<u32, std::string> m_subtitleIndex;
    std::vector<ModInfo> m_mods;
};

#endif // MOD_LOADER
