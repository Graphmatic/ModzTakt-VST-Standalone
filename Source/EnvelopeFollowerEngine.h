#pragma once

// ============================================================
//  EnvelopeFollowerEngine.h
//  AVAILABILITY: Linux Standalone + MODZTAKT_OVERBRIDGE builds ONLY.
//
//  Computes a per-block smoothed amplitude level for each of the
//  12 Syntakt "Audio Track" Overbridge channels (OB ch indices 2–13).
//
//  Thread model
//  ────────────
//  pushSamples()   — called on the OverbridgeEngine USB thread
//                    (via OverbridgeEngine::setAuxAudioCallback).
//  getTrackLevel() — called on the JUCE audio/MIDI processing thread
//                    (inside ModzTaktAudioProcessor::processBlock).
//
//  The boundary is a per-track std::atomic<float>: pushSamples() writes
//  after running the one-pole IIR smoother; processBlock() reads it.
//
//  Attack / release coefficients are std::atomic<float> so the
//  message thread can update them without synchronisation overhead.
// ============================================================

#if JUCE_LINUX \
    && defined (JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone \
    && defined (MODZTAKT_OVERBRIDGE) && MODZTAKT_OVERBRIDGE

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

class EnvelopeFollowerEngine
{
public:
    // Number of Audio Tracks exposed (Audio Track 1 … 12)
    static constexpr int kNumAudioTracks        = 12;

    // OverbridgeEngine channel index of Audio Track 1 (from the 20-ch layout)
    // ch 0 = Main L, ch 1 = Main R, ch 2 = Audio Track 1 … ch 13 = Audio Track 12
    static constexpr int kFirstAudioTrackOBch   = 2;

    // OverbridgeEngine always runs at 48 kHz
    static constexpr float kSampleRate          = 48000.0f;

    // ── Construction ─────────────────────────────────────────
    EnvelopeFollowerEngine()
    {
        // 1ms attack: fast enough to catch drum transients (which peak
        // within a handful of samples), while still smoothing HF noise.
        // 200ms release: smooth tail after the signal ends.
        setSmoothingMs (1.0f, 200.0f);

        for (int i = 0; i < kNumAudioTracks; ++i)
        {
            envelopeState[i] = 0.0f;
            trackLevel[i].store (0.0f, std::memory_order_relaxed);
        }
    }

    // ── Configuration (call from message thread) ──────────────

    /** Set the one-pole IIR smoothing times.
        attackMs  — how quickly the follower catches rising signals  (default 10 ms)
        releaseMs — how slowly it falls after the signal drops       (default 200 ms) */
    void setSmoothingMs (float attackMs, float releaseMs) noexcept
    {
        const float a = std::exp (-1.0f / (juce::jmax (0.1f, attackMs)  * 0.001f * kSampleRate));
        const float r = std::exp (-1.0f / (juce::jmax (0.1f, releaseMs) * 0.001f * kSampleRate));
        attackCoeff.store  (a, std::memory_order_relaxed);
        releaseCoeff.store (r, std::memory_order_relaxed);
    }

    // ── Audio push (USB callback thread) ─────────────────────

    /** Called by the OverbridgeEngine aux audio callback.
        samples[ch]  — numFrames float32 samples, non-interleaved (20 channels total).
        Only Audio Track channels (OB ch 2 … 13) are processed here. */
    void pushSamples (const float* const* samples,
                      int                 numChannels,
                      int                 numFrames) noexcept
    {
        const float atk = attackCoeff.load  (std::memory_order_relaxed);
        const float rel = releaseCoeff.load (std::memory_order_relaxed);

        for (int t = 0; t < kNumAudioTracks; ++t)
        {
            const int ch = kFirstAudioTrackOBch + t; // OB channel index (2 … 13)
            if (ch >= numChannels)
                break;

            const float* src = samples[ch];
            float env = envelopeState[t];

            for (int f = 0; f < numFrames; ++f)
            {
                const float s     = std::abs (src[f]);
                const float coeff = (s > env) ? atk : rel;
                env = s + coeff * (env - s);   // one-pole IIR follower
            }

            envelopeState[t] = env;

            // Publish the clamped level for the audio/MIDI thread to read
            trackLevel[t].store (juce::jlimit (0.0f, 1.0f, env),
                                 std::memory_order_release);
        }
    }

    // ── Level read (audio / MIDI thread) ─────────────────────

    /** Get the current smoothed amplitude for a given Audio Track.
        @param trackIndex  0-based index (0 = Audio Track 1, 11 = Audio Track 12).
        @return            Value in [0, 1]. Returns 0 for out-of-range indices. */
    float getTrackLevel (int trackIndex) const noexcept
    {
        if (trackIndex < 0 || trackIndex >= kNumAudioTracks)
            return 0.0f;
        return trackLevel[trackIndex].load (std::memory_order_acquire);
    }

    // ── Reset ─────────────────────────────────────────────────

    /** Zero all levels and envelope states (e.g. when the follower is disabled). */
    void reset() noexcept
    {
        for (int i = 0; i < kNumAudioTracks; ++i)
        {
            envelopeState[i] = 0.0f;
            trackLevel[i].store (0.0f, std::memory_order_relaxed);
        }
    }

private:
    // ── Per-track IIR state (written & read only on the USB thread) ──
    float envelopeState[kNumAudioTracks] {};

    // ── Published amplitude levels (USB thread writes, audio thread reads) ──
    std::atomic<float> trackLevel[kNumAudioTracks];

    // ── Smoothing coefficients (writable from message thread) ──
    std::atomic<float> attackCoeff  { 0.0f };
    std::atomic<float> releaseCoeff { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeFollowerEngine)
};

#endif // JUCE_LINUX && JucePlugin_Build_Standalone && MODZTAKT_OVERBRIDGE