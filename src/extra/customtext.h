#pragma once
#include "core.h"
#include "gen/utf8text.h"
#include <string>
#include <vector>

enum GameLanguage : s32 {
    LangEnglish = 0,
    LangGerman,
    LangFrench,
    LangItalian,
    LangSpanish,
    LangPortuguese,
    NumLanguages,
};

// CustomText manages localized text strings loaded from plain text files.
// Uses xcHash algorithm for token lookup (consistent with xclib).
// Simple vector-based storage with linear search (no overhead).
// File format:
//   [TOKEN]      (string token, max 16 chars)
//   [0x1234ABCD] (explicit hash token)
//   Actual string text
//   [TOKEN2]
//   Another string
//
class CustomText {
public:
    CustomText() = default;
    ~CustomText() = default;

    // Initialize text system and load strings for the current language
    void Init();

    // Shutdown and clear all loaded strings
    void Shutdown();

    // Change language and reload text files (triggers Init internally)
    void SetLanguage(GameLanguage language);

    // Get a localized string by token name (hashed with xcHash). Returns nullptr if not found.
    const char* GetString(const char* token) const;

    // Get a localized UTF-8 view by token name. Returns empty view if not found.
    Utf8TextView GetText(const char* token) const;

    // Get a localized string by pre-computed hash. Returns nullptr if not found.
    const char* GetStringByHash(u32 hash) const;

    // Get a localized UTF-8 view by pre-computed hash. Returns empty view if not found.
    Utf8TextView GetTextByHash(u32 hash) const;

    // Get current language
    GameLanguage GetLanguage() const { return lang; }

    // Get current localization ID (for tracking if language changed)
    s32 GetLocalizationID() const { return localizationID; }

private:
    struct StringEntry {
        u32 hash = 0;
        std::string text = {};
    };

    bool LoadTextFile(const char* path);
    bool TryParseTokenHash(const std::string& token, u32* outHash);
    void ClearStrings();

    GameLanguage lang = LangEnglish;
    s32 localizationID = 0; 
    std::vector<StringEntry> strings;
};

extern CustomText g_customText;