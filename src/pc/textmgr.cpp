#include "pc/textmgr.h"
#include "pc/textbackend.h"
#include "gen/common.h"
#include "pc/log.h"
#include "p3d/fileio.h"
#if CUSTOM_TEXT
#include "extra/customtext.h"
#endif


TextManager* g_textManager = nullptr;

TextManager::~TextManager() {
    Shutdown();
}

void TextManager::Init() {
    ResetState();
    m_stateStack.clear();
    m_fonts.clear();
    m_aliases.clear();
    m_nextFontHandle = 1;
    m_warnedNoBackend = false;

    if (!m_backend) {
        m_backend = CreateDefaultTextBackend();
    }

    if (m_backend && !m_backend->Init()) {
        LOG("[TextManager] Backend init failed");
    }
}

void TextManager::Shutdown() {
    if (m_backend) {
        m_backend->Shutdown();
        delete m_backend;
        m_backend = nullptr;
    }

    m_stateStack.clear();
    m_fonts.clear();
    m_aliases.clear();
    ResetState();
    m_warnedNoBackend = false;
}

TextFontHandle TextManager::LoadFont(const TextFontDesc& desc) {
    if (!desc.name || !desc.name[0] || !desc.path || !desc.path[0] || desc.pixelHeight <= 0) {
        LOG("[TextManager] Refusing to load invalid font descriptor");
        return 0;
    }

    if (TextFontHandle existing = FindRegisteredFont(desc.name)) {
        return existing;
    }

    LoadedFont font = {};
    font.handle = m_nextFontHandle++;
    font.name = desc.name;
    font.path = desc.path;
    font.pixelHeight = desc.pixelHeight;

    if (!ReadWholeFile(desc.path, font.fileData)) {
        LOG("[TextManager] Failed to read font file: %s", desc.path);
        return 0;
    }

    if (m_backend) {
        TextBackendFontDesc backendDesc = {};
        backendDesc.handle = font.handle;
        backendDesc.name = font.name.c_str();
        backendDesc.fileData = font.fileData.data();
        backendDesc.fileSize = (s32)font.fileData.size();
        backendDesc.pixelHeight = font.pixelHeight;

        if (!m_backend->RegisterFont(backendDesc)) {
            LOG("[TextManager] Backend failed to register font: %s", desc.name);
            return 0;
        }
    }

    m_fonts.push_back(font);
    return font.handle;
}

bool TextManager::AddFontStrike(const char* name, s32 pixelHeight, bool bakedOutline) {
    const TextFontHandle handle = FindRegisteredFont(name);
    if (!handle || !m_backend) {
        return false;
    }
    return m_backend->AddStrike(handle, pixelHeight, bakedOutline);
}

void TextManager::SetFontAlias(const char* name, const char* target) {
    if (!name || !name[0]) {
        return;
    }

    const TextFontHandle handle = FindRegisteredFont(target);
    for (s32 i = 0; i < (s32)m_aliases.size(); i++) {
        if (m_aliases[(size_t)i].name == name) {
            if (handle) {
                m_aliases[(size_t)i].handle = handle;
            }
            else {
                m_aliases.erase(m_aliases.begin() + i);
            }
            return;
        }
    }

    if (handle) {
        FontAlias alias = {};
        alias.name = name;
        alias.handle = handle;
        m_aliases.push_back(alias);
    }
}

TextFontHandle TextManager::FindFont(const char* name) const {
    if (!name || !name[0]) {
        return 0;
    }

    for (s32 i = 0; i < (s32)m_aliases.size(); i++) {
        if (m_aliases[(size_t)i].name == name) {
            return m_aliases[(size_t)i].handle;
        }
    }

    return FindRegisteredFont(name);
}

TextFontHandle TextManager::FindRegisteredFont(const char* name) const {
    if (!name || !name[0]) {
        return 0;
    }

    for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
        if (m_fonts[i].name == name) {
            return m_fonts[i].handle;
        }
    }

    return 0;
}

void TextManager::ResetState() {
    m_state = {};
}

void TextManager::PushState() {
    m_stateStack.push_back(m_state);
}

void TextManager::PopState() {
    if (m_stateStack.empty()) {
        return;
    }

    m_state = m_stateStack.back();
    m_stateStack.pop_back();
}

void TextManager::SetFont(TextFontHandle font) {
    m_state.font = font;
}

bool TextManager::SetFontByName(const char* name) {
    TextFontHandle handle = FindFont(name);
    if (!handle) {
        return false;
    }

    m_state.font = handle;
    return true;
}

void TextManager::SetScale(f32 scaleX, f32 scaleY) {
    m_state.scaleX = scaleX;
    m_state.scaleY = scaleY;
}

void TextManager::SetWrapWidth(f32 wrapWidth) {
    m_state.wrapWidth = wrapWidth;
}

void TextManager::SetLineSpacing(s32 lineSpacing) {
    m_state.lineSpacing = lineSpacing;
}

void TextManager::SetAlignment(TextAlign align) {
    m_state.align = align;
}

void TextManager::SetColor(u8 r, u8 g, u8 b, u8 a) {
    m_state.color = { r, g, b, a };
}

void TextManager::SetShadow(bool enabled, f32 offsetX, f32 offsetY,
                            u8 r, u8 g, u8 b, u8 a) {
    m_state.shadow.enabled = enabled;
    m_state.shadow.offsetX = offsetX;
    m_state.shadow.offsetY = offsetY;
    m_state.shadow.color = { r, g, b, a };
}

void TextManager::SetOutline(bool enabled, f32 thickness,
                             u8 r, u8 g, u8 b, u8 a) {
    m_state.outline.enabled = enabled;
    m_state.outline.thickness = thickness;
    m_state.outline.color = { r, g, b, a };
}

void TextManager::SetPromptsEnabled(bool enabled) {
    m_state.promptsEnabled = enabled;
}

TextBounds TextManager::MeasureString(Utf8TextView text) const {
    if (m_backend) {
        return m_backend->Measure(text, m_state);
    }

    TextBounds bounds = {};
    if (!text.IsEmpty()) {
        bounds.lineCount = 1;
    }
    return bounds;
}

s32 TextManager::CountWrappedLines(Utf8TextView text) const {
    if (m_backend) {
        return m_backend->CountWrappedLines(text, m_state);
    }

    return text.IsEmpty() ? 0 : 1;
}

TextBounds TextManager::MeasureToken(const char* token) const {
#if CUSTOM_TEXT
    return MeasureString(g_customText.GetText(token));
#else
    (void)token;
    return {};
#endif
}

s32 TextManager::CountWrappedLinesToken(const char* token) const {
#if CUSTOM_TEXT
    return CountWrappedLines(g_customText.GetText(token));
#else
    (void)token;
    return 0;
#endif
}

void TextManager::PrintToken(const char* token, f32 x, f32 y) const {
#if CUSTOM_TEXT
    PrintString(g_customText.GetText(token), x, y);
#else
    (void)token;
    (void)x;
    (void)y;
#endif
}

void TextManager::PrintString(const char* text, f32 x, f32 y) const {
    PrintString(Utf8TextView(text), x, y);
}

void TextManager::PrintString(Utf8TextView text, f32 x, f32 y) const {
    if (text.IsEmpty()) {
        return;
    }

    const LoadedFont* font = FindLoadedFont(m_state.font);
    if (!font) {
        LOG("[TextManager] PrintString called without a valid font selected");
        return;
    }

    if (m_backend) {
        m_backend->Draw(text, x, y, m_state);
    }

    if ((!m_backend || !m_backend->IsReady()) && !m_warnedNoBackend) {
        const_cast<TextManager*>(this)->m_warnedNoBackend = true;
        LOG("[TextManager] Unicode shaping/raster backend is not wired yet; PrintString is currently inert");
    }
}

bool TextManager::ReadWholeFile(const char* path, std::vector<u8>& outData) {
    outData.clear();

    if (!path || !path[0]) {
        return false;
    }

    auto data = p3d::io::ReadFile(p3d::io::ResolvePath(path));
    if (!data || data->empty()) {
        return false;
    }

    outData = std::move(*data);
    return true;
}

const TextManager::LoadedFont* TextManager::FindLoadedFont(TextFontHandle handle) const {
    if (!handle) {
        return nullptr;
    }

    for (s32 i = 0; i < (s32)m_fonts.size(); i++) {
        if (m_fonts[i].handle == handle) {
            return &m_fonts[i];
        }
    }

    return nullptr;
}
