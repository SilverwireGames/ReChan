// The PSP mixer as of commit 9c93abf (per-frame loops), kept verbatim as the
// bit-exact reference for the segmented mixer. Included by audio_mixer.cpp
// after psp_audio.cpp, inside its anonymous namespace's scope.
#pragma once

namespace {

static void ReferenceMixVoices(s32* mix, u32 requestedFrames) {
    for (Voice& voice : g_voices) {
        if (!voice.active) continue;
        Sample* sample = GetSample(voice.sample);
        if (!sample || sample->frames == 0) {
            voice.active = false;
            continue;
        }

        // Pan scales are fixed for the callback; only the volume fades.
        const float clampedPan = std::clamp(voice.pan, -1.0f, 1.0f);
        const float leftScale = g_masterVolume * (1.0f - clampedPan) * 0.5f;
        const float rightScale = g_masterVolume * (1.0f + clampedPan) * 0.5f;
        float left = voice.volume * leftScale;
        float right = voice.volume * rightScale;
        if (g_mono) right = left = (left + right) * 0.5f;
        s32 leftGain = GainToFixed(left);
        s32 rightGain = GainToFixed(right);
        const u64 sampleEnd = static_cast<u64>(sample->frames) * kCursorOne;
        u64 step = CachedCursorStep(voice, sample->rate);
        // Reduce large looping strides once. The inner loop only needs one
        // subtraction to wrap, including one-frame clips and extreme pitch.
        if (voice.loop) {
            if (step >= sampleEnd) step %= sampleEnd;
            if (voice.cursor >= sampleEnd) voice.cursor %= sampleEnd;
        }

        // The per-frame fade v += (t - v) / remaining is a linear ramp, so its
        // increment is constant: compute it once per callback (re-derived each
        // callback, and the last fade frame lands exactly on the target).
        const float fadeStep = voice.fadeFrames > 0
            ? (voice.targetVolume - voice.volume) / static_cast<float>(voice.fadeFrames)
            : 0.0f;

        const s16* pcm = sample->pcm.data();
        const u32 channels = sample->channels;
        const u32 lastFrame = sample->frames - 1;
        // Q15 source value: mono as is, stereo as the exact average, wider
        // layouts as their mean.
        const auto readQ15 = [&](u32 source, u32 next, u32 fraction) -> s32 {
            if (channels == 1) return LerpQ15(pcm[source], pcm[next], fraction);
            if (channels == 2) {
                const s32 current = pcm[source * 2] + pcm[source * 2 + 1];
                const s32 following = pcm[next * 2] + pcm[next * 2 + 1];
                return current * (1 << (kLerpBits - 1))
                    + static_cast<s32>((static_cast<s64>(following - current) * fraction) >> 1);
            }
            s32 current = 0;
            s32 following = 0;
            for (u32 channel = 0; channel < channels; ++channel) {
                current += pcm[source * channels + channel];
                following += pcm[next * channels + channel];
            }
            const s64 value = static_cast<s64>(current) * (1 << kLerpBits)
                + static_cast<s64>(following - current) * fraction;
            return static_cast<s32>(value / static_cast<s32>(channels));
        };

        for (u32 frame = 0; frame < requestedFrames; ++frame) {
            if (voice.cursor >= sampleEnd) {
                if (voice.loop) voice.cursor -= sampleEnd;
                else {
                    voice.active = false;
                    break;
                }
            }

            const u32 sourceFrame = static_cast<u32>(voice.cursor >> kCursorFractionBits);
            const u32 nextFrame = std::min<u32>(sourceFrame + 1, lastFrame);
            const s32 value = readQ15(sourceFrame, nextFrame, CursorLerpFraction(voice.cursor));
            const u32 outputIndex = frame * 2;
            mix[outputIndex] += ScaleToMix(value, leftGain);
            mix[outputIndex + 1] += ScaleToMix(value, rightGain);
            voice.cursor += step;
            if (voice.fadeFrames > 0) {
                --voice.fadeFrames;
                voice.volume = voice.fadeFrames == 0 ? voice.targetVolume : voice.volume + fadeStep;
                // Matches the previous mixer: fade updates use the panned
                // (non-mono-averaged) gains.
                leftGain = GainToFixed(voice.volume * leftScale);
                rightGain = GainToFixed(voice.volume * rightScale);
            }
        }
    }
}

static void ReferenceMixMusicStream(s32* mix, u32 requestedFrames) {
    g_musicMixers.fetch_add(1, std::memory_order_acquire);
    if (!g_musicStream.active.load(std::memory_order_acquire)
        || g_musicStream.totalFrames == 0) {
        g_musicMixers.fetch_sub(1, std::memory_order_release);
        return;
    }

    bool underrun = false;
    u32 underrunFrame = requestedFrames;
    u32 mixedFrames = 0;
    s32 holdLeft = 0;
    s32 holdRight = 0;
    bool held = false;
    const u64 streamEnd = static_cast<u64>(g_musicStream.totalFrames) * kCursorOne;
    // Linear fade: constant increment per callback (see the voice mixer).
    const float musicFadeStep = g_musicStream.fadeFrames > 0
        ? (g_musicStream.targetVolume - g_musicStream.volume) / static_cast<float>(g_musicStream.fadeFrames)
        : 0.0f;
    const u32 framesPerPair = g_musicStream.framesPerPair;
    s32 gain = GainToFixed(g_musicStream.volume * g_masterVolume);
    // First source frame of the pair being read: the per-frame pair/offset
    // division only runs when the cursor leaves it.
    u32 knownPair = kInvalidPair;
    u32 knownFirst = 0;
    for (u32 frame = 0; frame < requestedFrames; ++frame) {
        if (g_musicStream.cursor >= streamEnd) {
            if (!g_musicStream.loop) {
                g_musicStream.active.store(false, std::memory_order_release);
                break;
            }
            g_musicStream.cursor = 0;
            // A one-pair track is already resident in the current buffer.
            // Invalidating it here would make the callback wait for a pair
            // that the worker will never request again.
            if (g_musicStream.pairCount > 1u) {
                g_musicStream.currentPair = kInvalidPair;
            }
        }

        const u32 sourceFrame = static_cast<u32>(g_musicStream.cursor >> kCursorFractionBits);
        if (knownPair == kInvalidPair || knownPair != g_musicStream.currentPair
            || sourceFrame - knownFirst >= framesPerPair) {
            const u32 pair = sourceFrame / framesPerPair;
            if (pair >= g_musicStream.pairCount) break;
            if (pair != g_musicStream.currentPair) {
                if (!PromoteMusicPair(pair)) {
                    // A slow Memory Stick or worker can miss a period. Keep
                    // the stream alive and hold the last sample for this
                    // period instead of producing a long zero-filled hole.
                    underrun = true;
                    underrunFrame = frame;
                    break;
                }
                const u32 nextPair = pair + 1u < g_musicStream.pairCount ? pair + 1u : 0u;
                if (g_musicStream.pairCount > 1u) RequestMusicPair(nextPair);
            }
            knownPair = pair;
            knownFirst = pair * framesPerPair;
        }

        const u32 currentBuffer = g_musicStream.currentBuffer.load(std::memory_order_acquire);
        if (currentBuffer >= g_musicStream.chunks.size()) {
            underrun = true;
            underrunFrame = frame;
            break;
        }
        const MusicChunk& chunkData = g_musicStream.chunks[currentBuffer];
        const u32 localFrame = sourceFrame - knownFirst;
        const u32 nextFrame = std::min<u32>(localFrame + 1, framesPerPair - 1);
        const u32 fraction = CursorLerpFraction(g_musicStream.cursor);
        const s32 left = LerpQ15(chunkData.left[localFrame], chunkData.left[nextFrame], fraction);
        const s32 right = LerpQ15(chunkData.right[localFrame], chunkData.right[nextFrame], fraction);
        s32 outLeft = ScaleToMix(left, gain);
        s32 outRight = ScaleToMix(right, gain);
        if (g_mono) {
            outLeft = outRight = (outLeft + outRight) / 2;
        }
        const u32 outputIndex = frame * 2;
        mix[outputIndex] += outLeft;
        mix[outputIndex + 1] += outRight;
        holdLeft = outLeft;
        holdRight = outRight;
        held = true;

        g_musicStream.cursor += kCursorOne / 2u;
        ++mixedFrames;
        if (g_musicStream.fadeFrames > 0) {
            --g_musicStream.fadeFrames;
            g_musicStream.volume = g_musicStream.fadeFrames == 0
                ? g_musicStream.targetVolume
                : g_musicStream.volume + musicFadeStep;
            gain = GainToFixed(g_musicStream.volume * g_masterVolume);
        }
    }
    if (held) {
        constexpr float kMixScale = 1.0f / static_cast<float>(1u << kMixFractionBits);
        g_musicStream.holdLeft = static_cast<float>(holdLeft) * kMixScale;
        g_musicStream.holdRight = static_cast<float>(holdRight) * kMixScale;
    }

    if (underrun) {
        PspPerf::RecordAudioUnderrun();
        const s32 fillLeft = static_cast<s32>(static_cast<s16>(std::clamp(
            g_musicStream.holdLeft, -32768.0f, 32767.0f))) * (1 << kMixFractionBits);
        const s32 fillRight = static_cast<s32>(static_cast<s16>(std::clamp(
            g_musicStream.holdRight, -32768.0f, 32767.0f))) * (1 << kMixFractionBits);
        // Only the music adds after the voices: fill the remainder where the
        // voices left silence with a stable tail sample, so an underrun does
        // not click.
        for (u32 frame = underrunFrame; frame < requestedFrames; ++frame) {
            const u32 outputIndex = frame * 2;
            if (mix[outputIndex] == 0) mix[outputIndex] = fillLeft;
            if (mix[outputIndex + 1] == 0) mix[outputIndex + 1] = fillRight;
        }
    }

    PspPerf::RecordAudioMusicMix(mixedFrames);

    g_musicMixers.fetch_sub(1, std::memory_order_release);
}

} // namespace
