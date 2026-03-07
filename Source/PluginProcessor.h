#pragma once
#include <JuceHeader.h>

#include <array>
#include <unordered_map>

#include "MidiInParse.h"
#include "SyntaktParameterTable.h"
#include "MidiInput.h"
#include "EnvelopeEngine.h"
#include "LfoEngine.h"
#include "DelayEngine.h"

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

        // Delay
        delayEngine.setSampleRate(cachedSampleRate);
        delayEngine.reset();

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

    //==============================================================================
    // NEW: Consolidated state determination structure
    //==============================================================================
    struct LfoRunIntent
    {
        bool userButtonOn = false;          // UI button is ON
        bool userExplicitStop = false;      // User just clicked OFF (priority kill)
        bool noteForceActive = false;       // Note restart is forcing
        bool playForceActive = false;       // Transport start-on-play is forcing
        bool egForceActive = false;         // EG is forcing (at least one route)
        bool transportGateOpen = false;     // Transport is running (when sync enabled)
        bool syncEnabled = false;           // Sync mode active
        
        // Calculate if LFO should actually run
        bool shouldRun() const
        {
            // Priority 1: Explicit user stop always wins
            if (userExplicitStop)
            {
                return false;
            }
            
            // Priority 2: Any active forcing source OR user button
            const bool anyForce = noteForceActive || playForceActive || egForceActive || userButtonOn;
            if (!anyForce)
                return false;
            
            // Priority 3: Transport gate (EG forcing bypasses transport gate)
            if (syncEnabled && !transportGateOpen && !egForceActive)
                return false;
            
            return true;
        }
        
        // Determine if UI button should be ON
        bool shouldShowUiOn() const
        {
            return shouldRun() && !userExplicitStop;
        }
    };

    //==============================================================================
    // MAIN PROCESS BLOCK
    //==============================================================================


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

        const double rateSliderValueHz = apvts.getRawParameterValue("lfoRateHz")->load();
        const double depthSliderValue  = apvts.getRawParameterValue("lfoDepth")->load();

        const bool noteRestartToggleState = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
        // if (!noteRestartToggleState)
        //     lfoForcedActiveByNote = false;

        const bool noteOffStopToggleState = apvts.getRawParameterValue("noteOffStop")->load() > 0.5f;
        const int noteSourceChannel = (int) apvts.getRawParameterValue("noteSourceChannel")->load();
        const int syncDivisionId = (int) apvts.getRawParameterValue("syncDivision")->load() + 1;

        // Detect user explicit stop (button OFF)
        const bool userExplicitStop = lastLfoActiveParam && !lfoActiveParam;
        lastLfoActiveParam = lfoActiveParam;

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

        // Delay
        const int delaySourceChannel = (int) apvts.getRawParameterValue("delayNoteSourceChannel")->load();


        //======================================================================
        // EG Configuration
        //======================================================================
        const int egSourceChannel = (int) apvts.getRawParameterValue("egNoteSourceChannel")->load();


        modztakt::eg::Params egParams;
        egParams.enabled          = apvts.getRawParameterValue("egEnabled")->load() > 0.5f;
        egIsEnabled.store((bool) egParams.enabled, std::memory_order_release);

        egParams.attackSeconds    = apvts.getRawParameterValue("egAttack")->load();
        egParams.holdSeconds      = apvts.getRawParameterValue("egHold")->load();
        egParams.decaySeconds     = apvts.getRawParameterValue("egDecay")->load();
        egParams.sustain01        = apvts.getRawParameterValue("egSustain")->load();
        egParams.releaseSeconds   = apvts.getRawParameterValue("egRelease")->load();
        egParams.velocityAmount01 = apvts.getRawParameterValue("egVelAmount")->load();

        egParams.attackMode = (modztakt::eg::AttackMode) (int) apvts.getRawParameterValue("egAttackMode")->load();
        egParams.releaseLongMode = apvts.getRawParameterValue("egReleaseLong")->load() > 0.5f;

        egParams.decayCurveMode   = (modztakt::eg::CurveShape) (int) apvts.getRawParameterValue("egDecayCurve")->load();
        egParams.releaseCurveMode = (modztakt::eg::CurveShape) (int) apvts.getRawParameterValue("egReleaseCurve")->load();

        egEngine.setParams(egParams);

        // EG routing setup
        struct EgRouteRuntime
        {
            int channel = 0;        // 0=disabled
            int destChoice = 0;     // APVTS choice index
        };

        std::array<EgRouteRuntime, maxRoutes> egRoutesRt {};

        const int egMidiDestCount = (int) SyntaktParameterEgIndex.size();

        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);

            const int chChoice = (int) apvts.getRawParameterValue("egRoute" + rs + "_channel")->load();
            int ch = 0;  // 0=Disabled, -1=LFO, 1..16=MIDI channels

            if (chChoice == 0)      ch = 0;   // Disabled
            else                    ch = chChoice ;  // Ch 1..16 (choices 2..17 → 1..16)

            int destChoice = (int) apvts.getRawParameterValue("egRoute" + rs + "_dest")->load(); // master index

            // ALSO sanitize MIDI range to avoid out-of-bounds if automation wrote LFO indices:
            const int midiCount = (int)SyntaktParameterEgIndex.size();
            destChoice = juce::jlimit(0, midiCount - 1, destChoice);

            egRoutesRt[r] = { ch, destChoice };
        }

        // automation safety: enforce EG route exclusivity deterministically
        for (int r = 0; r < maxRoutes; ++r)
        {
            if (!egIsEnabled.load(std::memory_order_relaxed))
            {
                egRoutesRt[r].channel = 0;
                continue;
            }

            auto& cur = egRoutesRt[r];
            if (cur.channel == 0) continue;

            const int globalParamIdx = SyntaktParameterEgIndex[cur.destChoice];

            // conflict with LFO (same ch + same global param)
            bool conflictLfo = false;
            for (int lr = 0; lr < maxRoutes; ++lr)
            {
                if (lfoRoutes[lr].midiChannel == cur.channel && lfoRoutes[lr].parameterIndex == globalParamIdx)
                {
                    conflictLfo = true;
                    break;
                }
            }
            if (conflictLfo)
            {
                cur.channel = 0;
                continue;
            }

            // conflict with earlier EG routes (same ch + same global param)
            for (int j = 0; j < r; ++j)
            {
                const auto& prev = egRoutesRt[j];
                if (prev.channel == 0) continue;

                const int prevGlobalParam = SyntaktParameterEgIndex[prev.destChoice];
                if (prev.channel == cur.channel && prevGlobalParam == globalParamIdx)
                {
                    cur.channel = 0;
                    break;
                }
            }
        }

        // --- EG -> LFO routing map (per block, from EG routes selection) ---
        bool egToLfoDepthActive = apvts.getRawParameterValue("egToLfoDepth")->load() > 0.5f;
        bool egToLfoRateActive = apvts.getRawParameterValue("egToLfoRate")->load() > 0.5f;

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
                   // lfoRuntimeMuted = false;
                    requestLfoRestart.store(true, std::memory_order_release);

                    // if (!lfoActiveParam)
                    //     uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
                }
            }

            if (!hostPlaying && lastHostPlaying)
            {
                transportStopPending.store(true, std::memory_order_release);

                // Make gating effective immediately
                transportRunning.store(false, std::memory_order_release);

                // HARD STOP
                // lfoRuntimeMuted = true;
                lfoForcedActiveByNote = false;
                lfoForcedActiveByPlay = false;

                // Update UI param to OFF
                // uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
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

        // 1) NOTE ON
        if (pending.pendingNoteOn.exchange(false, std::memory_order_acq_rel))
        {
            const int ch   = pending.pendingNoteChannel.load(std::memory_order_relaxed);
            // CLEANED: Removed unused 'note' variable
            const float velocity = pending.pendingNoteVelocity.load (std::memory_order_relaxed);

            // --- EG ---
            if (egIsEnabled.load (std::memory_order_relaxed) && ch == egSourceChannel)
                egEngine.noteOn (velocity);

            // --- DELAY ---
            if (delayIsEnabled.load (std::memory_order_relaxed) && ch == delaySourceChannel)
            {
                const int note = pending.pendingNoteNumber.load (std::memory_order_relaxed);
                delayEngine.noteOn (note, velocity, blockStartMs);
            }

            // --- LFO Note Restart ---
            if (noteRestartToggleState)
            {
                const bool matchesSource = (noteSourceChannel <= 0) ? true : (ch == noteSourceChannel);

                // If EG->LFO protected run is active, ignore noteRestart unless this note-on
                // is from the EG note source channel (so EG + its LFO route can retrigger together).
                const bool allowRestartNow = !lfoForcedActiveByEg || (ch == egSourceChannel);

                if (matchesSource && allowRestartNow)
                {
                    lfoForcedActiveByNote = true;
                    lfoUiAutoOnByNote = true;

                    requestLfoRestart.store(true, std::memory_order_release);

                    for (int i = 0; i < maxRoutes; ++i)
                        lfoRoutes[i].totalPhaseAdvanced = 0.0;
                }
            }
        }

        // NOTE OFF
        if (pending.pendingNoteOff.exchange(false, std::memory_order_acq_rel))
        {
            const int ch = pending.pendingNoteChannel.load (std::memory_order_relaxed);

            // forward NOTE-OFF to EG if EG enabled and ch = egSourceChannel
            // LFO stopping is handled by pending.requestLfoStop (which is gated by noteOffStop toggle in the parser).
            if (egIsEnabled.load (std::memory_order_relaxed) && ch == egSourceChannel)
                egEngine.noteOff();

            // --- DELAY ---
            // Patches pending echo durations: min(inputNoteDuration, 70% of delayInterval)
            if (delayIsEnabled.load (std::memory_order_relaxed) && ch == delaySourceChannel)
            {
                const int note = pending.pendingNoteNumber.load (std::memory_order_relaxed);
                delayEngine.noteOff (note, blockStartMs);
            }
        }

        // If EG->LFO is currently forcing a protected run, ignore note-off stop for the duration.
        if (pending.requestLfoStop.exchange(false, std::memory_order_acq_rel))
        {
            if (!lfoForcedActiveByEg)
            {
                // Normal case: stop everything
                lfoForcedActiveByNote = false;   // note forcing ends
                lfoForcedActiveByPlay = false;   // note-off as ending forced transport-play too
                
                for (int i = 0; i < maxRoutes; ++i)
                {
                    lfoRouteSuppressedByNoteOff[i] = false;
                    lfoRoutes[i].totalPhaseAdvanced = 0.0;
                    lfoRoutes[i].hasFinishedOneShot = true;
                    lfoRoutes[i].passedPeak = true;
                }
                // If the Start button was only ON because noteRestart auto-enabled it,
                // then noteOffStop must turn it back OFF.
                if (lfoUiAutoOnByNote)
                {
                    lfoUiAutoOnByNote = false;

                    // Only flip the UI param if the user didn't manually latch it ON.
                    // (If user later clicked Start manually, we consider it "real".)
                    uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
                }

            }

            else
            {
                // EG is currently protecting one route. Stop other routes only.
                lfoForcedActiveByNote = false;   // note forcing ends
                lfoForcedActiveByPlay = false;   // treat note-off as ending forced play too
            }
        }

        // 4) Restart request
        if (requestLfoRestart.exchange(false, std::memory_order_acq_rel))
        {
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                lfoPhase[i] = modztakt::lfo::getWaveformStartPhase(shape, route.bipolar);

                route.hasFinishedOneShot = false;
                route.passedPeak = false;
                route.totalPhaseAdvanced = 0.0;

                lfoRouteSuppressedByNoteOff[i] = false;
            }
        }

        double eg01 = 0.0;
        bool egHasValue = false;

        if (egParams.enabled)
        {
            egHasValue = egEngine.processBlock(audio.getNumSamples(), eg01);
            eg01 = juce::jlimit(0.0, 1.0, eg01);
        }
        
        // EG is forcing LFO if it's modulating depth or rate
        lfoForcedActiveByEg = egHasValue && (egToLfoDepthActive || egToLfoRateActive);

        // ------------------------------------------------------------
        // EG -> LFO UI auto-latch + stop when EG ends (if EG was last)
        // ------------------------------------------------------------

        // If user manually latched the button ON while EG was driving, drop the auto flag
        if (lfoActiveParam)
            lfoUiAutoOnByEg = false;

        // Rising edge: EG starts forcing at least one route
        if (!egWasForcingLfo && lfoForcedActiveByEg)
        {
            // If UI button currently OFF, we auto-turn it ON (for visual consistency)
            if (!lfoActiveParam)
            {
                uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
                lfoUiAutoOnByEg = true;
            }
        }

        // Falling edge: EG stops forcing (all EG->LFO routes ended + ramps done)
        if (egWasForcingLfo && !lfoForcedActiveByEg)
        {
            const bool anyOtherForce = lfoForcedActiveByNote || lfoForcedActiveByPlay;

            // If EG was the last thing keeping LFO alive, stop it (unless note-force still true)
            if (lfoActiveParam && !anyOtherForce)
            {
                // Stop producing LFO immediately
                lfoRuntimeMuted = true;
                uiRequestSetLfoActiveOff.store(true, std::memory_order_release);

                // If the UI was ON only because EG auto-enabled it, turn it OFF now
                if (lfoUiAutoOnByEg)
                {
                    //uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
                    lfoUiAutoOnByEg = false;
                }
            }
            else
            {
                // EG ended but something else still drives LFO (note/play/user), so just clear auto state
                lfoUiAutoOnByEg = false;
            }
        }

        egWasForcingLfo = lfoForcedActiveByEg;

        // CONSOLIDATE ALL CONDITIONS AND DETERMINE LFO STATE
        LfoRunIntent intent;
        intent.userButtonOn = lfoActiveParam;
        intent.userExplicitStop = userExplicitStop;
        intent.noteForceActive = lfoForcedActiveByNote;
        intent.playForceActive = lfoForcedActiveByPlay;
        intent.egForceActive = lfoForcedActiveByEg;
        intent.transportGateOpen = transportRunning.load(std::memory_order_acquire);
        intent.syncEnabled = syncEnabled;

        const bool shouldRunLfo = intent.shouldRun();
        const bool shouldShowUiOn = intent.shouldShowUiOn();

        // Clear forcing flags if toggles are disabled
        if (!noteRestartToggleState)
            lfoForcedActiveByNote = false;
        
        if (!startOnPlayToggleState)
            lfoForcedActiveByPlay = false;

        // Handle user explicit stop
        if (userExplicitStop)
        {
            lfoForcedActiveByNote = false;
            lfoForcedActiveByPlay = false;
            lfoForcedActiveByEg = false;  // Simpler: just clear the flag
        }

        modztakt::lfo::applyLfoActiveState (shouldRunLfo,
                                   shape,
                                   lfoActive,
                                   lfoRuntimeMuted,
                                   lfoRoutes,
                                   lfoPhase);

        uiLfoIsRunning.store(shouldRunLfo && lfoActive && !lfoRuntimeMuted, std::memory_order_release);

        // SYNCHRONIZE UI BUTTON STATE
        // Detect state changes that require UI update
        const bool egWasDrivingLastBlock = egWasDrivingLfo;
        egWasDrivingLfo = lfoForcedActiveByEg;

        // EG started driving: turn UI on if not already
        if (!egWasDrivingLastBlock && lfoForcedActiveByEg && !lfoActiveParam)
        {
            uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
        }

        // EG stopped driving: turn UI off if nothing else is active
        if (egWasDrivingLastBlock && !lfoForcedActiveByEg)
        {
            const bool anyOtherForce = lfoForcedActiveByNote || lfoForcedActiveByPlay;
            if (!lfoActiveParam && !anyOtherForce)
            {
                uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
            }
        }

        // Update UI based on overall state if needed
        if (shouldShowUiOn && !lfoActiveParam)
        {
            uiRequestSetLfoActiveOn.store(true, std::memory_order_release);
        }
        else if (!shouldShowUiOn && lfoActiveParam && !userExplicitStop)
        {
            // Only auto-update UI to OFF if user didn't manually set it
            uiRequestSetLfoActiveOff.store(true, std::memory_order_release);
        }

        //======================================================================
        // DELAY CONFIGURATION
        //======================================================================

        modztakt::delay::Params delayParams;
        delayParams.enabled          = apvts.getRawParameterValue("delayEnabled")->load() > 0.5f;
        delayIsEnabled.store((bool) delayParams.enabled, std::memory_order_release);

        // NOTE: the Params field is delayTimeMs; APVTS id is "delayRate"
        delayParams.delayTimeMs = apvts.getRawParameterValue("delayRate")->load();
        delayParams.feedback  = apvts.getRawParameterValue("feedback")->load();

        const int delayEgShapeChoice = (int) apvts.getRawParameterValue ("delayEgShape")->load();

        const bool delayEgPerNote = apvts.getRawParameterValue ("delayEgPerNote")->load() > 0.5f;

        // Sync override: delaySyncDivision choice index 0 = Free, 1..8 = divisions.
        // Uses the same `bpm` and `syncEnabled` variables already computed above for the LFO.
        // If no clock is running (bpm == 0 or syncEnabled == false) the slider value is kept.
        const int delaySyncDivIdx = (int) apvts.getRawParameterValue("delaySyncDivision")->load();
        if (delaySyncDivIdx > 0 && syncEnabled && bpm > 0.0)
        {
            // divisionId is 1-based inside DelayEngine, matching choice index directly.
            delayParams.delayTimeMs = (float) modztakt::delay::divisionToMs(bpm, delaySyncDivIdx);
        }

        // Read output route channels (0 = Disabled, 1..16 = Ch1..Ch16) and transpose.
        for (int r = 0; r < maxRoutes; ++r)
        {
            const int chChoice = (int) apvts.getRawParameterValue("delayRoute" + juce::String(r) + "_channel")->load();

            delayParams.routeChannels[r]  = (chChoice == 0) ? 0 : chChoice;

            delayParams.routeTranspose[r] = (int) apvts.getRawParameterValue("delayRoute" + juce::String(r) + "_transpose")->load();
        }

        delayEngine.setParams(delayParams);

        // Per-note EG: driven by "delayEgPerNote" independently of "delayEgShape",
        // but only meaningful when a shaping destination is actually selected.
        // If delayEgShape == 0 (Off) the egPerNoteBtn is hidden in the UI and this
        // path is skipped so no spurious CC is sent.

        modztakt::delay::Params pne = delayEngine.getParams();
        pne.perNoteEg = delayEgPerNote && (delayEgShapeChoice > 0);
            
        if (pne.perNoteEg)
        {
            pne.noteEgParams         = egParams; // copy already-built EG params
            pne.noteEgParams.enabled = true;     // per-note EG always runs when active
        }
        
        delayEngine.setParams (pne);
        

        //======================================================================
        // LFO GENERATION
        //======================================================================

        if (lfoActive && !lfoRuntimeMuted)
        {
            // Apply EG to rate if routing is active
            double effectiveRateHz = rateHz;
            if (egToLfoRateActive && egHasValue)
            {
                // Scale rate by EG (0 to full rate)
                effectiveRateHz = rateHz * eg01;
            }

            // Use effectiveRateHz instead of rateHz in phase calculation
            const double phaseIncPerSample = effectiveRateHz / juce::jmax(1.0, getSampleRate());
            

            const int blockSize = audio.getNumSamples();

            int stepSamples = (int) std::round (juce::jmax (1.0, getSampleRate())
                                    / juce::jmax (0.001, effectiveRateHz) / 128.0);

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

                    // Apply EG modulation to depth or rate (globally)
                    double depth = depthSliderValue;
                    if (egToLfoDepthActive && egHasValue)
                    {
                        depth = depth * eg01;  // Scale depth by EG
                    }
                    
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

                    if (!r.oneShot || !r.hasFinishedOneShot)
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

        // EG MIDI OUTPUT
        if (egHasValue)
        {
           #if JUCE_DEBUG
            const float egScopeValue = (float)(eg01 * 2.0 - 1.0);
            scopeValues[2].store(egScopeValue, std::memory_order_relaxed);
           #endif

            for (int r = 0; r < maxRoutes; ++r)
            {
                const auto& er = egRoutesRt[r];
                if (er.channel == 0) continue;

                const int globalParamIdx = SyntaktParameterEgIndex[er.destChoice];
                const int egValue = mapEgToMidi(eg01, globalParamIdx);
                const auto& param = syntaktParameters[globalParamIdx];

                // Use a unique routeIndex key per EG route (not 0x7FFF for all)
                const int egRouteKey = (EG_ROUTE_KEY + r); // e.g. 0x7FFF, 0x8000, 0x8001
                sendThrottledParamValueToBuffer(midi,
                                                egRouteKey,
                                                er.channel,
                                                param,
                                                egValue,
                                                0);
            }
        }

        //======================================================================
        // EG → DELAY ECHO SHAPING
        // When delayEgShape > 0 and the EG is producing a value, we send the
        // current EG level as a CC / NRPN volume message to every MIDI channel
        // that currently has delay notes sounding.  This shapes the perceived
        // amplitude envelope of each echo using the Syntakt's own amp-EG input.
        //
        // The note-off cap in DelayEngine limits echo duration to 70 % of the
        // delay interval (or the original note duration if shorter), so the EG
        // attack + hold + early-decay stages are what naturally get applied —
        // matching the spirit of "EG shapes the echo, not the whole reverb tail".
        //======================================================================

        if (delayEgShapeChoice > 0 && !delayEgPerNote && egHasValue && delayParams.enabled)
        {
            // Resolve parameter without hardcoding CC numbers: use the same
            // syntaktParameters[] table entry that the LFO/EG routes use.
            const int paramIdx = (delayEgShapeChoice == 1)
                                 ? delayEgVolumeParamIdx    // "Amp: Volume"
                                 : delayEgTrackLvlParamIdx; // "Track Level"

            const auto& param   = syntaktParameters[paramIdx];
            const int   egValue = mapEgToMidi (eg01, paramIdx);

            // Ask the engine which channels have notes currently sounding.
            std::array<bool, 17> soundingChannels {};
            delayEngine.getActiveSoundingChannels (soundingChannels);

            for (int ch = 1; ch <= 16; ++ch)
            {
                if (!soundingChannels[ch])
                    continue;

                // One throttle-map slot per channel so rate-limiting works per-channel.
                sendThrottledParamValueToBuffer (midi,
                                                 DELAY_EG_SHAPE_KEY_BASE + ch,
                                                 ch,
                                                 param,
                                                 egValue,
                                                 0 /*sampleOffset*/);
            }
        }

        //======================================================================
        // PER-NOTE EG OUTPUT
        //
        // When "delayEgPerNote" is true AND a destination is selected, each echo
        // has its own EG retriggered at its note-on.  We mirror the UI rule:
        // egPerNoteBtn is only visible when delayEgShape > 0, so the processor
        // matches by also requiring delayEgShapeChoice > 0 here.
        //======================================================================

        if (delayEgPerNote && delayEgShapeChoice > 0 && delayParams.enabled)
        {
            const auto& pnEgOut = delayEngine.getPerNoteEgOutput();

            if (pnEgOut.hasAnyValue)
            {
                const int paramIdx = (delayEgShapeChoice == 1)
                                 ? delayEgVolumeParamIdx    // "Amp: Volume"
                                 : delayEgTrackLvlParamIdx; // "Track Level"

                const auto& param   = syntaktParameters[paramIdx];

                for (int ch = 1; ch <= 16; ++ch)
                {
                    const float eg01 = pnEgOut.maxEg01[ch];
                    if (eg01 <= 0.0f)
                        continue;

                    const int midiVal = mapEgToMidi (static_cast<double> (eg01), paramIdx);

                    // Throttle key range: DELAY_EG_SHAPE_KEY_BASE + 0x20 + ch
                    // (distinct from the global EG shaping keys at +0x00..+0x10).
                    sendThrottledParamValueToBuffer (midi,
                                                     DELAY_EG_SHAPE_KEY_BASE + 0x20 + ch,
                                                     ch,
                                                     param,
                                                     midiVal,
                                                     0 /*sampleOffset*/);
                }
            }
        }

        //======================================================================
        // DELAY MIDI OUTPUT
        // Must run every block (engine advances its internal clock even when idle)
        //======================================================================

        delayEngine.processBlock (audio.getNumSamples(), blockStartMs, midi);

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

    // EG filtered destinations (MIDI-capable only)
    static inline juce::Array<int> SyntaktParameterEgIndex = []()
    {
        juce::Array<int> indices;
        for (int i = 0; i < (int) juce::numElementsInArray(syntaktParameters); ++i)
            if (syntaktParameters[i].egDestination)
                indices.add(i);
        return indices;
    }();

    static inline int egMidiDestCount() { return SyntaktParameterEgIndex.size(); }

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

    static constexpr int maxRoutes = 3;

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

    // EG -> LFO forcing (no longer per-route)
    bool lfoForcedActiveByEg = false;

    std::atomic<double> bpmForUi { 0.0 };
    std::atomic<bool>   hostTransportRunning { false };
    std::atomic<bool>   hostTransportValid { false };
    bool lastHostPlaying = false; // audio thread only

    // Pending note flags (replaces GlobalMidiCallback storage)
    PendingMidiFlags pending;

    // MIDI clock (same class as you used in MainComponent)
    MidiClockHandler midiClock;

    //
    static constexpr int EG_ROUTE_KEY = 0x7FFF;  // Named constant for magic number

    // Delay EG-shaping uses one throttle slot per MIDI channel (channels 1..16).
    // Range 0x9001..0x9010 — no overlap with LFO (0..2) or EG routes (0x7FFF..0x8001).
    static constexpr int DELAY_EG_SHAPE_KEY_BASE = 0x9001;
    
    std::array<RouteSnapshot, maxRoutes> lastRouteSnapshot {};

    std::array<LfoRoute, maxRoutes> lfoRoutes {};
    std::array<double,   maxRoutes> lfoPhase  {};

    // When noteOffStop happens while EG is protecting one route,
    // we stop the other routes without killing the EG-protected one.
    std::array<bool, maxRoutes> lfoRouteSuppressedByNoteOff { false, false, false };

    bool lfoUiAutoOnByNote = false;   // UI Start was turned ON by noteRestart (not by the user)
    bool lfoUiAutoOnByEg = false;     // UI Start was turned ON by EG forcing
    bool egWasForcingLfo = false;     // previous-block EG forcing state (edge detector)

    juce::Random random;

    std::atomic<bool> requestLfoRestart { false };

    // EG
    modztakt::eg::Engine egEngine;
    std::atomic<bool> egIsEnabled { false };

    bool egWasDrivingLfo = false;  // audio thread only, edge detector for UI

    // Delay
    modztakt::delay::Engine delayEngine;
    std::atomic<bool> delayIsEnabled { false };

    // Resolved once at class-init time: index into syntaktParameters[] for the two
    // parameters used by the Delay EG-shaping feature.  Using a name-based lookup
    // avoids hardcoding CC numbers and automatically picks up NRPN if the table
    // entry is ever switched to NRPN for higher resolution.

    static inline int findSyntaktParamIndexByName (const char* name) noexcept
    {
        for (int i = 0; i < (int) juce::numElementsInArray (syntaktParameters); ++i)
            if (juce::String (syntaktParameters[i].name) == name)
                return i;
        jassertfalse; // entry not found — check spelling against SyntaktParameterTable.h
        return 0;
    }

    // "Amp: Volume"  → CC 7 (or NRPN, depending on the table entry)
    static inline const int delayEgVolumeParamIdx =
        findSyntaktParamIndexByName ("Knob A"); //("Amp: Volume");

    // "Track Level"  → CC 95 (or NRPN, depending on the table entry)
    static inline const int delayEgTrackLvlParamIdx =
        findSyntaktParamIndexByName ("Track Level");


    // Throttle state for outgoing MIDI
    std::unordered_map<int, int>    lastSentValuePerParam;
    std::unordered_map<int, double> lastSendTimePerParam;

    // Scope (shared audio->UI)
    std::array<std::atomic<float>, maxRoutes> scopeValues { 0.0f, 0.0f, 0.0f };
    std::array<std::atomic<bool>,  maxRoutes> scopeRoutesEnabled { false, false, false };

    static inline const juce::StringArray SyntaktParameterEG = []()
    {
        juce::StringArray filteredEgDest;
        for (int i = 0; i < (int) juce::numElementsInArray(syntaktParameters); ++i)
        {
            if (syntaktParameters[i].egDestination)
                filteredEgDest.add(syntaktParameters[i].name);
        }
        return filteredEgDest;
    }();
    
    // lerp (math) = "Calculates a number between two numbers at a specific increment"
    static inline double lerp(double a, double b, double t) noexcept
    {
        return a + (b - a) * t;
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
            "lfoRateHz",
            "LFO Rate",
            juce::NormalisableRange<float>(0.01f, 40.0f, 0.01f, 0.5f),
            1.0f,
            "Hz",
            juce::AudioProcessorParameter::genericParameter,
            [](float v, int)
            {
                return juce::String(v, 2);   // value → text
            },
            [](const juce::String& text)
            {
                return text.getFloatValue(); // text → value
            }
        ));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "lfoDepth", "LFO Depth",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0f, 1.0f),
            1.0f,
            "",
            juce::AudioProcessorParameter::genericParameter,
            [](float v, int)
            {
                return juce::String(v, 2);   // value → text
            },
            [](const juce::String& text)
            {
                return text.getFloatValue(); // text → value
            }
        ));

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

        // LFO Route channel choices
        auto makeLFOChannelChoices = []()
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
                makeLFOChannelChoices(),
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
            "egAttack", "EG Attack",
            juce::NormalisableRange<float>(0.0005f, 10.0f, 0.0f, 0.40f),
            0.01f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egHold", "EG Hold",
            juce::NormalisableRange<float>(0.0f, 5.0f),
            0.0f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egDecay", "EG Decay",
            juce::NormalisableRange<float>(0.001f, 10.0f, 0.0f, 0.45f),
            0.20f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egSustain", "EG Sustain",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.70f));

        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "egRelease", "EG Release",
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

        // EG ROUTES (maxEGRoutes)

        p.push_back (std::make_unique<juce::AudioParameterBool>("egToLfoDepth", "EG to LFO Depth", false));
        p.push_back (std::make_unique<juce::AudioParameterBool>("egToLfoRate", "EG to LFO Rate", false));

        auto makeEGChannelChoices = []()
        {
            juce::StringArray s;
            s.add("Disabled");
            // s.add("LFO");
            for (int ch = 1; ch <= 16; ++ch)
                s.add("Ch " + juce::String(ch));
            return s;
        };

        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);

            // 0 = Disabled, 1..16 = Ch1..Ch16  (Choice index)
            p.push_back (std::make_unique<juce::AudioParameterChoice>(
                "egRoute" + rs + "_channel",
                "EG Route " + rs + " Channel",
                makeEGChannelChoices(),
                0   // default: all eg routes Disabled
            ));

            // Destination choice is Ch_x: uses SyntaktParameterEG (EG destinations )
            //ModzTaktAudioProcessor::findFirstEgDestination()
            p.push_back (std::make_unique<juce::AudioParameterChoice>(
                "egRoute" + rs + "_dest",
                "EG Route " + rs + " Destination",
                SyntaktParameterEG,
                0 
            ));
        }

        // DELAY
        // Master switch
        p.push_back (std::make_unique<juce::AudioParameterBool>(
            "delayEnabled", "Delay Enabled", false));

        // Input note filtering for Delay (0=Disabled, 1..16 channels)
        p.push_back (std::make_unique<juce::AudioParameterInt>(
            "delayNoteSourceChannel", "Delay Note Source Channel",
            1, 16, 1));

        // Delay sync division  (0 = Free → use slider, 1..8 = BPM-locked intervals)
        // Mirrors the LFO syncDivision choices, with "Free" prepended.
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "delaySyncDivision", "Delay Sync Division",
            juce::StringArray { "Free",
                                "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
                                "1/8 dot", "1/16 dot" },
            0  // default: Free
        ));

        // Delay time  50 ms → 2 000 ms  (sqrt-ish skew so the middle of the
        // slider sits around 500 ms, matching the slider in DelayEditorComponent)
        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "delayRate", "Delay Time",
            juce::NormalisableRange<float>(50.0f, 2000.0f, 1.0f, 0.45f),
            500.0f));

        // Feedback  0 → 95 %  (capped below 1.0 to prevent infinite echoes)
        p.push_back (std::make_unique<juce::AudioParameterFloat>(
            "feedback", "Feedback",
            juce::NormalisableRange<float>(0.0f, 0.95f),
            0.5f));

        // EG shaping destination for delay echoes.
        // 0 = Off, 1 = EG Volume ("Amp: Volume"), 2 = EG Track Level ("Track Level").
        // Independent of the per-note EG flag below.
        p.push_back (std::make_unique<juce::AudioParameterChoice>(
            "delayEgShape", "Delay EG Shape",
            juce::StringArray { "Off", "EG Volume", "EG Track Level" },
            0   // default: Off
        ));

        // Per-note EG: each echo retriggers its own embedded EG (reuses main EG params).
        // Can be active simultaneously with any delayEgShape value — they are orthogonal.
        p.push_back (std::make_unique<juce::AudioParameterBool>(
            "delayEgPerNote", "Delay EG Per Note", false));

        // Delay output routes  (one MIDI-channel selector per route)
        auto makeDelayChannelChoices = []()
        {
            juce::StringArray s;
            s.add ("Disabled");
            for (int ch = 1; ch <= 16; ++ch)
                s.add ("Ch " + juce::String (ch));
            return s;
        };

        for (int r = 0; r < maxRoutes; ++r)
        {
            p.push_back (std::make_unique<juce::AudioParameterChoice>(
                "delayRoute" + juce::String (r) + "_channel",
                "Delay Route " + juce::String (r) + " Channel",
                makeDelayChannelChoices(),
                0  // default: Disabled
            ));

            // Semitone transpose per route  (-24 .. +24, default 0)
            p.push_back (std::make_unique<juce::AudioParameterInt>(
                "delayRoute" + juce::String (r) + "_transpose",
                "Delay Route " + juce::String (r) + " Transpose",
                -24, 24, 0
            ));
        }


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