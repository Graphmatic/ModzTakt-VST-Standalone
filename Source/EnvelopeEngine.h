#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>

namespace modztakt::eg
{
    enum class AttackMode { Fast = 0, Long = 1, Snap = 2 };
    enum class CurveShape { Linear = 0, Exponential = 1, Logarithmic = 2 };

    struct Params
    {
        bool      enabled = false;

        // “UI values” are stored as seconds in your original sliders
        double    attackSeconds  = 0.01;
        double    holdSeconds    = 0.0;
        double    decaySeconds   = 0.2;
        double    sustain01      = 0.7;
        double    releaseSeconds = 0.2;

        double    velocityAmount01 = 0.0; // 0..1

        AttackMode attackMode = AttackMode::Fast;
        bool       releaseLongMode = false;

        CurveShape decayCurveMode   = CurveShape::Exponential;
        CurveShape releaseCurveMode = CurveShape::Exponential;
    };

    struct State
    {
        enum class Stage { Idle, Attack, Hold, Decay, Sustain, Release };

        Stage  stage = Stage::Idle;
        double currentValue = 0.0;

        double stageStartMs = 0.0;
        double stageStartValue = 0.0;

        bool   noteHeld = false;

        // Velocity->peak logic (same as your code)
        double velocity = 1.0;       // 0..1
        double attackPeak = 1.0;     // computed per note
        bool   attackPeakComputed = false;

        // Used for deterministic time in processBlock
        double nowMs = 0.0;
    };

    // -------------------- helpers from your original code --------------------

    inline double computeAttackPeak(double velocity01, double velAmount01)
    {
        return juce::jlimit(0.0, 1.0,
                            juce::jmap(velAmount01, 0.0, 1.0, 1.0, velocity01));
    }

    inline double shapeCurve(double t, CurveShape mode, double k)
    {
        t = juce::jlimit(0.0, 1.0, t);
        if (mode == CurveShape::Linear || k <= 0.0)
            return t;

        const double p = 1.0 + 5.0 * k;

        if (mode == CurveShape::Exponential)
            return std::pow(t, p);             // slow start, fast end
        else
            return 1.0 - std::pow(1.0 - t, p); // fast start, slow end
    }

    inline double attackMsFromSlider(double seconds, AttackMode mode)
    {
        // Your original: slider is in seconds, then mode multipliers
        switch (mode)
        {
            case AttackMode::Fast: return seconds * 1000.0;
            case AttackMode::Long: return seconds * 1000.0 * 3.0;
            case AttackMode::Snap: return seconds * 1000.0 * 0.3;
        }
        return seconds * 1000.0;
    }

    inline double holdSliderToMs(double seconds)   { return seconds * 1000.0; }
    inline double decaySliderToMs(double seconds)  { return seconds * 1000.0; }

    inline double releaseSliderToMs(double seconds, bool releaseLongMode)
    {
        double s = seconds;
        if (releaseLongMode)
            s *= 3.0;
        return s * 1000.0;
    }

    // -------------------- the engine --------------------

    class Engine
    {
    public:
        void setSampleRate(double sr)
        {
            sampleRate = juce::jmax(1.0, sr);
            msPerSample = 1000.0 / sampleRate;
        }

        void setParams(const Params& newParams)
        {
            params = newParams;
        }

        const Params& getParams() const noexcept { return params; }
        const State&  getState()  const noexcept { return state; }

        // Called by processor when a note-on is received (already channel-filtered by processor)
        void noteOn(float vel)
        {
            if (!params.enabled)
                return;

            state.velocity = juce::jlimit(0.0, 1.0, (double)vel);
            state.attackPeakComputed = false;

            state.stage = State::Stage::Attack;
            state.stageStartMs = state.nowMs;
            state.stageStartValue = state.currentValue;
            state.noteHeld = true;
        }

        void noteOff()
        {
            if (!params.enabled)
                return;

            state.stage = State::Stage::Release;
            state.stageStartMs = state.nowMs;
            state.stageStartValue = state.currentValue;
            state.noteHeld = false;
        }

        // Advance engine by one audio block. Returns true if outValue01 is valid.
        bool processBlock(int numSamples, double& outValue01)
        {
            if (!params.enabled)
                return false;

            // Deterministic time advance
            state.nowMs += (double) numSamples * msPerSample;

            if (!advanceEnvelope(state))
                return false;

            outValue01 = juce::jlimit(0.0, 1.0, state.currentValue);
            return true;
        }

        void reset()
        {
            state = {};
            state.nowMs = 0.0;
        }

    private:
        bool advanceEnvelope(State& eg)
        {
            constexpr double epsilon = 0.001;

            const double attackMs  = attackMsFromSlider(params.attackSeconds, params.attackMode);
            const double holdMs    = holdSliderToMs(params.holdSeconds);
            const double decayMs   = decaySliderToMs(params.decaySeconds);
            const double releaseMs = releaseSliderToMs(params.releaseSeconds, params.releaseLongMode);
            const double sustain   = juce::jlimit(0.0, 1.0, params.sustain01);
            const double velAmt    = juce::jlimit(0.0, 1.0, params.velocityAmount01);

            const double nowMs = eg.nowMs;
            const double elapsed = nowMs - eg.stageStartMs;

            switch (eg.stage)
            {
                case State::Stage::Idle:
                    eg.currentValue = 0.0;
                    return false;

                case State::Stage::Attack:
                {
                    if (!eg.attackPeakComputed)
                    {
                        eg.attackPeak = computeAttackPeak(eg.velocity, velAmt);
                        eg.attackPeakComputed = true;
                    }

                    if (attackMs <= epsilon)
                    {
                        eg.currentValue = eg.attackPeak;
                    }
                    else
                    {
                        double t = juce::jlimit(0.0, 1.0, elapsed / attackMs);

                        if (params.attackMode == AttackMode::Snap)
                        {
                            constexpr double snapAmount = 6.0;
                            t = 1.0 - std::exp(-snapAmount * t);
                        }

                        eg.currentValue = eg.stageStartValue + (eg.attackPeak - eg.stageStartValue) * t;
                    }

                    if (elapsed >= attackMs || eg.currentValue >= (eg.attackPeak - 0.0001))
                    {
                        eg.currentValue = eg.attackPeak;
                        eg.stageStartMs = nowMs;
                        eg.stageStartValue = eg.attackPeak;

                        eg.stage = (holdMs > epsilon) ? State::Stage::Hold : State::Stage::Decay;
                    }
                    return true;
                }

                case State::Stage::Hold:
                {
                    eg.currentValue = eg.attackPeak;
                    if (elapsed >= holdMs)
                    {
                        eg.stage = State::Stage::Decay;
                        eg.stageStartMs = nowMs;
                        eg.stageStartValue = eg.attackPeak;
                    }
                    return true;
                }

                case State::Stage::Decay:
                {
                    const double actualSustainLevel = sustain * eg.attackPeak;

                    if (decayMs <= epsilon)
                    {
                        eg.currentValue = actualSustainLevel;
                        eg.stage = State::Stage::Sustain;
                        eg.stageStartMs = nowMs;
                        eg.stageStartValue = actualSustainLevel;
                    }
                    else
                    {
                        const double t = juce::jlimit(0.0, 1.0, elapsed / decayMs);

                        double kDecay = 0.0;
                        if (params.decayCurveMode == CurveShape::Exponential) kDecay = 0.30;
                        else if (params.decayCurveMode == CurveShape::Logarithmic) kDecay = 0.45;

                        const double shapedT = shapeCurve(t, params.decayCurveMode, kDecay);
                        eg.currentValue = eg.stageStartValue + (actualSustainLevel - eg.stageStartValue) * shapedT;

                        if (elapsed >= decayMs)
                        {
                            eg.currentValue = actualSustainLevel;
                            eg.stage = State::Stage::Sustain;
                            eg.stageStartMs = nowMs;
                            eg.stageStartValue = actualSustainLevel;
                        }
                    }
                    return true;
                }

                case State::Stage::Sustain:
                {
                    eg.currentValue = sustain * eg.attackPeak;

                    if (!eg.noteHeld)
                    {
                        eg.stage = State::Stage::Release;
                        eg.stageStartMs = nowMs;
                        eg.stageStartValue = eg.currentValue;
                    }
                    return true;
                }

                case State::Stage::Release:
                {
                    if (releaseMs <= epsilon)
                    {
                        eg.currentValue = 0.0;
                        eg.stage = State::Stage::Idle;
                    }
                    else
                    {
                        const double t = juce::jlimit(0.0, 1.0, elapsed / releaseMs);

                        double kRelease = 0.0;
                        if (params.releaseCurveMode == CurveShape::Exponential) kRelease = 0.35;
                        else if (params.releaseCurveMode == CurveShape::Logarithmic) kRelease = 0.50;

                        const double shapedT = shapeCurve(t, params.releaseCurveMode, kRelease);
                        eg.currentValue = eg.stageStartValue * (1.0 - shapedT);

                        if (elapsed >= releaseMs || eg.currentValue <= 0.0001)
                        {
                            eg.currentValue = 0.0;
                            eg.stage = State::Stage::Idle;
                        }
                    }
                    return true;
                }
            }

            return false;
        }

        Params params;
        State  state;

        double sampleRate = 48000.0;
        double msPerSample = 1000.0 / 48000.0;
    };
} // namespace modztakt::eg
