#pragma once
#include <JuceHeader.h>

#include <array>
#include <unordered_map>

#include "MidiInParse.h"
#include "SyntaktParameterTable.h"
#include "MidiInput.h"          // where MidiClock lives in your repo
#include "EnvelopeComponent.h"  // if you still use it for now

// Forward declare editor
class ModzTaktAudioProcessorEditor;

/**
    MIDI-only / MIDI-effect processor (plugin-first core).
    - acceptsMidi()  = true
    - producesMidi() = true
    - isMidiEffect() = true

    You can add audio later by:
      1) adding BusesProperties to the constructor,
      2) setting isMidiEffect() to false if you become an audio effect/synth,
      3) actually processing audio in processBlock().
*/
class ModzTaktAudioProcessor final : public juce::AudioProcessor,
                                     public MidiClockListener
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    inline ModzTaktAudioProcessor()
        // For pure MIDI-effect plugins, buses are typically omitted.
        // If you later want audio, change this to:
        // : juce::AudioProcessor (BusesProperties()
        //       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        //       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        : apvts (*this, nullptr, "PARAMS", createParameterLayout())
    {
        midiClock.setListener(this);
    }

    inline ~ModzTaktAudioProcessor() override = default;

    //==============================================================================
    inline void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        cachedSampleRate = (sampleRate > 0.0 ? sampleRate : 44100.0);
        cachedBlockSize  = samplesPerBlock;

        const bool isStandaloneWrapper =
                (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::WrapperType::wrapperType_Standalone);

        // If you later create an engine, prepare it here.
        // engine.prepare(cachedSampleRate, cachedBlockSize);

        // No MidiInput device open here: plugin-first (host/standalone wrapper routes MIDI to processBlock).
        // We keep MidiClockHandler for BPM estimation from incoming MIDI realtime messages.
        juce::ignoreUnused (isStandaloneWrapper);
    }

    inline void releaseResources() override {}

    inline bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        // MIDI-only plugin: ignore audio layouts
        juce::ignoreUnused (layouts);
        return true;
    }

    //==============================================================================
    inline void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;

        if (isMidiEffect())
            audio.clear();

        // Snapshot incoming MIDI before we append generated events
        const juce::MidiBuffer midiIn = midi;

        const double blockStartMs = timeMs;

        const double blockDurationMs = 1000.0 * (double) audio.getNumSamples()
                / juce::jmax (1.0, getSampleRate());

        currentBlockStartMs = blockStartMs;
        msPerSample = 1000.0 / juce::jmax (1.0, getSampleRate());

        // timeMs is advanced at the end of the block so we can compute per-sample timestamps
        // for throttling / scheduling when needed.

        // --- Read APVTS ---
        const auto shape = static_cast<LfoShape>(
            (int) apvts.getRawParameterValue("lfoShape")->load() + 1
        );

        const bool lfoActiveParam = apvts.getRawParameterValue("lfoActive")->load() > 0.5f;

        const double rateSliderValueHz = apvts.getRawParameterValue("lfoRateHz")->load();
        const double depthSliderValue  = apvts.getRawParameterValue("lfoDepth")->load();

        const int syncModeId   = ((int) apvts.getRawParameterValue("syncMode")->load()) + 1;
        const bool syncEnabled = (syncModeId == 2);

        const bool noteRestartToggleState = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
        const bool noteOffStopToggleState = apvts.getRawParameterValue("noteOffStop")->load() > 0.5f;
        const int  noteSourceChannel      = (int) apvts.getRawParameterValue("noteSourceChannel")->load();
        const int  syncDivisionId         = (int) apvts.getRawParameterValue("syncDivision")->load();

        // --- Parse incoming MIDI into pending flags (note on/off, requestLfoStop, etc.) ---
        parseIncomingMidiBuffer (midiIn,
                                 pending,
                                 syncEnabled,
                                 [this](const juce::MidiMessage& m)
                                 {
                                     // only realtime msgs are forwarded by MidiParse.h now
                                     midiClock.handleIncomingMidiMessage (nullptr, m);
                                 },
                                 noteRestartToggleState,
                                 noteOffStopToggleState);

        // --- Apply transport events (start/stop) ---
        applyPendingTransportEvents (shape);

        // --- Sync route params into engine state ---
        syncRoutesFromApvts (shape);

        // --- Apply LFO Active state (resets phases on edge) ---
        applyLfoActiveState (lfoActiveParam, shape);

        // 1) NOTE ON
        if (pending.pendingNoteOn.exchange(false, std::memory_order_acq_rel))
        {
            const int ch   = pending.pendingNoteChannel.load(std::memory_order_relaxed);
            const int note = pending.pendingNoteNumber.load (std::memory_order_relaxed);
            const float velocity = pending.pendingNoteVelocity.load (std::memory_order_relaxed);

            // --- EG ---
            if (envelopeComponent && envelopeComponent->isEgEnabled())
                envelopeComponent->noteOn (ch, note, velocity);

            // --- LFO Note Restart ---
            if (noteRestartToggleState && noteSourceChannel > 0 && ch == noteSourceChannel)
                requestLfoRestart.store (true, std::memory_order_release);

            // (UI debug label removed: cannot touch UI from audio thread)
        }

        // 2) NOTE OFF
        if (pending.pendingNoteOff.exchange(false, std::memory_order_acq_rel))
        {
            const int ch   = pending.pendingNoteChannel.load (std::memory_order_relaxed);
            const int note = pending.pendingNoteNumber.load (std::memory_order_relaxed);

            if (envelopeComponent && envelopeComponent->isEgEnabled())
                envelopeComponent->noteOff (ch, note);
        }

        // 3) Stop LFO on Note-Off (requested by callback logic)
        if (pending.requestLfoStop.exchange(false, std::memory_order_acq_rel))
        {
            lfoRuntimeMuted = true;

            // reset phases for next start
            for (int i = 0; i < maxRoutes; ++i)
            {
                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
        }

        // 4) LFO core variables (same as timerCallback)
        //const bool syncEnabled = (syncModeSelectedId == 2);
        const double bpm = midiClock.getCurrentBPM();

        // Handle pending retrigger from Note-On
        if (requestLfoRestart.exchange(false, std::memory_order_acq_rel))
        {
            lfoRuntimeMuted = false;

            // reset LFO value
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = getWaveformStartPhase(
                    shape,
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
                );

                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }

            if (!lfoActive)
            {
                lfoActive = true;
            }
        }

        // 5) LFO generation + send (writes into `midi`)
        if (lfoActive && !lfoRuntimeMuted)
        {
            double rateHz = rateSliderValueHz;

            if (syncEnabled && bpm > 0.0)
                rateHz = updateLfoRateFromBpm (rateHz, syncDivisionId, syncEnabled); // must be processor/engine method now

            // IMPORTANT: timerCallback used phaseInc = rateHz / sampleRate;
            // sampleRate in plugin is per-sample. We have numSamples in this block:
            const double phaseIncPerSample = rateHz / juce::jmax(1.0, getSampleRate());
            //const auto shapeId = static_cast<LfoShape>(shapeSelectedId);

            // ---- Sub-stepped LFO tick ----
            // In the old standalone UI, the LFO advanced once per timer tick.
            // In plugin context, blocks can be large (e.g. 256/512 samples), so a single
            // "once-per-block" update can sound/feel stepped at higher rates.
            // We therefore update the LFO multiple times within the block and emit MIDI
            // at the corresponding sample offsets.
            const int blockSize = audio.getNumSamples();

            // Choose a step size (in samples). Smaller step = smoother but more MIDI events.
            // This keeps modulation smooth without going per-sample.
            int stepSamples = (int) std::round (juce::jmax (1.0, getSampleRate()) / juce::jmax (0.001, rateHz) / 64.0);
            stepSamples = juce::jlimit (8, 128, stepSamples);
            stepSamples = juce::jlimit (1, juce::jmax (1, blockSize), stepSamples);

            for (int offset = 0; offset < blockSize; offset += stepSamples)
            {
                const int stepThis = juce::jmin (stepSamples, blockSize - offset);
                const double phaseIncThis = phaseIncPerSample * (double) stepThis;

                for (int i = 0; i < maxRoutes; ++i)
                {
                    auto& route = lfoRoutes[i];

                    if (route.midiChannel <= 0 || route.parameterIndex < 0)
                        continue;

                    if (route.oneShot && route.hasFinishedOneShot)
                        continue;

                    const bool wrapped = advancePhase (lfoPhase[i], phaseIncThis);
                    // ^^^^^ IMPORTANT CHANGE:
                    // timerCallback advanced once per timer tick; now we advance several times per block.
                    // We increment phase by (phaseIncPerSample * stepThis) so speed stays correct.

                    const double shapeComputed = computeWaveform (shape,
                                                          lfoPhase[i],
                                                          route.bipolar,
                                                          route.invertPhase,
                                                          random);

                    // One-shot logic (unchanged)
                    if (route.oneShot)
                    {
                        if (route.bipolar)
                        {
                            if (wrapped)
                                route.hasFinishedOneShot = true;
                        }
                        else
                        {
                            if (! route.passedPeak && shapeComputed >= 0.999)
                                route.passedPeak = true;

                            if (route.passedPeak && shapeComputed <= -0.999)
                                route.hasFinishedOneShot = true;
                        }
                    }

                    const auto& param = syntaktParameters[route.parameterIndex];
                    const double depth = depthSliderValue;

                    int midiVal = 0;

                    if (route.bipolar)
                    {
                        const int center = (param.minValue + param.maxValue) / 2;
                        const int range  = (param.maxValue - param.minValue) / 2;
                        midiVal = center + int (std::round (shapeComputed * depth * range));
                    }
                    else
                    {
                        const double uni = juce::jlimit (0.0, 1.0, (shapeComputed + 1.0) * 0.5);
                        midiVal = param.minValue + int (std::round (uni * depth * (param.maxValue - param.minValue)));
                    }

                    midiVal = juce::jlimit (param.minValue, param.maxValue, midiVal);

                    // Replace device send with MidiBuffer addEvent
                    sendThrottledParamValueToBuffer (midi, i, route.midiChannel, param, midiVal, offset);

                    // Scope tap
                    // (If you keep lfoRoutesToScope[], preserve it similarly in processor)
                    // lastLfoRoutesValues[i].store((float)(shape * depth), std::memory_order_relaxed);
                }
            }
        }

        // 6) EG tick / send (still uses EnvelopeComponent for now)
        if (envelopeComponent && envelopeComponent->isEgEnabled())
        {
            double egMIDIvalue = 0.0;

            if (envelopeComponent->tick (egMIDIvalue))
            {
                const int paramId = envelopeComponent->selectedEgOutParamsId();
                const int egValue = mapEgToMidi (egMIDIvalue, paramId);
                const int egCh    = envelopeComponent->selectedEgOutChannel();

                if (egCh > 0 && paramId >= 0)
                {
                    const auto& param = syntaktParameters[paramId];

                    sendThrottledParamValueToBuffer (midi,
                                                     0x7FFF,
                                                     egCh,
                                                     param,
                                                     egValue,
                                                     0);
                }
            }
        }

        // Advance global time after processing the block
        timeMs = blockStartMs + blockDurationMs;
    }

    //==============================================================================
    inline juce::AudioProcessorEditor* createEditor() override;
    inline bool hasEditor() const override { return true; }

    //==============================================================================
    inline const juce::String getName() const override { return JucePlugin_Name; }

    inline bool acceptsMidi() const override { return true; }
    inline bool producesMidi() const override { return true; }
    inline bool isMidiEffect() const override { return true; }

    inline double getTailLengthSeconds() const override { return 0.0; }

    //==============================================================================
    inline int getNumPrograms() override { return 1; }
    inline int getCurrentProgram() override { return 0; }
    inline void setCurrentProgram (int) override {}
    inline const juce::String getProgramName (int) override { return {}; }
    inline void changeProgramName (int, const juce::String&) override  {}

    //==============================================================================
    inline void getStateInformation (juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        copyXmlToBinary (*xml, destData);
    }

    inline void setStateInformation (const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

        if (xmlState != nullptr)
            if (xmlState->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
    }

    //==============================================================================
    inline APVTS& getAPVTS() noexcept { return apvts; }

    //==============================================================================
    // MidiClockListener
    inline void handleMidiStart() override { transportStartPending.store(true, std::memory_order_release); }
    inline void handleMidiStop() override  { transportStopPending.store(true, std::memory_order_release); }
    inline void handleMidiContinue() override {}

private:
    //==============================================================================
    // APVTS layout
    inline static APVTS::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

        // ... your existing parameter layout (not modified here) ...
        // NOTE: this file assumes you already have:
        //   - lfoActive (bool)
        //   - lfoRateHz (float)
        //   - lfoDepth (float)
        //   - lfoShape (choice)
        //   - syncMode (choice)
        //   - syncDivision (choice/int)
        //   - noteRestart (bool)
        //   - noteOffStop (bool)
        //   - noteSourceChannel (int/choice)
        //   - route{i}_* params (choices/bools)

        // Returning empty here would break your build; keep your actual layout.
        return { p.begin(), p.end() };
    }

    //==============================================================================
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
        int midiChannel = 0;      // 0 = disabled, 1..16 = enabled
        int parameterIndex = -1;  // index into syntaktParameters
        bool bipolar = false;
        bool invertPhase = false;
        bool oneShot = false;

        bool hasFinishedOneShot = false;
        bool passedPeak = false;
    };

    // Cache last route settings to detect changes (so we can reset oneshot/phase only when needed)
    struct RouteSnapshot
    {
        int midiChannel = 0;     // 0 disabled, 1..16 enabled
        int paramIndex  = 0;
        bool bipolar    = false;
        bool invert     = false;
        bool oneShot    = false;
    };

    //==============================================================================
    inline void applyPendingTransportEvents (LfoShape shape)
    {
        const bool gotStart = transportStartPending.exchange(false, std::memory_order_acq_rel);
        const bool gotStop  = transportStopPending.exchange(false,  std::memory_order_acq_rel);

        if (gotStart)
        {
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = getWaveformStartPhase(shape, route.bipolar, route.invertPhase);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
            }

            requestLfoRestart.store(true, std::memory_order_release);
        }

        if (gotStop)
        {
            // Optional: stop LFO on transport stop
            // lfoRuntimeMuted = true;
        }
    }

    inline void applyLfoActiveState (bool shouldBeActive, LfoShape shape)
    {
        if (shouldBeActive == lfoActive)
            return;

        lfoActive = shouldBeActive;

        // When turning ON: reset phases + one-shot state (old toggleLFO logic)
        if (lfoActive)
        {
            lfoRuntimeMuted = false;

            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = getWaveformStartPhase(shape, route.bipolar, route.invertPhase);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
            }
        }
        else
        {
            for (int i = 0; i < maxRoutes; ++i)
            {
                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
        }
    }

    inline void syncRoutesFromApvts (LfoShape currentShape)
    {
        for (int i = 0; i < maxRoutes; ++i)
        {
            const auto rs = juce::String(i);

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

                // Optional: re-align phase when the *shape/mode* changes,
                // or when route is re-enabled.
                const bool routeBecameEnabled = (prev.midiChannel == 0 && now.midiChannel != 0);
                if (modeChanged || routeBecameEnabled)
                {
                    lfoPhase[i] = getWaveformStartPhase (currentShape, now.bipolar, now.invert);
                }
            }

            lastRouteSnapshot[i] = now;
        }
    }

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

    inline double computeWaveform (LfoShape shape,
                                   double phase,
                                   bool bipolar,
                                   bool invertPhase,
                                   juce::Random& rng) const
    {
        // ... your existing waveform code (not modified here) ...
        // Placeholder:
        juce::ignoreUnused (bipolar, invertPhase);
        switch (shape)
        {
            case LfoShape::Sine:     return std::sin (juce::MathConstants<double>::twoPi * phase);
            default:                 return rng.nextDouble() * 2.0 - 1.0;
        }
    }

    inline double getWaveformStartPhase (LfoShape /*shape*/, bool /*bipolar*/, bool /*invert*/) const
    {
        // ... your existing logic ...
        return 0.0;
    }

    inline double updateLfoRateFromBpm (double rateHz, int /*syncDivisionId*/, bool /*syncEnabled*/) const
    {
        // ... your existing sync logic ...
        return rateHz;
    }

    inline int mapEgToMidi (double /*egNorm*/, int /*paramId*/) const
    {
        // ... your existing mapping ...
        return 0;
    }

    inline void sendThrottledParamValueToBuffer (juce::MidiBuffer& midiOut,
                                                 int routeIndex,
                                                 int midiChannel,
                                                 const SyntaktParameter& param,
                                                 int midiValue,
                                                 int sampleOffsetInBlock)
    {
        // Build per-route + per-parameter key
        const int paramKey =
            (routeIndex << 16) |
            (param.isCC ? 0x1000 : 0x2000) |
            (param.isCC ? param.ccNumber : ((param.nrpnMsb << 7) | param.nrpnLsb));

        const int lastVal = lastSentValuePerParam[paramKey];
        if (std::abs (midiValue - lastVal) < changeThreshold)
            return;

        lastSentValuePerParam[paramKey] = midiValue;

        const double now = currentBlockStartMs + (double) sampleOffsetInBlock * msPerSample;
        if (now - lastSendTimePerParam[paramKey] < msFloofThreshold)
            return;

        lastSendTimePerParam[paramKey] = now;

        if (param.isCC)
        {
            midiOut.addEvent (juce::MidiMessage::controllerEvent (midiChannel, param.ccNumber, midiValue),
                              sampleOffsetInBlock);
            return;
        }

        // NRPN: CC 99,98 then 6,38
        const int valueMSB = (midiValue >> 7) & 0x7F;
        const int valueLSB = midiValue & 0x7F;

        midiOut.addEvent (juce::MidiMessage::controllerEvent (midiChannel, 99, param.nrpnMsb), sampleOffsetInBlock);
        midiOut.addEvent (juce::MidiMessage::controllerEvent (midiChannel, 98, param.nrpnLsb), sampleOffsetInBlock);
        midiOut.addEvent (juce::MidiMessage::controllerEvent (midiChannel, 6,  valueMSB),   sampleOffsetInBlock);
        midiOut.addEvent (juce::MidiMessage::controllerEvent (midiChannel, 38, valueLSB),   sampleOffsetInBlock);
    }

private:
    //==============================================================================
    APVTS apvts;

    static constexpr int maxRoutes = 3;

    std::array<LfoRoute, maxRoutes> lfoRoutes {};
    std::array<RouteSnapshot, maxRoutes> lastRouteSnapshot {};
    std::array<double, maxRoutes> lfoPhase { { 0.0, 0.0, 0.0 } };

    bool lfoActive = false;
    bool lfoRuntimeMuted = false;

    juce::Random random;

    PendingMidiFlags pending;

    std::atomic<bool> requestLfoRestart { false };

    std::atomic<bool> transportStartPending { false };
    std::atomic<bool> transportStopPending  { false };

    MidiClockHandler midiClock;

    // Replace old device-send throttling with per-param throttling for buffer writes
    std::unordered_map<int, int>    lastSentValuePerParam;
    std::unordered_map<int, double> lastSendTimePerParam;

    int changeThreshold   = 1;
    double msFloofThreshold = 10.0;

    double cachedSampleRate = 44100.0;
    int cachedBlockSize     = 512;

    double timeMs = 0.0;

    // Helpers for per-sample scheduling (updated each processBlock)
    double currentBlockStartMs = 0.0;
    double msPerSample = 0.0;

    std::unique_ptr<EnvelopeComponent> envelopeComponent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModzTaktAudioProcessor)
};
