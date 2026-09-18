#pragma once
#include "core.h"
#include "gen/config.h"

#if MOD_LOADER

// Simple PCM audio buffer from a loaded WAV file.
struct WAVAudioBuffer {
    u8* data = nullptr;
    u32 size = 0;       // total bytes
    u32 sampleRate = 0;
    u16 channels = 0;
    u16 bitsPerSample = 0;

    ~WAVAudioBuffer() { delete[] data; }

    // Disallow copy, allow move
    WAVAudioBuffer() = default;
    WAVAudioBuffer(const WAVAudioBuffer&) = delete;
    WAVAudioBuffer& operator=(const WAVAudioBuffer&) = delete;
    WAVAudioBuffer(WAVAudioBuffer&& other) noexcept
        : data(other.data), size(other.size), sampleRate(other.sampleRate),
          channels(other.channels), bitsPerSample(other.bitsPerSample) {
        other.data = nullptr;
        other.size = 0;
    }
    WAVAudioBuffer& operator=(WAVAudioBuffer&& other) noexcept {
        if (this != &other) {
            delete[] data;
            data = other.data;
            size = other.size;
            sampleRate = other.sampleRate;
            channels = other.channels;
            bitsPerSample = other.bitsPerSample;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
};

// Loads WAV audio files into raw PCM buffers.
// Uses miniaudio (already integrated via vendor/miniaudio).
class WAVLoader {
public:
    // Load a WAV file and return the decoded PCM data.
    // Returns an empty buffer (data == nullptr) on failure.
    static WAVAudioBuffer LoadFromFile(const char* path);
};

#endif // MOD_LOADER