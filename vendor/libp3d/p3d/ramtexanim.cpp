#include "p3d/ramtexanim.h"
#include "p3d/chunkfile.h"
#include "p3d/inventory.h"

#include <cstring>
#include <string>
#include <vector>

tRAMTexAnim::tRAMTexAnim() = default;

tRAMTexAnim::~tRAMTexAnim() {
    delete[] textures;
    textures = nullptr;

    delete[] frameData;
    frameData = nullptr;
}

s32 tRAMTexAnim::GetNumFrames() const {
    return numFrames;
}

tFlipbook* tRAMTexAnim::MakePuppet() {
    auto* flipbook = new tRAMTexFlip();
    flipbook->SetAnimation(this);
    flipbook->Reset();
    return flipbook;
}

s32 tRAMTexAnim::GetFrame(s32 index) const {
    if (!frameData || index < 0 || index >= numFrames) {
        return -1;
    }

    return frameData[index];
}

s32 tRAMTexAnim::GetNumTextures() const {
    return numTextures;
}

tTexture* tRAMTexAnim::GetTexture(s32 index) const {
    if (!textures || index < 0 || index >= numTextures) {
        return nullptr;
    }

    return textures[index];
}

void tRAMTexAnim::SetNumFrames(s32 count) {
    numFrames = count;
}

void tRAMTexAnim::SetFrameData(u8* data) {
    delete[] frameData;
    frameData = data;
}

void tRAMTexAnim::SetNumTextures(s32 count) {
    numTextures = count;
}

void tRAMTexAnim::SetTextureData(tTexture** data) {
    delete[] textures;
    textures = data;
}

void tRAMTexFlip::Reset() {
    SetFrame(0);
    lastUploadedFrame = -1;
}

void tRAMTexFlip::Update() {
    auto* anim = static_cast<tRAMTexAnim*>(GetAnimation());
    if (!anim) {
        return;
    }

    const s32 frameIndex = anim->GetFrame(GetFrame());
    if (frameIndex == lastUploadedFrame) {
        return;
    }

    lastUploadedFrame = frameIndex;

    tTexture* texture = anim->GetTexture(frameIndex);
    if (!texture) {
        return;
    }

    tTextureRect* rect = texture->GetTextureRect();
    const u8* textureData = texture->GetTextureData();
    if (!rect || !textureData) {
        return;
    }

    // The PSP path copies these textures directly into the emulated PSX VRAM.
    // Reject malformed stream metadata before the nested copy loop can walk
    // outside the texture buffer or spend an unbounded amount of time copying.
    if (rect->x < 0 || rect->y < 0 || rect->w <= 0 || rect->h <= 0 ||
        rect->x >= 1024 || rect->y >= 512 || rect->w > 1024 - rect->x ||
        rect->h > 512 - rect->y ||
        texture->GetTextureDataSize() < static_cast<u32>(rect->w) * rect->h * 2u) {
        return;
    }

    if (rect->h == 1) {
        rect->x = 768;
        rect->y = 143;
    } else {
        rect->x = 448;
        rect->y = 0;
    }

    if (p3d::rawTextureUploader) {
        p3d::rawTextureUploader(rect->x, rect->y, rect->w, rect->h, textureData);
    }
}

void tRAMTexAnimLoader::LoadChunk(tChunkFile* file, tInventory* store) {
    if (!store) {
        return;
    }

    const std::string animName = file->GetPString();
    std::vector<tTexture*> textures;
    std::vector<u8> frameData;

    while (file->ChunksRemaining()) {
        const u16 chunkId = file->BeginChunk();

        if (chunkId == 0x6051) {
            const s32 textureCount = file->GetInt();
            if (textureCount > 0) {
                textures.reserve(static_cast<size_t>(textureCount));
                for (s32 i = 0; i < textureCount; ++i) {
                    const std::string textureName = file->GetPString();
                    textures.push_back(store->Find<tTexture>(textureName));
                }
            }
        } else if (chunkId == 0x6052) {
            const s32 frameCount = file->GetInt();
            if (frameCount > 0) {
                frameData.resize(static_cast<size_t>(frameCount));
                file->GetData(frameData.data(), static_cast<u32>(frameCount));
            }
        }

        file->EndChunk();
    }

    auto* anim = new tRAMTexAnim();
    anim->SetName(animName.c_str());
    anim->SetNumTextures(static_cast<s32>(textures.size()));
    anim->SetNumFrames(static_cast<s32>(frameData.size()));

    if (!textures.empty()) {
        auto** textureDataPtrs = new tTexture*[textures.size()];
        std::memcpy(textureDataPtrs, textures.data(), textures.size() * sizeof(tTexture*));
        anim->SetTextureData(textureDataPtrs);
    }

    if (!frameData.empty()) {
        auto* frameBytes = new u8[frameData.size()];
        std::memcpy(frameBytes, frameData.data(), frameData.size());
        anim->SetFrameData(frameBytes);
    }

    store->Store(anim);
    anim->Release();
}
