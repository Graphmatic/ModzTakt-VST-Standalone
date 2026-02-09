#pragma once
#include <JuceHeader.h>

#include <array>
#include <unordered_map>

#include "MidiInParse.h"
#include "SyntaktParameterTable.h"
#include "MidiInput.h"
#include "EnvelopeEngine.h"
#include "LfoEngine.h"

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
        cachedSampleRate = (sampleRate > 0.0 ? sampleRate : 48000.0);
        cachedBlockSize  = samplesPerBlock;

        // EG
        egEngine.setSampleRate(cachedSampleRate);
        egEngine.reset();

        // MIDI Out throttles/perf - Initialize from parameters
        if (auto* throttleParam = apvts.getParameter("midiDataThrottle"))
        {
            const int index = static_cast<int>(throttleParam->getValue() * 4.0f + 0.5f);
            changeThreshold.store(getChangeThresholdFromIndex(index), std::memory_order_relaxed);
        }

        if (auto* limiterParam = apvts.getParameter("midiRateLimiter"))
        {
            const int index = static_cast<int>(limiterParam->getValue() * 6.0f + 0.5f);
            msFloofThreshold.store(getMsFloofThresholdFromIndex(index), std::memory_order_relaxed);
        }

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

        const double blockDurationMs = 1000.0 * (double) audio.getNumSamples() / juce::jmax (1.0, getSampleRate());

        currentBlockStartMs = blockStartMs;
        msPerSample = 1000.0 / juce::jmax (1.0, getSampleRate());

        // timeMs is advanced at the end of the block so we can compute per-sample timestamps
        // for throttling / scheduling when needed.

        const bool lfoActiveParam = apvts.getRawParameterValue("lfoActive")->load() > 0.5f;

        const auto shape = static_cast<LfoShape>( (int) apvts.getRawParameterValue("lfoShape")->load() + 1 );

        modztakt::lfo::syncRoutesFromApvts<maxRoutes> (apvts, shape, lfoRoutes, lastRouteSnapshot, lfoPhase);

        const int syncModeId = ((int) apvts.getRawParameterValue("syncMode")->load()) + 1;
        const bool syncEnabled = (syncModeId == 2);

        const bool startOnPlayToggleState = apvts.getRawParameterValue("playStart")->load() > 0.5f;
        startOnPlay.store(startOnPlayToggleState, std::memory_order_release);

        if (!startOnPlayToggleState)
            lfoForcedActiveByPlay = false;

        // Detect user toggling OFF (explicit stop) and clear forced-run (Option A master kill)
        if (lastLfoActiveParam && !lfoActiveParam)
        {
            lfoForcedActiveByNote = false;
            lfoForcedActiveByPlay = false;

            lfoForcedActiveByEg = false;
            lfoForcedEgRouteIndex = -1;

            lfoRuntimeMuted = false;

            // Also clear EG gate/ramp state so we don't "finish" after a hard stop
            for (int i = 0; i < maxRoutes; ++i)
            {
                egGateWasOpen[i] = false;
                neutralRampActive[i] = false;
                neutralRampPos[i] = 0;
            }
        }
        lastLfoActiveParam = lfoActiveParam;


        const double rateSliderValueHz = apvts.getRawParameterValue("lfoRateHz")->load();
        const double depthSliderValue  = apvts.getRawParameterValue("lfoDepth")->load();

        const bool noteRestartToggleState = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
        if (!noteRestartToggleState)
            lfoForcedActiveByNote = false;

        const bool noteOffStopToggleState = apvts.getRawParameterValue("noteOffStop")->load() > 0.5f;
        const int noteSourceChannel = (int) apvts.getRawParameterValue("noteSourceChannel")->load();
        const int syncDivisionId = (int) apvts.getRawParameterValue("syncDivision")->load() + 1;
        // -------------------------------------------------------------------------

        // LFO Routes
        for (int i = 0; i < maxRoutes; ++i)
        {
            const auto rs = juce::String(i);

            // CLEANED: Removed redundant ternary - chChoice0 is already the channel value we need
            const int midiChannel = (int) apvts.getRawParameterValue("route" + rs + "_channel")->load();

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

        // EG
        const int egSourceChannel = (int) apvts.getRawParameterValue("egNoteSourceChannel")->load(); // 1..16=Ch1..16

        modztakt::eg::Params egParams;
        egParams.enabled          = apvts.getRawParameterValue("egEnabled")->load() > 0.5f;
        egIsEnabled.store((bool) egParams.enabled, std::memory_order_release);

        egParams.attackSeconds    = apvts.getRawParameterValue("egAttackSec")->load();
        egParams.holdSeconds      = apvts.getRawParameterValue("egHoldSec")->load();
        egParams.decaySeconds     = apvts.getRawParameterValue("egDecaySec")->load();
        egParams.sustain01        = apvts.getRawParameterValue("egSustain")->load();
        egParams.releaseSeconds   = apvts.getRawParameterValue("egReleaseSec")->load();
        egParams.velocityAmount01 = apvts.getRawParameterValue("egVelAmount")->load();

        egParams.attackMode = (modztakt::eg::AttackMode) (int) apvts.getRawParameterValue("egAttackMode")->load();
        egParams.releaseLongMode = apvts.getRawParameterValue("egReleaseLong")->load() > 0.5f;

        egParams.decayCurveMode   = (modztakt::eg::CurveShape) (int) apvts.getRawParameterValue("egDecayCurve")->load();
        egParams.releaseCurveMode = (modztakt::eg::CurveShape) (int) apvts.getRawParameterValue("egReleaseCurve")->load();

        egEngine.setParams(egParams);

        const int egDestChoice = (int) apvts.getRawParameterValue("egDestParamIndex")->load();
        
        // CLEANED: Simplified EG->LFO mode logic with clearer variable names
        const int egMidiDestCount = SyntaktParameterEgIndex.size();
        const bool egToLfoMode = (egDestChoice >= egMidiDestCount);
        const int egToLfoRouteIndex = egToLfoMode ? (egDestChoice - egMidiDestCount) : -1;
        const bool egToLfoEffective = egIsEnabled && egToLfoMode;


        // scope view
        bool scopeOn = apvts.getRawParameterValue("scope")->load() > 0.5f;

        // 0) Parse incoming MIDI -> set pending flags (same semantics as GlobalMidiCallback)
        parseIncomingMidiBuffer (midiIn,
                                 pending,
                                 syncEnabled,
                                 [this](const juce::MidiMessage& m){ midiClock.handleIncomingMidiMessage (nullptr, m); },
                                 noteRestartToggleState,
                                 noteOffStopToggleState);

        applyPendingTransportEvents(shape, syncEnabled);

        const double bpm = updateTempoFromHostOrMidiClock(syncEnabled);

        // --- Host transport -> pending transport events (plugin case) ---
        // if DAWs do NOT send MIDI Start/Stop to plugins; use playhead instead.
        if (syncEnabled && hostTransportValid.load(std::memory_order_relaxed))
        {
            const bool hostPlaying = hostTransportRunning.load(std::memory_order_relaxed);

            if (hostPlaying && !lastHostPlaying)
            {
                transportStartPending.store(true, std::memory_order_release);

                // Make gating effective immediately for this block
                transportRunning.store(true, std::memory_order_release);

                if (startOnPlay.load(std::memory_order_relaxed))
                {
                    lfoForcedActiveByPlay = true;
                    lfoRuntimeMuted = false;
                    requestLfoRestart.store(true, std::memory_order_release);

                    if (!lfoActiveParam)
                        uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
                }
            }

            if (!hostPlaying && lastHostPlaying)
            {
                transportStopPending.store(true, std::memory_order_release);

                // Make gating effective immediately
                transportRunning.store(false, std::memory_order_release);

                // HARD STOP
                lfoRuntimeMuted = true;
                lfoForcedActiveByNote = false;
                lfoForcedActiveByPlay = false;

                // Update UI param to OFF
                uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
            }

            lastHostPlaying = hostPlaying;
        }


        double rateHz = rateSliderValueHz;

        if (syncEnabled && bpm > 0.0)
        {
                rateHz = modztakt::lfo::updateLfoRateFromBpm (rateHz, bpm, syncDivisionId);
                // only request UI update if it actually changed enough
                const float current = apvts.getRawParameterValue("lfoRateHz")->load();
                if (std::abs((float)rateHz - current) > 0.0005f)
                {
                    uiRateHzToSet.store((float)rateHz, std::memory_order_relaxed);
                    uiRequestSetRateHz.store(true, std::memory_order_release);
                }
        }

        const bool transportOk = (!syncEnabled) || transportRunning.load(std::memory_order_acquire);

        // EG-forced run ignores transport gate
        const bool wantsLfo = lfoActiveParam || lfoForcedActiveByNote || lfoForcedActiveByPlay || lfoForcedActiveByEg;

        const bool shouldRunLfo = wantsLfo && (transportOk || (lfoForcedActiveByEg && egToLfoEffective));

        modztakt::lfo::applyLfoActiveState (shouldRunLfo,
                                   shape,
                                   lfoActive,
                                   lfoRuntimeMuted,
                                   lfoRoutes,
                                   lfoPhase);

        // 1) NOTE ON
        if (pending.pendingNoteOn.exchange(false, std::memory_order_acq_rel))
        {
            const int ch   = pending.pendingNoteChannel.load(std::memory_order_relaxed);
            // CLEANED: Removed unused 'note' variable
            const float velocity = pending.pendingNoteVelocity.load (std::memory_order_relaxed);

            // --- EG ---
            if (egIsEnabled.load (std::memory_order_relaxed) && ch == egSourceChannel)
                egEngine.noteOn (velocity);

            // --- LFO Note Restart ---
            if (noteRestartToggleState)
            {
                const bool matchesSource = (noteSourceChannel <= 0) ? true : (ch == noteSourceChannel);

                // If EG->LFO protected run is active, ignore noteRestart unless this note-on
                // is from the EG note source channel (so EG + its LFO route can retrigger together).
                const bool allowRestartNow = ! (lfoForcedActiveByEg && egToLfoEffective) || (ch == egSourceChannel);

                if (matchesSource && allowRestartNow)
                {
                    if (!lfoActiveParam)
                        uiRequestSetLfoActiveOn.store(true, std::memory_order_release);

                    lfoForcedActiveByNote = true;
                    requestLfoRestart.store(true, std::memory_order_release);

                    for (int i = 0; i < maxRoutes; ++i)
                        lfoRoutes[i].totalPhaseAdvanced = 0.0;
                }
            }
        }

        // 2) NOTE OFF
        if (pending.pendingNoteOff.exchange(false, std::memory_order_acq_rel))
        {
            const int ch = pending.pendingNoteChannel.load (std::memory_order_relaxed);
            // CLEANED: Removed unused 'note' variable

            if (egIsEnabled.load (std::memory_order_relaxed) && ch == egSourceChannel)
            {
                egEngine.noteOff ();
            }
            else
            {
                uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
            }
        }

        // 3) Stop LFO on Note-Off (requested by callback logic)
        // If EG->LFO is currently forcing a protected run, ignore note-off stop for the duration.
        if (pending.requestLfoStop.exchange(false, std::memory_order_acq_rel))
        {
            // Normal case: no EG-protected route -> keep your old global stop behavior.
            if (!lfoForcedActiveByEg)
            {
                lfoRuntimeMuted = true;
                lfoForcedActiveByNote = false;

                // Clear per-route suppression mask (not strictly needed, but keeps state tidy)
                for (int i = 0; i < maxRoutes; ++i)
                    lfoRouteSuppressedByNoteOff[i] = false;

                for (int i = 0; i < maxRoutes; ++i)
                {
                    lfoRoutes[i].totalPhaseAdvanced = 0.0;
                    lfoRoutes[i].hasFinishedOneShot = true;
                    lfoRoutes[i].passedPeak = true;
                }
            }
            else
            {
                // EG is currently protecting one route. Stop other routes only.
                lfoForcedActiveByNote = false;   // note forcing ends
                lfoForcedActiveByPlay = false;   // optional: treat note-off as ending forced play too

                // Do NOT globally mute the engine, we still need the EG-protected route to run.
                lfoRuntimeMuted = false;

                for (int i = 0; i < maxRoutes; ++i)
                {
                    lfoRouteSuppressedByNoteOff[i] = (i != lfoForcedEgRouteIndex);
                }
            }
        }

        // 4) Restart request
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

                lfoRouteSuppressedByNoteOff[i] = false;
            }

            if (! lfoActive)
                lfoActive = true;
        }

        double eg01 = 0.0;
        bool egHasValue = false;

        if (egIsEnabled.load(std::memory_order_relaxed))
        {
            egHasValue = egEngine.processBlock(audio.getNumSamples(), eg01);
            eg01 = juce::jlimit(0.0, 1.0, eg01);

            // EG->LFO protected-run: while EG is active and destination is EG->LFO Route X,
            // force LFO engine to run even if transport stops / noteOffStop happens.
            // CLEANED: Simplified egToLfoRouteIndexEffective check
            if (egToLfoEffective && egToLfoRouteIndex >= 0 && egToLfoRouteIndex < maxRoutes)
            {
                if (egHasValue)
                {
                    lfoForcedActiveByEg = true;
                    lfoForcedEgRouteIndex = egToLfoRouteIndex;
                    lfoRuntimeMuted = false;
                }
                // If egHasValue == false, keep forced-run true until your "release after ramp" code clears it.
            }
            else
            {
                // EG disabled OR destination not EG->LFO => release priority immediately
                lfoForcedActiveByEg = false;
                lfoForcedEgRouteIndex = -1;

                for (int i = 0; i < maxRoutes; ++i)
                    lfoRouteSuppressedByNoteOff[i] = false;
            }
        }

        // LFO generation + send
        if (lfoActive && !lfoRuntimeMuted)
        {
            // sampleRate in plugin is per-sample
            const double phaseIncPerSample = rateHz / juce::jmax(1.0, getSampleRate());

            const int blockSize = audio.getNumSamples();

            int stepSamples = (int) std::round (juce::jmax (1.0, getSampleRate())
                                                / juce::jmax (0.001, rateHz) / 128.0);
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

                    // If noteOffStop suppressed this route, skip it (unless it's the EG-protected route)
                    if (lfoRouteSuppressedByNoteOff[i])
                        continue;

                    // Accumulate before advancing
                    route.totalPhaseAdvanced += phaseIncThis;

                    const bool wrapped = modztakt::lfo::advancePhase (lfoPhase[i], phaseIncThis);

                    double shapeComputed = modztakt::lfo::computeWaveform (shape,
                                                                          lfoPhase[i],
                                                                          route.bipolar,
                                                                          route.invertPhase,
                                                                          random);

                    // One-shot: complete after full cycle
                    if (route.oneShot && route.totalPhaseAdvanced >= 1.0)
                        route.hasFinishedOneShot = true;

                    const auto& param = syntaktParameters[route.parameterIndex];

                    const double depth = depthSliderValue;

                    // EG->LFO: gate + neutral on end (shape-domain only)
                    bool shouldSend = true;

                    const bool egToThisRoute = (egToLfoRouteIndex == i);
                    
                    // If LFO isn't user-enabled and isn't forced by note/play, then during EG-forced run
                    // only the targeted route is allowed to emit MIDI.
                    const bool anyUserOrOtherForce = lfoActiveParam || lfoForcedActiveByNote || lfoForcedActiveByPlay;

                    if (!anyUserOrOtherForce && lfoForcedActiveByEg)
                    {
                        if (i != lfoForcedEgRouteIndex)
                            continue;
                    }

                    if (egToThisRoute)
                    {
                        const bool gateOpen = egHasValue;

                        // Route neutral in waveform domain:
                        // bipolar -> 0.0
                        // unipolar -> -1.0 (maps to 0)
                        // unipolar+invert -> +1.0 (maps to 127)
                        const double neutral = neutralShapeForRoute(route.bipolar, route.invertPhase);

                        // Detect falling edge: gate was open, now closed => start ramp toward neutral
                        if (!gateOpen && egGateWasOpen[i])
                        {
                            neutralRampActive[i] = true;

                            const double safeDepth = juce::jmax(1.0e-6, depth);
                            neutralRampStart[i]  = lastShapeDepthVal[i] / safeDepth; // back to shape domain
                            neutralRampTarget[i] = neutral;
                            neutralRampPos[i]    = 0;
                        }

                        // Update gate memory
                        egGateWasOpen[i] = gateOpen;

                        if (gateOpen)
                        {
                            // Gate open: cancel any pending neutral ramp
                            neutralRampActive[i] = false;

                            // fade from neutral -> full waveform by EG amount
                            const double egAmt = juce::jlimit(0.0, 1.0, eg01);
                            shapeComputed = neutral + (shapeComputed - neutral) * egAmt;
                        }
                        else
                        {
                            // Gate closed: either emit the neutral ramp, or fully mute
                            if (neutralRampActive[i])
                            {
                                const double t =
                                    (neutralRampSteps <= 1)
                                        ? 1.0
                                        : (double) neutralRampPos[i] / (double) (neutralRampSteps - 1);

                                shapeComputed = neutralRampStart[i]
                                              + (neutralRampTarget[i] - neutralRampStart[i]) * t;

                                ++neutralRampPos[i];
                                if (neutralRampPos[i] >= neutralRampSteps)
                                    neutralRampActive[i] = false;
                            }
                            else
                            {
                                shouldSend = false; // fully muted until EG triggers again
                            }
                        }
                    }
                    else
                    {
                        // EG->LFO off: clear state so we don't edge-trigger later
                        egGateWasOpen[i] = false;
                        neutralRampActive[i] = false;
                    }

                    if (!shouldSend)
                        continue;

                    // MIDI mapping
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

                    // Store last (shape * depth) so ramp starts from the true last output
                    lastShapeDepthVal[i] = shapeComputed * depth;

                    sendThrottledParamValueToBuffer (midi, i, route.midiChannel, param, midiVal, offset);

                    if (scopeRoutesEnabled[i].load(std::memory_order_relaxed))
                        scopeValues[i].store((float)(shapeComputed * depth), std::memory_order_relaxed);

                    juce::ignoreUnused(wrapped);
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

                    if (!r.oneShot)
                    {
                        anyRouteStillRunning = true;
                        break;
                    }

                    if (!r.hasFinishedOneShot)
                    {
                        anyRouteStillRunning = true;
                        break;
                    }
                }

                if (anyEnabledRoute && !anyRouteStillRunning)
                {
                    lfoRuntimeMuted = true;
                    lfoForcedActiveByNote = false;
                    uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
                }
            }
        }

        // Release EG-forced run only after EG ended AND neutral ramp finished.
        if (lfoForcedActiveByEg && !egHasValue)
        {
            const int r = lfoForcedEgRouteIndex;

            const bool routeValid = (r >= 0 && r < maxRoutes);

            if (!routeValid)
            {
                lfoForcedActiveByEg = false;
                lfoForcedEgRouteIndex = -1;
            }
            else
            {
                const bool rampDone = !neutralRampActive[r] && !egGateWasOpen[r];

                if (rampDone)
                {
                    lfoForcedActiveByEg = false;
                    lfoForcedEgRouteIndex = -1;

                    // CLEANED: Removed commented-out code and simplified logic
                    const bool refreshStartButton = lfoForcedActiveByNote || lfoForcedActiveByPlay || lfoForcedActiveByEg;
                    if (!refreshStartButton)
                        uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
                }
            }
        }

        // EG send (re-use eg01 computed earlier in the block)
        if (egHasValue)
        {
           #if JUCE_DEBUG
            const float egScopeValue = (float)(eg01 * 2.0 - 1.0);
            scopeValues[2].store(egScopeValue, std::memory_order_relaxed);
           #endif

            const int outCh  = (int) apvts.getRawParameterValue("egOutChannel")->load();

            if (!egToLfoEffective) // send to MIDI output
            {
                const int paramIdx = SyntaktParameterEgIndex[egDestChoice];

                // UI already prevents this conflict, automation safety guard
                bool egConflictsWithLfo = false;

                for (int r = 0; r < maxRoutes; ++r)
                {
                    const auto& lfoR = lfoRoutes[r];
                    if (lfoR.midiChannel == outCh && lfoR.parameterIndex == paramIdx)
                    {
                        egConflictsWithLfo = true;
                        break;
                    }
                }
                if (!egConflictsWithLfo)
                {
                    const int egValue  = mapEgToMidi(eg01, paramIdx);

                    const auto& param = syntaktParameters[paramIdx];

                    sendThrottledParamValueToBuffer(midi,
                                                    EG_ROUTE_KEY,  // CLEANED: Use constant for magic number
                                                    outCh,
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

    // Helper to convert APVTS choice index to changeThreshold value
    static inline int getChangeThresholdFromIndex(int index)
    {
        const int values[] = {0, 1, 2, 4, 8};
        return (index >= 0 && index < 5) ? values[index] : 0;
    }

    // Helper to convert changeThreshold value to APVTS choice index
    static inline int getIndexFromChangeThreshold(int threshold)
    {
        switch (threshold)
        {
            case 0: return 0;
            case 1: return 1;
            case 2: return 2;
            case 4: return 3;
            case 8: return 4;
            default: return 0;
        }
    }

    // Helper to convert APVTS choice index to msFloofThreshold value
    static inline double getMsFloofThresholdFromIndex(int index)
    {
        const double values[] = {0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0};
        return (index >= 0 && index < 7) ? values[index] : 0.0;
    }

    // Helper to convert msFloofThreshold value to APVTS choice index
    static inline int getIndexFromMsFloofThreshold(double threshold)
    {
        if (threshold == 0.0) return 0;
        if (threshold == 0.5) return 1;
        if (threshold == 1.0) return 2;
        if (threshold == 1.5) return 3;
        if (threshold == 2.0) return 4;
        if (threshold == 3.0) return 5;
        if (threshold == 5.0) return 6;
        return 0;
    }

    //==============================================================================
    // PUBLIC INTERFACE FOR UI
    //==============================================================================
public:
    APVTS apvts;

    // LFO Start/Stop label refresh
    bool isLfoRunningForUi() const noexcept
    {
        return uiLfoIsRunning.load(std::memory_order_acquire);
    }

    std::atomic<bool> uiRequestSetLfoActiveOn { false };
    std::atomic<bool> uiRequestSetLfoActiveOff { false };

    // Scope accessors for UI (safe: atomics)
    inline auto& getScopeValues() noexcept { return scopeValues; }
    inline auto& getScopeRoutesEnabled() noexcept { return scopeRoutesEnabled; }

    double getBpmForUi() const noexcept { return bpmForUi.load(std::memory_order_relaxed); }

    std::atomic<bool>  uiRequestSetRateHz { false };
    std::atomic<float> uiRateHzToSet { 0.0f };

    // Settings parameters (accessed by UI and audio thread)
    std::atomic<int> changeThreshold { 0 };
    std::atomic<double> msFloofThreshold { 0.0 };

    inline APVTS&       getAPVTS()       noexcept { return apvts; }

    inline double getSampleRateCached() const noexcept { return cachedSampleRate; }
    inline int    getBlockSizeCached()  const noexcept { return cachedBlockSize; }

    //==============================================================================
    // PRIVATE IMPLEMENTATION
    //==============================================================================
private:
    // Audio processing state
    double cachedSampleRate = 48000.0;
    int    cachedBlockSize  = 0;

    // Helpers for per-sample scheduling (updated each processBlock)
    double currentBlockStartMs = 0.0;
    double msPerSample = 0.0;
    double timeMs = 0.0;

    // LFO state flags
    bool lfoRuntimeMuted = false;
    bool lfoForcedActiveByNote = false;
    bool lastLfoActiveParam = false;
    bool lfoActive = false;

    std::atomic<bool> uiLfoIsRunning { false };

    // Transport and sync
    std::atomic<bool> transportRunning { false };
    std::atomic<bool> transportStartPending { false };
    std::atomic<bool> transportStopPending  { false };

    std::atomic<bool> startOnPlay { false };
    bool lfoForcedActiveByPlay = false;

    // EG -> LFO "protected run" (Option A: only UI Start/Stop can kill)
    bool lfoForcedActiveByEg = false;      // audio thread only
    int  lfoForcedEgRouteIndex = -1;       // 0..maxRoutes-1, audio thread only

    std::atomic<double> bpmForUi { 0.0 };
    std::atomic<bool>   hostTransportRunning { false };
    std::atomic<bool>   hostTransportValid { false };
    bool lastHostPlaying = false; // audio thread only

    // Pending note flags (replaces GlobalMidiCallback storage)
    PendingMidiFlags pending;

    // MIDI clock (same class as you used in MainComponent)
    MidiClockHandler midiClock;

    // LFO state
    static constexpr int maxRoutes = 3;
    static constexpr int EG_ROUTE_KEY = 0x7FFF;  // CLEANED: Named constant for magic number

    std::array<RouteSnapshot, maxRoutes> lastRouteSnapshot {};

    std::array<LfoRoute, maxRoutes> lfoRoutes {};
    std::array<double,   maxRoutes> lfoPhase  {};

    // When noteOffStop happens while EG is protecting one route,
    // we stop the other routes without killing the EG-protected one.
    std::array<bool, maxRoutes> lfoRouteSuppressedByNoteOff { false, false, false };


    // EG->LFO gate + neutral ramp (per route)
    std::array<bool,   maxRoutes> egGateWasOpen     { false, false, false };

    // Store last *shape value* after depth (so we can ramp smoothly in the same domain)
    // This should be the value you scope: shapeComputed * depth  (range ~ [-depth..+depth])
    std::array<double, maxRoutes> lastShapeDepthVal { 0.0, 0.0, 0.0 };

    std::array<bool,   maxRoutes> neutralRampActive { false, false, false };
    std::array<double, maxRoutes> neutralRampStart  { 0.0, 0.0, 0.0 };
    std::array<double, maxRoutes> neutralRampTarget { 0.0, 0.0, 0.0 };
    std::array<int,    maxRoutes> neutralRampPos    { 0, 0, 0 };

    static constexpr int neutralRampSteps = 8; // per-block steps (fast, but smooth enough)

    juce::Random random;

    std::atomic<bool> requestLfoRestart { false };

    // EG
    modztakt::eg::Engine egEngine;
    std::atomic<bool> egIsEnabled { false };

    // Throttle state for outgoing MIDI
    std::unordered_map<int, int>    lastSentValuePerParam;
    std::unordered_map<int, double> lastSendTimePerParam;

    // Scope (shared audio->UI)
    std::array<std::atomic<float>, maxRoutes> scopeValues { 0.0f, 0.0f, 0.0f };
    std::array<std::atomic<bool>,  maxRoutes> scopeRoutesEnabled { false, false, false };

    // EG filtered destinations
    static inline juce::Array<int> SyntaktParameterEgIndex = []()
    {
        juce::Array<int> indices;
        for (int i = 0; i < (int) juce::numElementsInArray(syntaktParameters); ++i)
        {
            if (syntaktParameters[i].egDestination)
                indices.add(i);
        }
        return indices;
    }();
    
    static inline const juce::StringArray SyntaktParameterEG = []()
    {
        juce::StringArray filteredEgDest;
        for (int i = 0; i < (int) juce::numElementsInArray(syntaktParameters); ++i)
        {
            if (syntaktParameters[i].egDestination)
                filteredEgDest.add(syntaktParameters[i].name);
        }
        filteredEgDest.add("EG to LFO Route 1");
        filteredEgDest.add("EG to LFO Route 2");
        filteredEgDest.add("EG to LFO Route 3");

        return filteredEgDest;
    }();
    
    // lerp (math) = "Calculates a number between two numbers at a specific increment"
    static inline double lerp(double a, double b, double t) noexcept
    {
        return a + (b - a) * t;
    }

    // Target in "shape*depth domain" (NOT MIDI domain):
    // bipolar -> 0.0
    // unipolar -> -1.0 (so (shape+1)*0.5 => 0)
    // unipolar+invert -> +1.0 (so (shape+1)*0.5 => 1)
    static inline double neutralShapeForRoute(bool bipolar, bool invertPhase) noexcept
    {
        if (bipolar) return 0.0;
        return invertPhase ? +1.0 : -1.0;
    }


    //==============================================================================

    inline static APVTS::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

        // Main switches
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

        p.push_back (std::make_unique<juce::AudioParameterBool>("playStart", "Start on Play", false));

        p.push_back (std::make_unique<juce::AudioParameterChoice>( // syncDivision
            "syncDivision", "Sync Division",
            juce::StringArray{ "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8 dotted", "1/16 dotted" }, 0));

        // Note restart feature
        p.push_back (std::make_unique<juce::AudioParameterBool>("noteRestart", "Note Restart", false));
        p.push_back (std::make_unique<juce::AudioParameterInt>("noteSourceChannel", "Note Restart Channel", 1, 16, 1));
        p.push_back (std::make_unique<juce::AudioParameterBool>("noteOffStop", "Stop on Note Off", false));

        // Scope view
        p.push_back (std::make_unique<juce::AudioParameterBool>("scope", "Scope View", false));

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

        // EG
        // Master switch
        p.push_back (std::make_unique<juce::AudioParameterBool>(
            "egEnabled", "EG Enabled", false));

        // Time params are stored in seconds (same as your sliders)
        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egAttackSec", "EG Attack",
            juce::NormalisableRange<float>(0.0005f, 10.0f, 0.0f, 0.40f),
            0.01f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egHoldSec", "EG Hold",
            juce::NormalisableRange<float>(0.0f, 5.0f),
            0.0f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egDecaySec", "EG Decay",
            juce::NormalisableRange<float>(0.001f, 10.0f, 0.0f, 0.45f),
            0.20f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egSustain", "EG Sustain",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.70f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egReleaseSec", "EG Release",
            juce::NormalisableRange<float>(0.005f, 10.0f, 0.0f, 0.45f),
            0.20f));

        // Velocity amount (0..1)
        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egVelAmount", "EG Velocity Amount",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.0f));

        // Attack mode (0..2)
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "egAttackMode", "EG Attack Mode",
            juce::StringArray { "Fast", "Long", "Snap" }, 0));

        // Release long toggle
        p.push_back (std::make_unique<juce::AudioParameterBool>(
            "egReleaseLong", "EG Release Long", false));

        // Curve choices (0..2)
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "egDecayCurve", "EG Decay Curve",
            juce::StringArray { "Linear", "Exponential", "Logarithmic" }, 1));

        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "egReleaseCurve", "EG Release Curve",
            juce::StringArray { "Linear", "Exponential", "Logarithmic" }, 1));

        // Input note filtering for EG (0=Disabled, 1..16 channels)
        p.push_back (std::make_unique<juce::AudioParameterInt>(
            "egNoteSourceChannel", "EG Note Source Channel",
            1, 16, 1));

        // EG output MIDI channel (1..16)
        p.push_back (std::make_unique<juce::AudioParameterInt>(
            "egOutChannel", "EG Out Channel",
            1, 16, 1));

        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "egDestParamIndex",
            "EG Destination Param Index",
            SyntaktParameterEG, 
            ModzTaktAudioProcessor::findFirstEgDestination()));  // Default to first EG destination

        // SETTINGS MENU PARAMETERS (Performance)
        // MIDI Data Throttle (change threshold in steps)
        p.push_back(std::make_unique<juce::AudioParameterChoice>(
            "midiDataThrottle",
            "MIDI Data Throttle",
            juce::StringArray{"Off (send every change)", "1 step (fine)", "2 steps", "4 steps", "8 steps (coarse)"},
            1));  // Default to index 1 = 1 step

        // MIDI Rate Limiter (in milliseconds)
        // Options: 0.0 (Off), 0.5ms, 1.0ms, 1.5ms, 2.0ms, 3.0ms, 5.0ms
        p.push_back(std::make_unique<juce::AudioParameterChoice>(
            "midiRateLimiter",
            "MIDI Rate Limiter",
            juce::StringArray{"Off (send every change)", "0.5ms", "1.0ms", "1.5ms", "2.0ms", "3.0ms", "5.0ms"},
            0));  // Default to index 0 = Off

    return { p.begin(), p.end() };
    }

    //==============================================================================

    // Helpers
    inline bool isAnyScopeRouteEnabled() const noexcept
    {
        for (const auto& r : scopeRoutesEnabled)
            if (r.load(std::memory_order_relaxed))
                return true;
        return false;
    }

    inline void handleMidiStart() override
    {
        transportStartPending.store(true, std::memory_order_release);
    }

    inline void handleMidiStop() override
    {
        transportStopPending.store(true, std::memory_order_release);
    }

    inline void handleMidiContinue() override {}

    inline const double updateTempoFromHostOrMidiClock (bool syncEnabled)
    {
        if (!syncEnabled)
        {
            bpmForUi.store(0.0, std::memory_order_relaxed);
            hostTransportRunning.store(true, std::memory_order_relaxed);
            return 0.0;
        }

        double bpm = 0.0;
        bool valid = false;
        bool playing = false;  // CLEANED: Initialize before use, not with default true

        if (auto* playHead = getPlayHead())
        {
            auto pos = playHead->getPosition();
            if (pos.hasValue())
            {
                valid = true;
                // BPM (Optional<double>)
                if (auto hostBpm = pos->getBpm(); hostBpm.hasValue())
                {
                    const double v = *hostBpm;
                    if (std::isfinite(v) && v > 0.0)
                        bpm = v;
                }

                // isPlaying (bool)
                playing = pos->getIsPlaying();
            }
        }

        // Fallback to MIDI clock if host doesn't provide BPM
        hostTransportValid.store(valid, std::memory_order_relaxed);
        hostTransportRunning.store(playing, std::memory_order_relaxed);
        bpmForUi.store(bpm <= 0.0 ? midiClock.getCurrentBPM() : bpm, std::memory_order_relaxed);

        return bpmForUi.load(std::memory_order_relaxed);
    }

    inline void applyPendingTransportEvents (LfoShape shape, bool syncEnabled)
    {
        if (!syncEnabled)
        {
            transportRunning.store(true, std::memory_order_release);
            return;
        }

        const bool gotStart = transportStartPending.exchange(false, std::memory_order_acq_rel);
        const bool gotStop  = transportStopPending.exchange(false, std::memory_order_acq_rel);

        if (!gotStart && !gotStop)
            return;

        if (gotStart)
            transportRunning.store(true, std::memory_order_release);

        if (gotStop)
            transportRunning.store(false, std::memory_order_release);

        // Reset phases + one-shot runtime flags
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
            // HARD STOP (but EG->LFO protected run must finish its cycle)
            if (!lfoForcedActiveByEg)
            {
                lfoRuntimeMuted = true;
                lfoForcedActiveByNote = false;
                lfoForcedActiveByPlay = false;

                // Update UI param to OFF
                uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
            }
            else
            {
                // Transport stop cancels play/note forcing, but EG-forced run continues.
                lfoForcedActiveByNote = false;
                lfoForcedActiveByPlay = false;

                // keep producing LFO for the EG-forced route
                lfoRuntimeMuted = false;
            }
        }
        else if (gotStart)
        {
            lfoRuntimeMuted = false;

            if (startOnPlay.load(std::memory_order_relaxed))
            {
                lfoForcedActiveByPlay = true;
                uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
            }
        }
    }

    // Helper to find first valid destination
    static int findFirstEgDestination()
    {
        for (int i = 0; i < juce::numElementsInArray(syntaktParameters); ++i)
        {
            if (syntaktParameters[i].egDestination)
                return i;
        }
        return 0;
    }

    inline int mapEgToMidi (double egVal, int paramId) const
    {
        const auto& param = syntaktParameters[paramId];

        if (param.isBipolar)
        {
            const double center = (param.minValue + param.maxValue) * 0.5;
            const double range  = (param.maxValue - param.minValue) * 0.5;
            return (int) (center + (egVal * 2.0 - 1.0) * range);
        }

        return (int) (param.minValue + egVal * (param.maxValue - param.minValue));
    }

    inline void sendThrottledParamValueToBuffer (juce::MidiBuffer& midiOut,
                                             int routeIndex,
                                             int midiChannel,
                                             const SyntaktParameter& param,
                                             int midiValue,
                                             int sampleOffsetInBlock)
    {
        // Build per-route + per-parameter key
        static constexpr int CC_MASK = 0x1000;
        static constexpr int NRPN_MASK = 0x2000;

        const int paramKey =
            (routeIndex << 16) |
            (param.isCC ? CC_MASK : NRPN_MASK) |
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModzTaktAudioProcessor)
};

// declare factory (defined in PluginEntry.cpp)
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();