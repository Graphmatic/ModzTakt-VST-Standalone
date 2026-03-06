#pragma once
#include <JuceHeader.h>
#include <array>
#include <unordered_map>
#include <vector>
#include <cmath>

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
        std::array<int, maxDelayRoutes> routeChannels { 0, 0, 0 };

        // Semitone transpose applied to echoes per route. Range: -24 .. +24.
        std::array<int, maxDelayRoutes> routeTranspose { 0, 0, 0 };
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Internal scheduled event: one note-on + matching note-off on one channel.
    // ─────────────────────────────────────────────────────────────────────────
    struct ScheduledNote
    {
        int    originalNote = 60;  // raw input note (used for noteOff matching)
        int    note         = 60;  // transposed note sent over MIDI
        int    velocity     = 64;   // MIDI 1 – 127
        int    channel      = 1;    // MIDI channel 1 – 16
        double onTimeMs     = 0.0;  // absolute time to fire note-on
        double offTimeMs    = 0.0;  // absolute time to fire note-off (patchable)
        bool   noteOnFired  = false;
        bool   noteOffFired = false;
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

                // Recompute offTimeMs relative to this echo's own note-on time,
                // so each echo in the chain keeps the correct phasing.
                n.offTimeMs = n.onTimeMs + echoDurMs;
            }
        }

        // ── Called every processBlock to flush due events into the output buffer.
        //    Pass the same `midi` buffer that LFO / EG already write into.
        // ─────────────────────────────────────────────────────────────────────
        void processBlock (int               numSamples,
                           double            blockStartMs,
                           juce::MidiBuffer& midi)
        {
            if (!params.enabled)
            {
                scheduledNotes.clear();
                noteOnTimes.clear();
                return;
            }

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

        // ── Hard-clear all pending echoes (call from prepareToPlay) ──────────
        void reset()
        {
            scheduledNotes.clear();
            noteOnTimes.clear();
        }

    private:
        // Convert an absolute timestamp to a sample offset within the current block.
        static int msToSampleOffset (double eventMs, double blockStartMs,
                                     double msPerSample, int numSamples) noexcept
        {
            if (msPerSample <= 0.0)
                return 0;

            const int offset = static_cast<int> (
                std::round ((eventMs - blockStartMs) / msPerSample));

            return juce::jlimit (0, juce::jmax (0, numSamples - 1), offset);
        }

        // ─────────────────────────────────────────────────────────────────────
        Params params;

        // Upper bound prevents runaway memory growth with extreme settings.
        static constexpr std::size_t maxQueueSize = 128;
        std::vector<ScheduledNote> scheduledNotes;

        // Tracks note-start times so noteOff() can compute input note duration.
        // Key = MIDI note number (0–127), Value = blockStartMs at note-on.
        std::unordered_map<int, double> noteOnTimes;

        double sampleRate  = 48000.0;
        double msPerSample = 1000.0 / 48000.0;
    };

} // namespace modztakt::delay