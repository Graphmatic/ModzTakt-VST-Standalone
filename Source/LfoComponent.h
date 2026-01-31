#pragma once
#include <JuceHeader.h>
#include <array>

#include "SyntaktParameterTable.h"

namespace modztakt::lfo
{
    // Keep the same IDs you already use in the UI / APVTS.
    enum class LfoShape
    {
        Sine = 1,
        Triangle,
        Square,
        Saw,
        Random
    };

    struct LfoRoute
    {
        int  midiChannel = 0;      // 0 = disabled, 1..16 = enabled
        int  parameterIndex = -1;  // index into syntaktParameters
        bool bipolar = false;
        bool invertPhase = false;
        bool oneShot = false;

        // runtime state
        bool hasFinishedOneShot = false;
        bool passedPeak = false;

        double totalPhaseAdvanced = 0.0;  // ADD THIS
    };

    // Cache last route settings to detect changes (so we can reset oneshot/phase only when needed)
    struct RouteSnapshot
    {
        int  midiChannel = 0;
        int  paramIndex  = 0;
        bool bipolar     = false;
        bool invert      = false;
        bool oneShot     = false;
    };

    //==============================================================================
    inline bool advancePhase (double& phase, double inc) noexcept
    {
        phase += inc;
        bool wrapped = false;
        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            wrapped = true;
        }
        return wrapped;
    }

    // waveforms
    inline double lfoSine(double phase)
    {
        return std::sin(juce::MathConstants<double>::twoPi * phase);
    }

    inline double lfoTriangle(double phase)
    {
        // canonical triangle: 0 → +1 → 0 → -1 → 0
        double t = phase - std::floor(phase);
        return 4.0 * std::abs(t - 0.5) - 1.0;
    }

    inline double lfoSquare(double phase)
    {
        return (phase < 0.5) ? 1.0 : -1.0;
    }

    inline double lfoSaw(double phase)
    {
        // -1..+1 ramp
        return 2.0 * (phase - std::floor(phase)) - 1.0;
    }

        inline double lfoRandom (double phase, juce::Random& rng)
    {
        static double lastPhase = 0.0;
        static double lastValue = 0.0;

        if (phase < lastPhase)
            lastValue = rng.nextDouble() * 2.0 - 1.0;

        lastPhase = phase;
        return lastValue;
    }

    inline double computeWaveform(LfoShape shape,
                                  double phase,
                                  bool bipolar,
                                  bool invertPhase,
                                  juce::Random& rng)
    {
        // true phase inversion (180°)
        if (invertPhase && shape != LfoShape::Saw)
        {
            phase += 0.5;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (invertPhase && (shape == LfoShape::Saw))
        {
            phase = -phase;
            if (phase <= 1.0)
                phase += 1.0;
        }

        // phase alignment per shape
        if (shape == LfoShape::Triangle && !bipolar)
        {
            phase += 0.25;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (shape == LfoShape::Triangle && bipolar)
        {
            phase -= 0.25;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (shape == LfoShape::Saw && bipolar)
        {
            phase += 0.5;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        switch (shape)
        {
            case LfoShape::Sine:     return lfoSine(phase);
            case LfoShape::Triangle: return lfoTriangle(phase);
            case LfoShape::Square:   return lfoSquare(phase);
            case LfoShape::Saw:      return lfoSaw(phase);
            case LfoShape::Random:   return lfoRandom(phase, rng);
            default:                 return 0.0;
        }
    }

    inline double getWaveformStartPhase (LfoShape shape, bool isBipolar)
    {
        double phase = 0.0;

        if (!isBipolar)
        {
            switch (shape)
            {
                case LfoShape::Sine:     phase = 0.75; break; // -1
                case LfoShape::Triangle: phase = 0.25; break; // -1
                case LfoShape::Square:   phase = 0.5;  break; // -1
                case LfoShape::Saw:      phase = 0.0;  break; // -1
                default: break;
            }
        }

        return phase;
    }

    // BPM → Frequency Conversion
    inline double bpmToHz(double bpm, int syncDivisionId)
    {
        if (bpm <= 0.0)
            return 0.0;

        // Division multiplier relative to 1 beat = quarter note
        double multiplier = 1.0;

        switch (syncDivisionId)
        {
            case 1: multiplier = 0.25; break;  // whole note (4 beats per cycle)
            case 2: multiplier = 0.5;  break;  // half note
            case 3: multiplier = 1.0;  break;  // quarter note
            case 4: multiplier = 2.0;  break;  // eighth
            case 5: multiplier = 4.0;  break;  // sixteenth
            case 6: multiplier = 8.0;  break;  // thirty-second
            case 7: multiplier = 2.0 / 1.5; break;  // dotted ⅛ (triplet-based)
            case 8: multiplier = 4.0 / 1.5; break;  // dotted 1/16
            default: break;
        }

        // base beat frequency = beats per second
        const double beatsPerSecond = bpm / 60.0;

        // final LFO frequency in Hz
        return beatsPerSecond * multiplier;
    }

    inline double updateLfoRateFromBpm (double rateHz, double bpm, int syncDivisionId, bool syncEnabled)
    {
        if (syncEnabled && bpm > 0.0)
            rateHz = bpmToHz(bpm, syncDivisionId);

        return rateHz;
    }

    //==============================================================================

    template <size_t MaxRoutes>
    inline void syncRoutesFromApvts (juce::AudioProcessorValueTreeState& apvts,
                                    LfoShape currentShape,
                                    std::array<LfoRoute, MaxRoutes>& lfoRoutes,
                                    std::array<RouteSnapshot, MaxRoutes>& lastRouteSnapshot,
                                    std::array<double, MaxRoutes>& lfoPhase)
    {
        for (size_t i = 0; i < MaxRoutes; ++i)
        {
            const auto rs = juce::String((int)i);

            const int chChoice0 =
                (int) apvts.getRawParameterValue("route" + rs + "_channel")->load(); // 0=Disabled, 1..16=Ch1..16

            const int midiChannel = (chChoice0 == 0) ? 0 : chChoice0;

            const int paramIdx =
                (int) apvts.getRawParameterValue("route" + rs + "_param")->load();   // 0..N-1

            bool bipolar =
                apvts.getRawParameterValue("route" + rs + "_bipolar")->load() > 0.5f;

            bool invert =
                apvts.getRawParameterValue("route" + rs + "_invert")->load() > 0.5f;

            bool oneShot =
                apvts.getRawParameterValue("route" + rs + "_oneshot")->load() > 0.5f;

            // Engine constraint: Random ignores these (you can also enforce via UI)
            if (currentShape == LfoShape::Random)
            {
                bipolar = false;
                invert  = false;
            }

            // Detect changes (so we can reset runtime-only flags safely)
            const RouteSnapshot now { midiChannel, paramIdx, bipolar, invert, oneShot };
            const auto& prev = lastRouteSnapshot[i];

            const bool channelChanged = (now.midiChannel != prev.midiChannel);
            const bool paramChanged   = (now.paramIndex  != prev.paramIndex);
            const bool modeChanged    = (now.bipolar     != prev.bipolar) || (now.invert != prev.invert);
            const bool oneshotChanged = (now.oneShot     != prev.oneShot);

            // Apply to engine route
            auto& r = lfoRoutes[i];
            r.midiChannel     = now.midiChannel;   // 0 means disabled
            r.parameterIndex  = now.paramIndex;
            r.bipolar         = now.bipolar;
            r.invertPhase     = now.invert;

            // if oneshot is turned off, clear completion state so it can run again next time
            if (!now.oneShot)
                r.hasFinishedOneShot = false;
            r.oneShot = now.oneShot;

            // If route settings changed while running, it’s safe to reset runtime state.
            // This replaces your old “stop LFO during combobox interaction” trick.
            if (channelChanged || paramChanged || modeChanged || oneshotChanged)
            {
                r.hasFinishedOneShot = false;
                r.passedPeak = false;

                // Re-align phase when the *shape/mode* changes,
                // or when route is re-enabled.
                const bool routeBecameEnabled = (prev.midiChannel == 0 && now.midiChannel != 0);
                if (modeChanged || routeBecameEnabled)
                {
                    lfoPhase[i] = getWaveformStartPhase (currentShape, now.bipolar);
                }
            }

            lastRouteSnapshot[i] = now;
        }
    }

    template <size_t MaxRoutes>
    inline void applyLfoActiveState (bool shouldBeActive,
                                    LfoShape shape,
                                    bool& lfoActive,
                                    bool& lfoRuntimeMuted,
                                    std::array<LfoRoute, MaxRoutes>& lfoRoutes,
                                    std::array<double, MaxRoutes>& lfoPhase)
    {
        if (shouldBeActive == lfoActive)
            return;

        lfoActive = shouldBeActive;

        // When turning ON: reset phases + one-shot state
        if (lfoActive)
        {
            lfoRuntimeMuted = false;

            for (size_t i = 0; i < MaxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = getWaveformStartPhase(shape, route.bipolar);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
                route.totalPhaseAdvanced = 0.0;
            }
        }
        else
        {
            for (size_t i = 0; i < MaxRoutes; ++i)
            {
                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
        }
    }
} // namespace modztakt::lfo
