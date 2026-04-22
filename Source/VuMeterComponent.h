#pragma once

// ============================================================
//  VuMeterComponent.h
//  AVAILABILITY: Linux Standalone + MODZTAKT_OVERBRIDGE builds ONLY.
// ============================================================

#if JUCE_LINUX \
    && defined (JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone \
    && defined (MODZTAKT_OVERBRIDGE) && MODZTAKT_OVERBRIDGE

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>

// ============================================================
//  VuMeterLevelStore
// ============================================================
class VuMeterLevelStore
{
public:
    static constexpr int kMaxChannels = 20;

    VuMeterLevelStore()
    {
        for (auto& a : levels)
            a.store (0.0f, std::memory_order_relaxed);
    }

    // Called from OB USB thread — atomic peak-hold per channel
    void pushAudio (const float* const* samples,
                    int                 numCh,
                    int                 numFrames) noexcept
    {
        const int n = juce::jmin (numCh, kMaxChannels);
        for (int ch = 0; ch < n; ++ch)
        {
            float peak = 0.0f;
            const float* src = samples[ch];
            for (int i = 0; i < numFrames; ++i)
            {
                const float a = std::abs (src[i]);
                if (a > peak) peak = a;
            }
            float current = levels[ch].load (std::memory_order_relaxed);
            while (peak > current
                   && ! levels[ch].compare_exchange_weak (
                          current, peak,
                          std::memory_order_relaxed,
                          std::memory_order_relaxed))
            {}
        }
    }

    // Called from UI timer at 20 Hz — smooth decay
    void decayAll (float factor = 0.85f) noexcept
    {
        for (auto& a : levels)
            a.store (a.load (std::memory_order_relaxed) * factor,
                     std::memory_order_relaxed);
    }

    // Called from paint() on the message thread
    float getLevel (int ch) const noexcept
    {
        if (ch < 0 || ch >= kMaxChannels) return 0.0f;
        return levels[(size_t) ch].load (std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        for (auto& a : levels) a.store (0.0f, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, kMaxChannels> levels;
};


// ============================================================
//  VuMeterGeom — geometry constants
//  Used by both drawChannelVuMeter() and ChannelStrip to
//  position the label pivot next to the VU strip.
// ============================================================
namespace VuMeterGeom
{
    static constexpr int   kNumLeds  = 7;
    static constexpr int   kNumGreen = 5;   // LEDs 0..4
    static constexpr float kLedDiam  = 7.0f;
    static constexpr float kLedGap   = 3.0f;
    static constexpr int   kVuStripW = 10;  // px, width of one VU strip

    // Total height: 7×6 + 6×3 = 60 px
    static constexpr float kTotalH =
        (float)kNumLeds * kLedDiam + (float)(kNumLeds - 1) * kLedGap;

    // Amplitude thresholds per LED index (0 = bottom/quietest)
    // Approx: −44, −30, −20, −10, −4, −1, −0.3 dBFS
    static constexpr float kThresholds[kNumLeds] =
    { 0.006f, 0.032f, 0.100f, 0.316f, 0.630f, 0.891f, 0.970f };
}


// ============================================================
//  drawChannelVuMeter()
//
//  Draws 7 stacked round LEDs (5 green + 2 red/orange) for
//  one channel.  No transform must be active on 'g'.
//
//  stripX   — left edge of the 10px VU strip in component coords
//  areaTop  — top of the label/VU area (below the arm LED)
//  areaH    — available height for the meter column
//  level    — peak amplitude 0..1 from VuMeterLevelStore
// ============================================================
inline void drawChannelVuMeter (juce::Graphics& g,
                                float            stripX,
                                float            areaTop,
                                float            areaH,
                                float            level) noexcept
{
    // Pull constants from namespace explicitly — no 'using namespace'
    // so there is no ambiguity risk with other translation units.
    const int   nLeds  = VuMeterGeom::kNumLeds;
    const int   nGreen = VuMeterGeom::kNumGreen;
    const float diam   = VuMeterGeom::kLedDiam;
    const float gap    = VuMeterGeom::kLedGap;
    const float totalH = VuMeterGeom::kTotalH;
    const float stripW = (float) VuMeterGeom::kVuStripW;

    // ── Colours ───────────────────────────────────────────────
    // OFF colours use medium-dark tones clearly distinguishable
    // from the panel background (0xee191b1e).
    static const juce::Colour kGreenOn  (0xff22dd44);
    static const juce::Colour kGreenOff (0xff1a3d28);  // mid-dark green — always visible
    static const juce::Colour kOrangeOn (0xffff9900);
    static const juce::Colour kRedOn    (0xffff2211);
    static const juce::Colour kRedOff   (0xff3d1a1a);  // mid-dark red — always visible

    // ── Vertical centring within the available area ───────────
    const float startY = areaTop + (areaH - totalH) * 0.5f;
    const float cx     = stripX + stripW * 1.3f;

    // ── Draw LEDs top-to-bottom (index 0 = bottom = quietest) ─
    for (int row = 0; row < nLeds; ++row)
    {
        // Flip row index so row 0 draws at the top (hottest LED)
        const int   ledIdx = nLeds - 1 - row;
        const float y      = startY + (float) row * (diam + gap);

        const juce::Rectangle<float> r (cx - diam * 0.5f, y, diam, diam);

        const bool lit = (level >= VuMeterGeom::kThresholds[ledIdx]);

        if (ledIdx >= nGreen)
        {
            // ── Red / orange zone ─────────────────────────────
            const juce::Colour onCol = (ledIdx == nLeds - 1) ? kRedOn : kOrangeOn;

            if (lit)
            {
                g.setColour (onCol);
                g.fillEllipse (r);
                // Specular highlight
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.fillEllipse (juce::Rectangle<float> (
                    r.getX() + diam * 0.10f,
                    r.getY() + diam * 0.10f,
                    diam * 0.55f, diam * 0.38f));
            }
            else
            {
                g.setColour (kRedOff);
                g.fillEllipse (r);
            }
        }
        else
        {
            // ── Green zone ────────────────────────────────────
            if (lit)
            {
                g.setColour (kGreenOn);
                g.fillEllipse (r);
                // Specular highlight
                g.setColour (juce::Colours::white.withAlpha (0.30f));
                g.fillEllipse (juce::Rectangle<float> (
                    r.getX() + diam * 0.10f,
                    r.getY() + diam * 0.10f,
                    diam * 0.55f, diam * 0.38f));
            }
            else
            {
                g.setColour (kGreenOff);
                g.fillEllipse (r);
            }
        }
    }
}

#endif // JUCE_LINUX && JucePlugin_Build_Standalone && MODZTAKT_OVERBRIDGE