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
class ModzTaktAudioProcessor final : public juce::AudioProcessor
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
        // If you later create an engine, prepare it here.
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
        audio.clear();

        timeMs += 1000.0 * (double) audio.getNumSamples() / juce::jmax (1.0, getSampleRate());

        // -------------------------------------------------------------------------
        // TEMP: replace these with APVTS reads (next step)
        // -------------------------------------------------------------------------
        const bool noteRestartToggleState = true;   // TODO: apvts "noteRestart"
        const bool noteOffStopToggleState = true;   // TODO: apvts "noteOffStop"
        const int  restartCh              = 1;      // TODO: apvts "noteRestartCh" (0=off)
        const int  syncModeSelectedId     = 1;      // TODO: apvts "syncMode" (1=free,2=clock)
        const double rateSliderValueHz    = 1.0;    // TODO: apvts "rate"
        const double depthSliderValue     = 1.0;    // TODO: apvts "depth"
        const int shapeSelectedId         = 1;      // TODO: apvts "shape"
        const int divisionSelectedId      = 4;      // TODO: apvts "division"
        // -------------------------------------------------------------------------

        // Snapshot incoming MIDI before we append generated events.
        const juce::MidiBuffer midiIn = midi;

        // 0) Parse incoming MIDI -> set pending flags (same semantics as GlobalMidiCallback)
        parseIncomingMidiBuffer (midiIn,
                                 pending,
                                 syncModeSelectedId,
                                 [this](const juce::MidiMessage& m){ midiClock.handleIncomingMidiMessage (nullptr, m); },
                                 noteRestartToggleState,
                                 noteOffStopToggleState);

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
            if (noteRestartToggleState && restartCh > 0 && ch == restartCh)
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
            lfoActive = false;

            // reset phases for next start
            for (int i = 0; i < maxRoutes; ++i)
            {
                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }

            // (UI startButton text removed)
        }

        // 4) LFO core variables (same as timerCallback)
        const bool syncEnabled = (syncModeSelectedId == 2);
        const double bpm = midiClock.getCurrentBPM();

        // Handle pending retrigger from Note-On
        if (requestLfoRestart.exchange(false, std::memory_order_acq_rel))
        {
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = getWaveformStartPhase(
                    static_cast<LfoShape>(shapeSelectedId),
                    route.bipolar,
                    route.invertPhase);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
            }

            if (! lfoActive)
                lfoActive = true;
        }

        // (BPM label updates removed: UI must poll processor state instead)

        // 5) LFO generation + send (writes into `midi`)
        if (lfoActive)
        {
            double rateHz = rateSliderValueHz;

            if (syncEnabled && bpm > 0.0)
                rateHz = updateLfoRateFromBpm (rateHz, divisionSelectedId); // must be processor/engine method now

            // IMPORTANT: timerCallback used phaseInc = rateHz / sampleRate;
            // sampleRate in plugin is per-sample. We have numSamples in this block:
            const double phaseIncPerSample = rateHz / juce::jmax(1.0, getSampleRate());
            const auto shapeId = static_cast<LfoShape>(shapeSelectedId);

            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                if (route.midiChannel <= 0 || route.parameterIndex < 0)
                    continue;

                if (route.oneShot && route.hasFinishedOneShot)
                    continue;

                const bool wrapped = advancePhase (lfoPhase[i], phaseIncPerSample * audio.getNumSamples());
                // ^^^^^ IMPORTANT CHANGE:
                // timerCallback advanced once per timer tick; now we advance once per block.
                // Multiply by numSamples so speed stays correct.

                const double shape = computeWaveform (shapeId,
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
                        if (! route.passedPeak && shape >= 0.999)
                            route.passedPeak = true;

                        if (route.passedPeak && shape <= -0.999)
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
                    midiVal = center + int (std::round (shape * depth * range));
                }
                else
                {
                    const double uni = juce::jlimit (0.0, 1.0, (shape + 1.0) * 0.5);
                    midiVal = param.minValue + int (std::round (uni * depth * (param.maxValue - param.minValue)));
                }

                midiVal = juce::jlimit (param.minValue, param.maxValue, midiVal);

                // Replace device send with MidiBuffer addEvent
                sendThrottledParamValueToBuffer (midi, i, route.midiChannel, param, midiVal, 0);

                // Scope tap
                // (If you keep lfoRoutesToScope[], preserve it similarly in processor)
                // lastLfoRoutesValues[i].store((float)(shape * depth), std::memory_order_relaxed);
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

        // (Debug label update removed: UI must pull state from processor)
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
    inline const APVTS& getAPVTS() const noexcept { return apvts; }

    inline double getSampleRateCached() const noexcept { return cachedSampleRate; }
    inline int    getBlockSizeCached()  const noexcept { return cachedBlockSize; }

private:
    //==============================================================================
    inline static APVTS::ParameterLayout createParameterLayout()
    {
        // Start minimal. We'll add your real params next step
        // (LFO rate/depth/sync, EG params, route targets, etc).
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        // A tiny “heartbeat” param just to confirm APVTS wiring works.
        params.push_back (std::make_unique<juce::AudioParameterBool>(
            "enabled", "Enabled", true));

        return { params.begin(), params.end() };
    }

    //==============================================================================
    /** Migration state copied from MainComponent (temporary). */

    // Pending note flags (replaces GlobalMidiCallback storage)
    PendingMidiFlags pending;

    // MIDI clock (same class as you used in MainComponent)
    MidiClockHandler midiClock;

    bool useHostClock = true; // plugin hosted: true, standalone with separate clock device: false

    // LFO state
    static constexpr int maxRoutes = 3; // IMPORTANT: your MainComponent uses 3

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
        int midiChannel = 0;
        int parameterIndex = 0;
        bool bipolar = false;
        bool invertPhase = false;
        bool oneShot = false;

        // runtime state
        bool passedPeak = false;
        bool hasFinishedOneShot = false;
    };

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

    //==============================================================================
    // Helpers copied from MainComponent (minimal set)

    inline bool advancePhase (double& phase, double inc)
    {
        const double prev = phase;
        phase += inc;

        if (phase >= 1.0)
            phase -= std::floor(phase);  // handles inc > 1 safely

        return (prev > phase); // wrapped if we crossed 1.0
    }

    // waveforms
    inline double lfoSine (double phase)     { return std::sin(juce::MathConstants<double>::twoPi * phase); }
    inline double lfoTriangle (double phase) { double t = phase - std::floor(phase); return 4.0 * std::abs(t - 0.5) - 1.0; }
    inline double lfoSquare (double phase)   { return (phase < 0.5) ? 1.0 : -1.0; }
    inline double lfoSaw (double phase)      { return 2.0 * phase - 1.0; }

    inline double lfoRandom (double phase, juce::Random& rng)
    {
        // NOTE: this matches your existing logic, but static state means one random stream shared.
        // We'll refine later to be per-route.
        static double lastPhase = 0.0;
        static double lastValue = 0.0;

        if (phase < lastPhase)
            lastValue = rng.nextDouble() * 2.0 - 1.0;

        lastPhase = phase;
        return lastValue;
    }

    inline double computeWaveform (LfoShape shape,
                                   double phase,
                                   bool bipolar,
                                   bool invertPhase,
                                   juce::Random& rng)
    {
        // true phase inversion (180°)
        if (invertPhase && shape != LfoShape::Saw)
        {
            phase += 0.5;
            if (phase >= 1.0) phase -= 1.0;
        }

        if (invertPhase && (shape == LfoShape::Saw))
        {
            phase = -phase;
            if (phase <= 1.0) phase += 1.0;
        }

        // phase alignment per shape
        if (shape == LfoShape::Triangle && !bipolar)
        {
            phase += 0.25;
            if (phase >= 1.0) phase -= 1.0;
        }

        if (shape == LfoShape::Triangle && bipolar)
        {
            phase -= 0.25;
            if (phase >= 1.0) phase -= 1.0;
        }

        if (shape == LfoShape::Saw && bipolar)
        {
            phase += 0.5;
            if (phase >= 1.0) phase -= 1.0;
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

    inline double getWaveformStartPhase (LfoShape shape, bool bipolar, bool /*invert*/) const
    {
        double phase = 0.0;

        if (!bipolar)
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

    inline double bpmToHz (double bpm, int divisionSelectedId) const
    {
        if (bpm <= 0.0)
            return 0.0;

        double multiplier = 1.0;

        switch (divisionSelectedId)
        {
            case 1: multiplier = 0.25; break;       // whole
            case 2: multiplier = 0.5;  break;       // half
            case 3: multiplier = 1.0;  break;       // quarter
            case 4: multiplier = 2.0;  break;       // eighth
            case 5: multiplier = 4.0;  break;       // sixteenth
            case 6: multiplier = 8.0;  break;       // 32nd
            case 7: multiplier = 2.0 / 1.5; break;  // dotted 1/8
            case 8: multiplier = 4.0 / 1.5; break;  // dotted 1/16
            default: break;
        }

        return (bpm / 60.0) * multiplier;
    }

    double updateLfoRateFromBpm(double rateHz, int divisionID)
    {
        const double bpm = midiClock.getCurrentBPM();
        //const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        // if ( syncModeSelectedId == 2 && bpm > 0.0)
        // {
            rateHz = bpmToHz(bpm, divisionID);

            //TODO UPDATE ui
            //rateSlider.setValue(rateHz, juce::dontSendNotification);
        // }
            
        return rateHz;
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
    double cachedSampleRate = 44100.0;
    int    cachedBlockSize  = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModzTaktAudioProcessor)
};

// declare factory (defined in PluginEntry.cpp)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();