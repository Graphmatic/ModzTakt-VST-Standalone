#pragma once
#include <JuceHeader.h>
#include <array>
#include <unordered_map>
#include <vector>
#include <cmath>

// The per-note EG embeds modztakt::eg::Engine — same params structure as the
// main EG, so the user drives both from one set of knobs.
#include "EnvelopeEngine.h"

namespace modztakt::delay
{
    static constexpr int maxDelayRoutes = 3;

    // ─────────────────────────────────────────────────────────────────────────
    // BPM sync helper
    //
    //   divisionId  1-based index matching the "delaySyncDivision" APVTS choice
    //               offset by 1  (choice index 0 = "Free" = not synced).
    //
    //   Division mapping (mirrors LFO syncDivision):
    //     1 = 1/1        4 beats
    //     2 = 1/2        2 beats
    //     3 = 1/4        1 beat   (quarter note)
    //     4 = 1/8        0.5 beats
    //     5 = 1/16       0.25 beats
    //     6 = 1/32       0.125 beats
    //     7 = 1/8 dot    0.75 beats
    //     8 = 1/16 dot   0.375 beats
    //
    //   Returns the delay interval in milliseconds, clamped to [10, 10000].
    // ─────────────────────────────────────────────────────────────────────────
    inline double divisionToMs (double bpm, int divisionId) noexcept
    {
        if (bpm <= 0.0) return 500.0;

        const double beatMs = 60000.0 / bpm; // duration of one quarter-note in ms

        double beats = 1.0;
        switch (divisionId)
        {
            case 1:  beats = 4.0;     break; // 1/1
            case 2:  beats = 2.0;     break; // 1/2
            case 3:  beats = 1.0;     break; // 1/4
            case 4:  beats = 0.5;     break; // 1/8
            case 5:  beats = 0.25;    break; // 1/16
            case 6:  beats = 0.125;   break; // 1/32
            case 7:  beats = 0.75;    break; // 1/8 dotted
            case 8:  beats = 0.375;   break; // 1/16 dotted
            default: beats = 1.0;     break;
        }

        return juce::jlimit (10.0, 10000.0, beatMs * beats);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Params – filled each processBlock() from APVTS, then handed to the engine.
    // ─────────────────────────────────────────────────────────────────────────
    struct Params
    {
        bool  enabled   = false;
        float delayTimeMs = 500.0f;   // milliseconds between echoes  (50 – 2000 ms)
        float feedback  = 0.5f;     // velocity decay factor per echo (0.0 – 0.95)

        // Output MIDI channels per route.
        // 0 = Disabled, 1-16 = MIDI channel number.
        std::array<int, maxDelayRoutes> routeChannels  { 0, 0, 0 };

        // Semitone transpose applied to echoes per route. Range: -24 .. +24.
        std::array<int, maxDelayRoutes> routeTranspose { 0, 0, 0 };

        // Per-note EG: when true each echo retriggers its own independent EG
        // using the parameters below (copied from the main EG APVTS params).
        bool perNoteEg = false;
        modztakt::eg::Params noteEgParams;

        // Step sequencer: when seqEnabled is true, each echo index is looked up
        // in seqSteps[echoIdx % stepCount] before being queued.  A false entry
        // silently skips that echo (no note-on / note-off scheduled).
        // stepCount is 6 when seqTernary is true, 8 otherwise.
        // All steps default to true so the sequencer is transparent when enabled
        // but no step has been manually muted.
        static constexpr int maxSteps = 8;
        bool seqEnabled  = false;
        bool seqTernary  = false;  // false = 8-step (binary), true = 6-step (ternary)
        std::array<bool, maxSteps> seqSteps { true, true, true, true,
                                              true, true, true, true };

        // Auto-pan: when panEnabled, a pan CC is injected before each echo note-on.
        // panWidth 0..1 maps to 0..63 deviation from bipolar CC centre (64).
        // Odd echoes pan left (64 - deviation), even echoes pan right (64 + deviation).
        bool  panEnabled = false;
        float panWidth   = 0.5f;                                      
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Per-note EG output — filled each processBlock() when perNoteEg is active.
    // Processor reads this immediately after processBlock() to send CC.
    // ─────────────────────────────────────────────────────────────────────────
    struct PerNoteEgOutput
    {
        // Max EG value (0..1) across all echoes currently sounding per channel.
        // Index 0 is unused; indices 1..16 = MIDI channels.
        std::array<float, 17> maxEg01 {};
        bool hasAnyValue = false;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Pre-note CC primer — returned by collectNoteOnPrefires() which the
    // processor must call BEFORE processBlock().
    //
    // For each echo note-on due this block the processor injects a CC at
    // sampleOffset carrying the initial EG value (0.0 = silence).  Because
    // JUCE MidiBuffer preserves insertion order for equal timestamps, the CC
    // lands before the note-on that processBlock() writes at the same offset,
    // so the synth's volume is already set before the note event arrives.
    // ─────────────────────────────────────────────────────────────────────────
    struct NoteOnPrefire
    {
        int   channel;       // MIDI channel 1–16
        int   sampleOffset;  // position within the current block
        float initialEg01;   // always 0.0f (attack always starts from silence)
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Internal scheduled event: one note-on + matching note-off on one channel.
    // ─────────────────────────────────────────────────────────────────────────
    struct ScheduledNote
    {
        int    originalNote = 60;  // raw input note (used for noteOff matching)
        int    note         = 60;  // transposed note sent over MIDI
        int    velocity     = 64;  // MIDI 1 – 127
        int    channel      = 1;   // MIDI channel 1 – 16
        double onTimeMs     = 0.0; // absolute time to fire note-on
        double offTimeMs    = 0.0; // absolute time to fire note-off (patchable)
        bool   noteOnFired  = false;
        bool   noteOffFired = false;
        int    echoIndex    = 0;   // 0-based echo number; used for L/R pan alternation

        // Per-note EG — one independent envelope engine per echo.
        // Only active when Params::perNoteEg is true.
        // EG params are copied from the main EG at the moment this echo fires,
        // so late-arriving echoes pick up any knob changes made between echoes.
        modztakt::eg::Engine noteEg;
        bool noteEgStarted = false; // true once noteEg.noteOn() has been called

        // Per-note EG release timing.
        //
        // egReleaseStartMs: absolute time at which to call noteEg.noteOff()
        //   so the EG enters its release stage *before* the MIDI note-off fires.
        //   Initialised tentatively at note-on (release ends at offTimeMs).
        //   Patched when the real note-off arrives to reproduce the original
        //   hold duration; offTimeMs is then extended by releaseMs so the
        //   MIDI note-off fires only after the release has fully completed.
        //
        // egNoteOffFired: prevents double-calling noteEg.noteOff().
        double egReleaseStartMs = -1.0;
        bool   egNoteOffFired   = false;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Engine
    // ─────────────────────────────────────────────────────────────────────────
    class Engine
    {
    public:

        // ── Called once from prepareToPlay ────────────────────────────────────
        void setSampleRate (double sr)
        {
            sampleRate  = juce::jmax (1.0, sr);
            msPerSample = 1000.0 / sampleRate;
        }

        // ── Called every processBlock to refresh params from APVTS ────────────
        void setParams (const Params& newParams)
        {
            params = newParams;
        }

        const Params& getParams() const noexcept { return params; }

        // ── Call when a note-on passes the source-channel filter. ─────────────
        //
        //    Stores the note-start time and pre-queues all feedback echoes with a
        //    *tentative* duration of 70 % of the delay interval.  If the real
        //    note-off arrives before that expires, noteOff() will patch the stored
        //    offTimeMs for every pending echo of this note.
        //
        //    blockStartMs : absolute time (ms) of the first sample in this block.
        //    note         : MIDI note number (0 – 127).
        //    vel01        : MIDI velocity normalised to 0.0 – 1.0.
        // ─────────────────────────────────────────────────────────────────────
        void noteOn (int note, float vel01, double blockStartMs)
        {
            if (!params.enabled)
                return;

            bool anyRoute = false;
            for (int r = 0; r < maxDelayRoutes; ++r)
                if (params.routeChannels[r] > 0) { anyRoute = true; break; }
            if (!anyRoute)
                return;

            // Record note-start time so noteOff() can compute actual duration.
            noteOnTimes[note] = blockStartMs;

            const double delayMs = juce::jmax (10.0, static_cast<double> (params.delayTimeMs));

            // Tentative echo duration: 70 % of delay interval (patched by noteOff if needed).
            const double tentativeDurMs = delayMs * 0.70;

            float echoVel = vel01;

            for (int echoIdx = 0; echoIdx < 32; ++echoIdx)
            {
                echoVel *= params.feedback;

                if (echoVel < (1.0f / 127.0f))
                    break;

                // Step sequencer gate: when enabled, look up whether this echo
                // index (mod stepCount) is active.
                // - Muted steps are skipped (no ScheduledNote created), BUT
                //   echoVel is NOT decayed again for a muted step — only the
                //   advance from the top of the loop counts.  This keeps the
                //   velocity chain consistent regardless of the mute pattern;
                //   otherwise dense muting would exhaust the budget too fast
                //   and cut off later active echoes prematurely.
                if (params.seqEnabled)
                {
                    const int stepCount = params.seqTernary ? 6 : Params::maxSteps;
                    // Shift by 1: echo 0 → button 1, echo (stepCount-1) → button 0.
                    // Button 0 therefore controls the first echo of the *next* loop
                    // iteration — aligning with bar-start when sync is active.
                    const int stepIdx = (echoIdx + 1) % stepCount;
                    if (!params.seqSteps[stepIdx])
                        continue;
                }

                const double onMs  = blockStartMs + static_cast<double> (echoIdx + 1) * delayMs;
                const double offMs = onMs + tentativeDurMs;
                const int    mVel  = juce::jlimit (1, 127,
                                         static_cast<int> (std::round (echoVel * 127.0f)));

                for (int r = 0; r < maxDelayRoutes; ++r)
                {
                    if (params.routeChannels[r] <= 0)
                        continue;

                    if (scheduledNotes.size() >= maxQueueSize)
                        return; // safety cap

                    const int transposedNote = juce::jlimit (0, 127,
                                                   note + params.routeTranspose[r]);

                    ScheduledNote n;
                    n.originalNote = note;           // raw note for noteOff matching
                    n.note         = transposedNote; // what actually gets sent
                    n.velocity     = mVel;
                    n.channel      = params.routeChannels[r];
                    n.onTimeMs     = onMs;
                    n.offTimeMs    = offMs;
                    n.noteOnFired  = false;
                    n.noteOffFired = false;
                    n.echoIndex    = echoIdx;

                    // Per-note EG: set a tentative release start so the release
                    // finishes exactly when the MIDI note-off fires.
                    // If a real note-off arrives later, both values are patched
                    // in noteOff() to reproduce the original hold duration.
                    if (params.perNoteEg)
                    {
                        const double releaseMs = egReleaseMs (params.noteEgParams);
                        n.egReleaseStartMs = juce::jmax (onMs, offMs - releaseMs);
                    }

                    scheduledNotes.push_back (n);
                }
            }
        }

        // ── Call when a note-off passes the source-channel filter. ────────────
        //
        //    Computes the actual input-note duration and patches the offTimeMs of
        //    every still-unfired echo for this note so that:
        //
        //        echo duration = min( inputDuration, 70 % of delayInterval )
        //
        //    Echoes that have already fired their note-on are updated too, meaning
        //    even a "currently sounding" first echo will be shortened if the
        //    original note was released early.
        //
        //    blockStartMs : approximate time (ms) of the note-off (block boundary).
        // ─────────────────────────────────────────────────────────────────────
        void noteOff (int note, double blockStartMs)
        {
            if (!params.enabled)
                return;

            auto it = noteOnTimes.find (note);
            if (it == noteOnTimes.end())
                return; // no matching note-on tracked

            const double inputDurMs = blockStartMs - it->second;
            noteOnTimes.erase (it);

            if (inputDurMs <= 0.0)
                return;

            const double delayMs        = juce::jmax (10.0, static_cast<double> (params.delayTimeMs));
            const double maxEchoDurMs   = delayMs * 0.70;
            const double echoDurMs      = juce::jmin (inputDurMs, maxEchoDurMs);

            // Patch all pending echoes for this note that haven't fired their note-off yet.
            // Match on originalNote (the raw input pitch) because n.note may be transposed.
            for (auto& n : scheduledNotes)
            {
                if (n.originalNote != note || n.noteOffFired)
                    continue;

                if (params.perNoteEg)
                {
                    // Reproduce the original hold duration on each echo:
                    //   egReleaseStartMs = echo note-on  +  min(inputDur, 70% delayInterval)
                    // Then extend offTimeMs so the MIDI note-off fires only AFTER the
                    // release stage has had time to complete fully.
                    const double holdDurMs = echoDurMs; // same cap as the non-EG path
                    n.egReleaseStartMs = n.onTimeMs + holdDurMs;
                    n.offTimeMs        = n.egReleaseStartMs + egReleaseMs (params.noteEgParams);
                }
                else
                {
                    // Recompute offTimeMs relative to this echo's own note-on time,
                    // so each echo in the chain keeps the correct phasing.
                    n.offTimeMs = n.onTimeMs + echoDurMs;
                }
            }
        }

        // ── Call this BEFORE processBlock when perNoteEg is active. ──────────
        //
        //    Scans scheduled notes and returns one NoteOnPrefire entry for every
        //    echo whose note-on will fire during [blockStartMs, blockEndMs).
        //    The processor uses these to inject an initial CC into the MIDI buffer
        //    BEFORE calling processBlock so the CC timestamp precedes the note-on.
        //
        //    out is appended to (not cleared) so the caller can accumulate across
        //    multiple calls if desired.
        // ─────────────────────────────────────────────────────────────────────
        void collectNoteOnPrefires (double                      blockStartMs,
                                    int                         numSamples,
                                    std::vector<NoteOnPrefire>& out) const
        {
            if (!params.perNoteEg)
                return;

            const double blockEndMs = blockStartMs
                                    + static_cast<double> (numSamples) * msPerSample;

            for (const auto& n : scheduledNotes)
            {
                if (n.noteOnFired || n.onTimeMs >= blockEndMs)
                    continue;

                out.push_back ({
                    n.channel,
                    msToSampleOffset (n.onTimeMs, blockStartMs, msPerSample, numSamples),
                    0.0f   // EG always starts from silence (reset() before noteOn())
                });
            }
        }

        // ── Pan CC primer — call BEFORE processBlock when panEnabled is true. ──
        //
        //    For each echo note-on due this block, appends one PanPrefire entry.
        //    The processor injects a pan CC at the given sample offset BEFORE
        //    calling processBlock(), so the synth's pan is set before the note arrives.
        //
        //    Odd echoes pan left  (CC centre − deviation).
        //    Even echoes pan right (CC centre + deviation).
        // ─────────────────────────────────────────────────────────────────────
        struct PanPrefire
        {
            int channel;
            int sampleOffset;
            int panCcValue;   // 0..127, bipolar centre = 64
        };

        void collectPanPrefires (double                   blockStartMs,
                                 int                      numSamples,
                                 std::vector<PanPrefire>& out) const
        {
            if (!params.panEnabled || params.panWidth <= 0.0f)
                return;

            const double blockEndMs  = blockStartMs
                                     + static_cast<double> (numSamples) * msPerSample;
            const int    deviation   = juce::roundToInt (params.panWidth * 63.0f);

            for (const auto& n : scheduledNotes)
            {
                if (n.noteOnFired || n.onTimeMs >= blockEndMs)
                    continue;

                // Even echo index → right (+deviation), odd → left (−deviation).
                const int pan = (n.echoIndex % 2 == 0)
                              ? juce::jlimit (0, 127, 64 + deviation)
                              : juce::jlimit (0, 127, 64 - deviation);

                out.push_back ({
                    n.channel,
                    msToSampleOffset (n.onTimeMs, blockStartMs, msPerSample, numSamples),
                    pan
                });
            }
        }

        // ── Called every processBlock to flush due events into the output buffer.
        //    Pass the same `midi` buffer that LFO / EG already write into.
        //
        //    When Params::perNoteEg is true, each echo retriggers its own embedded
        //    EG.  After this call, read getPerNoteEgOutput() to obtain the max EG
        //    value per MIDI channel (use it to send a volume CC from the processor).
        //
        //    IMPORTANT: when perNoteEg is true, call collectNoteOnPrefires() and
        //    inject the returned CCs into the same midi buffer BEFORE this call so
        //    the synth receives volume=0 before the incoming note event.
        // ─────────────────────────────────────────────────────────────────────
        void processBlock (int               numSamples,
                           double            blockStartMs,
                           juce::MidiBuffer& midi)
        {
            if (!params.enabled)
            {
                scheduledNotes.clear();
                noteOnTimes.clear();
                perNoteEgOutput = {};
                return;
            }

            // Reset per-note EG accumulator for this block.
            perNoteEgOutput = {};

            const double blockEndMs = blockStartMs
                                    + static_cast<double> (numSamples) * msPerSample;

            for (auto& n : scheduledNotes)
            {
                // ── Note-on ─────────────────────────────────────────────────
                if (!n.noteOnFired && n.onTimeMs < blockEndMs)
                {
                    const int offset = msToSampleOffset (n.onTimeMs, blockStartMs,
                                                         msPerSample, numSamples);
                    midi.addEvent (
                        juce::MidiMessage::noteOn (n.channel, n.note,
                                                   static_cast<juce::uint8> (n.velocity)),
                        offset);
                    n.noteOnFired = true;

                    // ── Start per-note EG ────────────────────────────────────
                    // EG params are captured NOW (block-boundary accuracy), so
                    // consecutive echoes each pick up the current knob settings.
                    if (params.perNoteEg)
                    {
                        n.noteEg.setSampleRate (sampleRate);
                        n.noteEg.setParams (params.noteEgParams);
                        n.noteEg.reset();
                        n.noteEg.noteOn (static_cast<float> (n.velocity) / 127.0f);
                        n.noteEgStarted = true;
                    }
                }

                // ── Note-off ────────────────────────────────────────────────
                if (n.noteOnFired && !n.noteOffFired && n.offTimeMs < blockEndMs)
                {
                    const int offset = msToSampleOffset (n.offTimeMs, blockStartMs,
                                                         msPerSample, numSamples);
                    midi.addEvent (
                        juce::MidiMessage::noteOff (n.channel, n.note),
                        offset);
                    n.noteOffFired = true;

                    // Fallback: if the real note-off never arrived (tentative duration
                    // expired) and we haven't triggered the EG release yet, do it now.
                    // In the normal perNoteEg path this has already been called earlier
                    // via egReleaseStartMs, so egNoteOffFired will be true here.
                    if (params.perNoteEg && n.noteEgStarted && !n.egNoteOffFired)
                    {
                        n.noteEg.noteOff();
                        n.egNoteOffFired = true;
                    }
                }
            }

            // ── Advance per-note EGs and accumulate max per channel ──────────
            // This is a separate second pass so all note-on / note-off events
            // for this block have already been applied before the EG advances.
            if (params.perNoteEg)
            {
                for (auto& n : scheduledNotes)
                {
                    if (!n.noteEgStarted || !n.noteOnFired || n.noteOffFired)
                        continue;

                    // ── Trigger EG release at the scheduled time ─────────────
                    // This fires BEFORE offTimeMs so the release stage plays out
                    // while the echo is still sounding; offTimeMs was extended
                    // by noteOff() (or set tentatively at noteOn()) to match.
                    if (!n.egNoteOffFired
                        && n.egReleaseStartMs >= 0.0
                        && n.egReleaseStartMs < blockEndMs)
                    {
                        n.noteEg.noteOff();
                        n.egNoteOffFired = true;
                    }

                    double eg01 = 0.0;
                    if (n.noteEg.processBlock (numSamples, eg01))
                    {
                        const auto ch = static_cast<std::size_t> (n.channel);
                        if (ch >= 1 && ch <= 16)
                        {
                            perNoteEgOutput.maxEg01[ch] =
                                juce::jmax (perNoteEgOutput.maxEg01[ch],
                                            static_cast<float> (eg01));
                            perNoteEgOutput.hasAnyValue = true;
                        }
                    }
                }
            }

            // Prune fully-dispatched events.
            scheduledNotes.erase (
                std::remove_if (scheduledNotes.begin(), scheduledNotes.end(),
                                [](const ScheduledNote& sn)
                                {
                                    return sn.noteOnFired && sn.noteOffFired;
                                }),
                scheduledNotes.end());
        }

        // ── Per-note EG output (valid after each processBlock call) ───────────
        const PerNoteEgOutput& getPerNoteEgOutput() const noexcept
        {
            return perNoteEgOutput;
        }

        // ── Query which MIDI channels have notes currently sounding ──────────
        //
        //    "Sounding" = note-on has been fired but note-off has not yet fired.
        //    Used by PluginProcessor to determine which channels to send
        //    EG-volume / EG-track-level CC to when delayEgShape is active.
        //
        //    out[1]..out[16] are set to true if that channel has at least one
        //    echo currently sounding.  out[0] is always left false.
        // ─────────────────────────────────────────────────────────────────────
        void getActiveSoundingChannels (std::array<bool, 17>& out) const noexcept
        {
            out.fill (false);
            for (const auto& n : scheduledNotes)
                if (n.noteOnFired && !n.noteOffFired)
                    out[static_cast<std::size_t> (n.channel)] = true;
        }

        // ── Hard-clear all pending echoes (call from prepareToPlay) ──────────
        void reset()
        {
            scheduledNotes.clear();
            noteOnTimes.clear();
            perNoteEgOutput = {};
        }

    private:
        // Convert an absolute timestamp to a sample offset within the current block.
        static int msToSampleOffset (double eventMs, double blockStartMs,
                                     double msPrSample, int numSamples) noexcept
        {
            if (msPrSample <= 0.0)
                return 0;

            const int offset = static_cast<int> (
                std::round ((eventMs - blockStartMs) / msPrSample));

            return juce::jlimit (0, juce::jmax (0, numSamples - 1), offset);
        }

        // Compute the EG release duration in milliseconds from a Params struct.
        // Mirrors EnvelopeEngine::releaseSliderToMs (seconds × 3 in long mode).
        static double egReleaseMs (const modztakt::eg::Params& p) noexcept
        {
            return p.releaseSeconds * (p.releaseLongMode ? 3.0 : 1.0) * 1000.0;
        }

        // ─────────────────────────────────────────────────────────────────────
        Params params;

        // Upper bound prevents runaway memory growth with extreme settings.
        static constexpr std::size_t maxQueueSize = 128;
        std::vector<ScheduledNote> scheduledNotes;

        // Tracks note-start times so noteOff() can compute input note duration.
        // Key = MIDI note number (0–127), Value = blockStartMs at note-on.
        std::unordered_map<int, double> noteOnTimes;

        // Per-note EG output — rebuilt each processBlock() when perNoteEg is active.
        PerNoteEgOutput perNoteEgOutput;

        double sampleRate  = 48000.0;
        // safe fallback
        double msPerSample = 1000.0 / 48000.0;
    };

} // namespace modztakt::delay