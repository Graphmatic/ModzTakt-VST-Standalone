#pragma once
#include <JuceHeader.h>

#include <array>
#include <unordered_map>

#include "MidiInParse.h"
#include "SyntaktParameterTable.h"
#include "MidiInput.h"
#include "EnvelopeComponent.h"
#include "LfoComponent.h"

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

    // LFO types live in LfoComponent.h
    using LfoShape = modztakt::lfo::LfoShape;
    using LfoRoute = modztakt::lfo::LfoRoute;
    using RouteSnapshot = modztakt::lfo::RouteSnapshot;

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

    useHostClock = !isStandaloneWrapper; // plugin = host clock, standalone = device clock
        // If audio added:
        // engine.prepare(cachedSampleRate, cachedBlockSize);
    }

    inline void releaseResources() override {}

    inline bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        // MIDI-only: accept anything the host provides (often "no buses").
        // If you later add audio buses, validate layouts here.
        juce::ignoreUnused (layouts);
        return true;
    }

    inline void processBlock (juce::AudioBuffer<float>& audio,
                                                  juce::MidiBuffer& midi)
    {
        juce::ScopedNoDenormals noDenormals;
        if (isMidiEffect())
            audio.clear();

        // Snapshot incoming MIDI before we append generated events
        const juce::MidiBuffer midiIn = midi;

        midi.clear();

        // pass through everything EXCEPT notes
        for (const auto meta : midiIn)
        {
            const auto msg = meta.getMessage();
            if (! msg.isNoteOnOrOff())
                midi.addEvent(msg, meta.samplePosition);
        }

        const double blockStartMs = timeMs;

        const double blockDurationMs = 1000.0 * (double) audio.getNumSamples()
                / juce::jmax (1.0, getSampleRate());

        currentBlockStartMs = blockStartMs;
        msPerSample = 1000.0 / juce::jmax (1.0, getSampleRate());

        // timeMs is advanced at the end of the block so we can compute per-sample timestamps
        // for throttling / scheduling when needed.

        const bool lfoActiveParam = apvts.getRawParameterValue("lfoActive")->load() > 0.5f;
        //xx const int shapeSelectedId = (int) apvts.getRawParameterValue("lfoShape")->load() + 1; // back to your old 1..5 if you still use that

        const auto shape = static_cast<LfoShape>( (int) apvts.getRawParameterValue("lfoShape")->load() + 1 );

        modztakt::lfo::syncRoutesFromApvts<maxRoutes> (apvts, shape, lfoRoutes, lastRouteSnapshot, lfoPhase);

        // Detect user toggling OFF (explicit stop) and clear forced-run
        if (lastLfoActiveParam && !lfoActiveParam)
        {
            lfoForcedActiveByNote = false;
            lfoRuntimeMuted = false;
        }
        lastLfoActiveParam = lfoActiveParam;

        // Apply LFO Active state (resets phases)
        const bool shouldRunLfo = (lfoActiveParam || lfoForcedActiveByNote);

        modztakt::lfo::applyLfoActiveState (shouldRunLfo,
                                   shape,
                                   lfoActive,
                                   lfoRuntimeMuted,
                                   lfoRoutes,
                                   lfoPhase);

        const double rateSliderValueHz = apvts.getRawParameterValue("lfoRateHz")->load();
        const double depthSliderValue  = apvts.getRawParameterValue("lfoDepth")->load();

        const int syncModeId = ((int) apvts.getRawParameterValue("syncMode")->load()) + 1;
        const bool syncEnabled = (syncModeId == 2);

        const bool noteRestartToggleState = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
        if (!noteRestartToggleState)
            lfoForcedActiveByNote = false;

        const bool noteOffStopToggleState = apvts.getRawParameterValue("noteOffStop")->load() > 0.5f;
        const int noteSourceChannel = (int) apvts.getRawParameterValue("noteSourceChannel")->load();
        const int syncDivisionId = (int) apvts.getRawParameterValue("syncDivision")->load();
        // -------------------------------------------------------------------------

        // LFO Routes
        for (int i = 0; i < maxRoutes; ++i)
        {
            const auto rs = juce::String(i);

            const int chChoice0 = (int) apvts.getRawParameterValue("route" + rs + "_channel")->load(); // 0=Disabled, 1..16=Ch1..16
            const int midiChannel = (chChoice0 == 0) ? 0 : chChoice0;

            const int paramIdx = (int) apvts.getRawParameterValue("route" + rs + "_param")->load(); // 0..N-1

            bool bipolar = apvts.getRawParameterValue("route" + rs + "_bipolar")->load() > 0.5f;
            bool invert  = apvts.getRawParameterValue("route" + rs + "_invert")->load()  > 0.5f;
            bool oneShot = apvts.getRawParameterValue("route" + rs + "_oneshot")->load() > 0.5f;

            // if Random shape, force these off
            if (shape == LfoShape::Random)
            {
                bipolar = false;
                invert  = false;
            }

            // Apply to engine routes
            auto& r = lfoRoutes[i];
            r.midiChannel = midiChannel;
            r.parameterIndex = paramIdx;
            r.bipolar = bipolar;
            r.invertPhase = invert;

            // if oneshot is turned off, clear completion state
            if (!oneShot)
                r.hasFinishedOneShot = false;
            r.oneShot = oneShot;
        }

        // 0) Parse incoming MIDI -> set pending flags (same semantics as GlobalMidiCallback)
        parseIncomingMidiBuffer (midiIn,
                                 pending,
                                 syncEnabled,
                                 [this](const juce::MidiMessage& m){ midiClock.handleIncomingMidiMessage (nullptr, m); },
                                 noteRestartToggleState,
                                 noteOffStopToggleState);

        applyPendingTransportEvents(shape);

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
            if (noteRestartToggleState)
            {
                const bool matchesSource =
                    (noteSourceChannel <= 0) ? true : (ch == noteSourceChannel);

                if (matchesSource)
                {
                    if (!lfoActiveParam)
                        uiRequestSetLfoActiveOn.store(true, std::memory_order_release);

                    lfoForcedActiveByNote = true;  // <--- NEW
                    requestLfoRestart.store (true, std::memory_order_release);
                    for (int i = 0; i < maxRoutes; ++i)
                    {
                        lfoRoutes[i].totalPhaseAdvanced = 0.0;
                    }
                }
            }
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
            lfoForcedActiveByNote = false;   // <--- NEW (so it truly stops)

            for (int i = 0; i < maxRoutes; ++i)
            {
                lfoRoutes[i].totalPhaseAdvanced = 0.0;
                lfoRoutes[i].hasFinishedOneShot = true;
                lfoRoutes[i].passedPeak = true;
            }
        }

        // 4) LFO core variables (same as timerCallback)
        const double bpm = midiClock.getCurrentBPM();

        // Handle pending retrigger from Note-On
        if (requestLfoRestart.exchange(false, std::memory_order_acq_rel))
        {
            lfoRuntimeMuted = false;

            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = modztakt::lfo::getWaveformStartPhase(shape, route.bipolar);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
                route.totalPhaseAdvanced = 0.0;
            }

            if (! lfoActive)
                lfoActive = true;
        }

        // LFO generation + send (writes into `midi`)
        if (lfoActive && !lfoRuntimeMuted)
        {
            double rateHz = rateSliderValueHz;

            if (syncEnabled && bpm > 0.0)
                rateHz = modztakt::lfo::updateLfoRateFromBpm (rateHz, bpm, syncDivisionId, syncEnabled); // must be processor/engine method now

            // IMPORTANT: timerCallback used phaseInc = rateHz / sampleRate;
            // sampleRate in plugin is per-sample. We have numSamples in this block:
            const double phaseIncPerSample = rateHz / juce::jmax(1.0, getSampleRate());

            // ---- Sub-stepped LFO tick ----
            // In the old standalone UI, the LFO advanced once per timer tick.
            // In plugin context, blocks can be large (e.g. 256/512 samples), so a single
            // "once-per-block" update can sound/feel stepped at higher rates.
            // We therefore update the LFO multiple times within the block and emit MIDI
            // at the corresponding sample offsets.
            const int blockSize = audio.getNumSamples();

            // Choose a step size (in samples). Smaller step = smoother but more MIDI events.
            // This keeps modulation smooth without going per-sample.
            int stepSamples = (int) std::round (juce::jmax (1.0, getSampleRate()) / juce::jmax (0.001, rateHz) / 128.0); //edit last var: 64=stepper / 256=smoother
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

                    // Accumulate before advancing
                    route.totalPhaseAdvanced += phaseIncThis;
                    
                    const bool wrapped = modztakt::lfo::advancePhase (lfoPhase[i], phaseIncThis);
                    
                    const double shapeComputed = modztakt::lfo::computeWaveform (shape,
                                                          lfoPhase[i],
                                                          route.bipolar,
                                                          route.invertPhase,
                                                          random);

                    // One-shot: complete after full cycle
                    if (route.oneShot && route.totalPhaseAdvanced >= 1.0)
                    {
                        route.hasFinishedOneShot = true;
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

            // AUTO-STOP AFTER ONE-SHOT
            {
                bool anyEnabledRoute = false;
                bool anyRouteStillRunning = false;

                for (int i = 0; i < maxRoutes; ++i)
                {
                    const auto& r = lfoRoutes[i];

                    if (r.midiChannel <= 0 || r.parameterIndex < 0)
                        continue;

                    anyEnabledRoute = true;

                    // If any enabled route is NOT one-shot, LFO should keep running
                    if (!r.oneShot)
                    {
                        anyRouteStillRunning = true;
                        break;
                    }

                    // Enabled + one-shot: still running until finished
                    if (!r.hasFinishedOneShot)
                    {
                        anyRouteStillRunning = true;
                        break;
                    }
                }

                // Only auto-stop if we had something enabled and all enabled routes were one-shot and finished
                if (anyEnabledRoute && !anyRouteStillRunning)
                {
                    // stop producing values
                    lfoRuntimeMuted = true;
                    lfoForcedActiveByNote = false;

                    // Request UI to set the parameter OFF so button syncs visually
                    uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
                }
            }
        }

        // 6) EG tick + send (writes into `midi`)
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
    juce::AudioProcessorEditor* createEditor() override; // NOT inline here

    inline bool hasEditor() const override { return true; }

    //==============================================================================
    inline const juce::String getName() const override { return "ModzTakt"; }

    inline bool acceptsMidi()  const override { return true; }
    inline bool producesMidi() const override { return true; }
    inline bool isMidiEffect() const override { return true; }

    inline double getTailLengthSeconds() const override { return 0.0; }

    //==============================================================================
    inline int getNumPrograms() override                               { return 1; }
    inline int getCurrentProgram() override                            { return 0; }
    inline void setCurrentProgram (int) override                       {}
    inline const juce::String getProgramName (int) override            { return {}; }
    inline void changeProgramName (int, const juce::String&) override  {}

    //==============================================================================
    inline void getStateInformation (juce::MemoryBlock& destData) override
    {
        // Save APVTS state
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, destData);
    }

    inline void setStateInformation (const void* data, int sizeInBytes) override
    {
        // Restore APVTS state
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    //==============================================================================
    inline APVTS&       getAPVTS()       noexcept { return apvts; }

    inline double getSampleRateCached() const noexcept { return cachedSampleRate; }
    inline int    getBlockSizeCached()  const noexcept { return cachedBlockSize; }

    // LFO Start/Stop label refresh
    bool isLfoRunningForUi() const noexcept
    {
        return uiLfoIsRunning.load(std::memory_order_acquire);
    }

    std::atomic<bool> uiRequestSetLfoActiveOn { false };
    std::atomic<bool> uiRequestSetLfoActiveOff { false };

private:

    // MIDI clock/transport
    std::atomic<bool> transportStartPending { false };
    std::atomic<bool> transportStopPending  { false };

    //==============================================================================
    inline static APVTS::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

        // Main switches
        p.push_back (std::make_unique<juce::AudioParameterBool>("enabled", "Enabled", true));
        p.push_back (std::make_unique<juce::AudioParameterBool>("lfoActive", "LFO Active", false));

        // LFO core
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "lfoShape", "LFO Shape",
            juce::StringArray{ "Sine", "Triangle", "Square", "Saw", "Random" }, 0)); // 0..4

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "lfoRateHz", "LFO Rate",
            juce::NormalisableRange<float>(0.01f, 40.0f, 0.0f, 0.5f), 1.0f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "lfoDepth", "LFO Depth",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0f, 1.0f), 1.0f));

        // Sync mode: 0=Free, 1=MIDI Clock
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "syncMode", "Sync Mode",
            juce::StringArray{ "Free", "MIDI Clock" }, 0));  

        p.push_back (std::make_unique<juce::AudioParameterChoice>( // syncDivision
            "syncDivision", "Sync Division",
            juce::StringArray{ "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8 dotted", "1/16 dotted" }, 0));

        // Note restart feature
        p.push_back (std::make_unique<juce::AudioParameterBool>("noteRestart", "Note Restart", false));
        p.push_back (std::make_unique<juce::AudioParameterInt>("noteSourceChannel", "Note Restart Channel", 0, 16, 0));
        p.push_back (std::make_unique<juce::AudioParameterBool>("noteOffStop", "Stop on Note Off", false));

        //LFO routes
        // Build the syntakt parameter name list for combo boxes
        juce::StringArray syntaktParamNames;
        for (int i = 0; i < (int) juce::numElementsInArray(syntaktParameters); ++i)
            syntaktParamNames.add (syntaktParameters[i].name);

        // Route channel choices
        auto makeChannelChoices = []()
        {
            juce::StringArray s;
            s.add ("Disabled");
            for (int ch = 1; ch <= 16; ++ch)
                s.add ("Ch " + juce::String(ch));
            return s;
        };

        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);

            // channel: 0=Disabled, 1..16=Ch 1..16
            p.push_back (std::make_unique<juce::AudioParameterChoice>(
                "route" + rs + "_channel",
                "Route " + rs + " Channel",
                makeChannelChoices(),
                (r == 0 ? 1 : 0) // default: route0=Ch1, others Disabled
            ));

            // parameter index: 0..N-1
            p.push_back (std::make_unique<juce::AudioParameterChoice>(
                "route" + rs + "_param",
                "Route " + rs + " Parameter",
                syntaktParamNames,
                0 // default first param
            ));

            p.push_back (std::make_unique<juce::AudioParameterBool>(
                "route" + rs + "_bipolar",
                "Route " + rs + " Bipolar",
                false
            ));

            p.push_back (std::make_unique<juce::AudioParameterBool>(
                "route" + rs + "_invert",
                "Route " + rs + " Invert",
                false
            ));

            p.push_back (std::make_unique<juce::AudioParameterBool>(
                "route" + rs + "_oneshot",
                "Route " + rs + " OneShot",
                false
            ));
        }

    return { p.begin(), p.end() };
    }

    //==============================================================================

    // Pending note flags (replaces GlobalMidiCallback storage)
    PendingMidiFlags pending;

    // MIDI clock (same class as you used in MainComponent)
    MidiClockHandler midiClock;

    bool useHostClock = true; // plugin hosted: true, standalone with separate clock device: false

    // LFO state
    static constexpr int maxRoutes = 3; // IMPORTANT: your MainComponent uses 3

    std::array<RouteSnapshot, maxRoutes> lastRouteSnapshot {};

    std::array<LfoRoute, maxRoutes> lfoRoutes {};
    std::array<double,   maxRoutes> lfoPhase  {};

    juce::Random random;

    std::atomic<bool> requestLfoRestart { false };
    bool lfoActive = false;

    // EG (temporary: only if EnvelopeComponent is engine-like in your codebase)
    std::unique_ptr<EnvelopeComponent> envelopeComponent;

    // Throttle state for outgoing MIDI
    std::unordered_map<int, int>    lastSentValuePerParam;
    std::unordered_map<int, double> lastSendTimePerParam;

    int    changeThreshold  = 1;
    double msFloofThreshold = 10.0;
    double timeMs = 0.0;


    inline void handleMidiStart() override
    {
        transportStartPending.store(true, std::memory_order_release);
    }

    inline void handleMidiStop() override
    {
        transportStopPending.store(true, std::memory_order_release);
    }

    inline void handleMidiContinue() override {}

    //==============================================================================
    // Helpers

    inline void applyPendingTransportEvents (LfoShape shape)
    {
        const bool gotStart = transportStartPending.exchange(false, std::memory_order_acq_rel);
        const bool gotStop  = transportStopPending.exchange(false,  std::memory_order_acq_rel);

        if (!gotStart && !gotStop)
            return;

        // Resetting phases + one-shot runtime flags.
        for (int i = 0; i < maxRoutes; ++i)
        {
            auto& route = lfoRoutes[i];

            lfoPhase[i] = modztakt::lfo::getWaveformStartPhase(shape, route.bipolar);

            route.hasFinishedOneShot = false;
            route.passedPeak = false;
            route.totalPhaseAdvanced = 0.0;
        }

        requestLfoRestart.store(true, std::memory_order_release);

        if (gotStop)
        {
            // Stop LFO on transport stop
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
                route.totalPhaseAdvanced = 0.0;
            }
            lfoRuntimeMuted = true;
        }
    }

    inline int mapEgToMidi (double egValue01, int paramId) const
    {
        const auto& param = syntaktParameters[paramId];

        if (param.isBipolar)
        {
            const double center = (param.minValue + param.maxValue) * 0.5;
            const double range  = (param.maxValue - param.minValue) * 0.5;
            return (int) (center + (egValue01 * 2.0 - 1.0) * range);
        }

        return (int) (param.minValue + egValue01 * (param.maxValue - param.minValue));
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

        const double now = timeMs;
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

public:
    APVTS apvts;

private:
    double cachedSampleRate = 48000.0;
    int    cachedBlockSize  = 0;

    // Helpers for per-sample scheduling (updated each processBlock)timeMs
    double currentBlockStartMs = 0.0;
    double msPerSample = 0.0;

    bool lfoRuntimeMuted = false;

    bool lfoForcedActiveByNote = false;

    bool lastLfoActiveParam = false;

    std::atomic<bool> uiLfoIsRunning { false };


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModzTaktAudioProcessor)
};

// declare factory (defined in PluginEntry.cpp)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();