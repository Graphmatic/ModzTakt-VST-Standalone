#pragma once

#include <JuceHeader.h>
#include "SyntaktParameterTable.h"
#include "Cosmetic.h"

class EnvelopeComponent : public juce::Component
{
public:
    EnvelopeComponent()
    {

        setName("Envelope");

        // Group
        addAndMakeVisible(egGroup);

        egGroup.setText("EG");
        egGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::white);
        egGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::white);

        // ---- MIDI note source channel
        noteSourceEgChannelLabel.setText("Note Source", juce::dontSendNotification);
        addAndMakeVisible(noteSourceEgChannelLabel);

        egEnabled = false;

        for (int ch = 1; ch <= 16; ++ch)
            noteSourceEgChannelBox.addItem("Ch " + juce::String(ch), ch);

        noteSourceEgChannelBox.addItem("Off", 17);

        noteSourceEgChannelBox.setSelectedId(17); // default OFF

        addAndMakeVisible(noteSourceEgChannelBox);

        noteSourceEgChannelBox.onChange = [this]()
        {
            const int id = noteSourceEgChannelBox.getSelectedId();
            noteSourceEgChannel.store(id, std::memory_order_relaxed);
            egEnabled.store(id != 17, std::memory_order_relaxed);
        };

        // ---- MIDI Channel ----
        midiChannelLabel.setText("Dest. Channel", juce::dontSendNotification);
        addAndMakeVisible(midiChannelLabel);

        addAndMakeVisible(midiChannelBox);
        for (int ch = 1; ch <= 16; ++ch)
            midiChannelBox.addItem("Ch " + juce::String(ch), ch);
        midiChannelBox.setSelectedId(1);

        midiChannelBox.onChange = [this]()
        {
            egOutChannel = (midiChannelBox.getSelectedId());
        };

        // ---- Destination ----
        destinationLabel.setText("Dest. CC", juce::dontSendNotification);

        addAndMakeVisible(destinationLabel);

        // Populate EG destinations
        addAndMakeVisible(destinationBox);

        populateEgDestinationBox();
        destinationBox.setSelectedItemIndex(15, juce::dontSendNotification);

        egOutParamsId = (destinationBox.getSelectedId() - 1);

        destinationBox.onChange = [this]()
        {
            egOutParamsId = (destinationBox.getSelectedId() - 1);
        };

        setupSlider(attackSlider, attackLabel, "Attack", juce::Slider::LinearHorizontal);

        constexpr int attackModeGroupId = 1001;

        attackFast = std::make_unique<LedToggleButton>
        (
            "Fast",
            SetupUI::LedColour::Green
        );

        attackLong = std::make_unique<LedToggleButton>
        (
            "Long",
            SetupUI::LedColour::Orange
        );

        attackSnap = std::make_unique<LedToggleButton>
        (
            "Snap",
            SetupUI::LedColour::Green
        );

        attackFast->setRadioGroupId(attackModeGroupId);

        attackFastLabel.setText ("Fast", juce::dontSendNotification);
        attackFastLabel.setJustificationType (juce::Justification::centredLeft);
        attackFastLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        attackLong->setRadioGroupId(attackModeGroupId);

        attackLongLabel.setText ("Long", juce::dontSendNotification);
        attackLongLabel.setJustificationType (juce::Justification::centredLeft);
        attackLongLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        attackSnap->setRadioGroupId(attackModeGroupId);

        attackSnapLabel.setText ("Snap", juce::dontSendNotification);
        attackSnapLabel.setJustificationType (juce::Justification::centredLeft);
        attackSnapLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        attackFast->setToggleState(true, juce::dontSendNotification);

        auto updateAttackMode = [this]()
        {
            if (attackFast->getToggleState())
            {
                attackMode = AttackMode::Fast;
                attackSlider.setLookAndFeel(&lookGreen);
            }
            else if (attackLong->getToggleState())
            {
                attackMode = AttackMode::Long;
                attackSlider.setLookAndFeel(&lookOrange);
            }
            else if (attackSnap->getToggleState())
            {
                attackMode = AttackMode::Snap;
                attackSlider.setLookAndFeel(&lookGreen);
            }

            attackSlider.updateText();
        };

        addAndMakeVisible(*attackFast);
        addAndMakeVisible(attackFastLabel);

        addAndMakeVisible(*attackSnap);
        addAndMakeVisible(attackSnapLabel);

        addAndMakeVisible(*attackLong);
        addAndMakeVisible(attackLongLabel);


        attackFast->onClick = updateAttackMode;
        attackLong->onClick = updateAttackMode;
        attackSnap->onClick = updateAttackMode;

        setupSlider(holdSlider, holdLabel, "Hold", juce::Slider::LinearHorizontal);

        setupSlider(decaySlider, decayLabel, "Decay", juce::Slider::LinearHorizontal);

        constexpr int decayCurveGroupId = 2001;

        decayLinear = std::make_unique<LedToggleButton>
        (
            "Lin",
            SetupUI::LedColour::Green
        );

        decayExpo = std::make_unique<LedToggleButton>
        (
            "Exp",
            SetupUI::LedColour::Green
        );

        decayLog = std::make_unique<LedToggleButton>
        (
            "Log",
            SetupUI::LedColour::Green
        );

        decayLinear->setRadioGroupId(decayCurveGroupId);

        decayLinearLabel.setText ("Lin", juce::dontSendNotification);
        decayLinearLabel.setJustificationType (juce::Justification::centredLeft);
        decayLinearLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        decayExpo->setRadioGroupId(decayCurveGroupId);

        decayExpoLabel.setText ("Exp", juce::dontSendNotification);
        decayExpoLabel.setJustificationType (juce::Justification::centredLeft);
        decayExpoLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        decayLog->setRadioGroupId(decayCurveGroupId);

        decayLogLabel.setText ("Log", juce::dontSendNotification);
        decayLogLabel.setJustificationType (juce::Justification::centredLeft);
        decayLogLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        decayExpo->setToggleState(true, juce::dontSendNotification);

        auto updateDecayCurve = [this]()
        {
            if (decayLinear->getToggleState())      decayCurveMode = CurveShape::Linear;
            else if (decayExpo->getToggleState())   decayCurveMode = CurveShape::Exponential;
            else if (decayLog->getToggleState())    decayCurveMode = CurveShape::Logarithmic;
        };

        decayLinear->onClick = updateDecayCurve;
        decayExpo->onClick   = updateDecayCurve;
        decayLog->onClick    = updateDecayCurve;

        addAndMakeVisible(*decayLinear);
        addAndMakeVisible(decayLinearLabel);

        addAndMakeVisible(*decayExpo);
        addAndMakeVisible(decayExpoLabel);

        addAndMakeVisible(*decayLog);
        addAndMakeVisible(decayLogLabel);

        setupSlider(sustainSlider, sustainLabel, "Sustain", juce::Slider::LinearHorizontal);

        setupSlider(releaseSlider, releaseLabel, "Release", juce::Slider::LinearHorizontal);

        constexpr int releaseCurveGroupId = 2002;

        releaseLinear = std::make_unique<LedToggleButton>
        (
            "Lin",
            SetupUI::LedColour::Green
        );

        releaseExpo = std::make_unique<LedToggleButton>
        (
            "Exp",
            SetupUI::LedColour::Green
        );

        releaseLog = std::make_unique<LedToggleButton>
        (
            "Log",
            SetupUI::LedColour::Green
        );

        releaseLinear->setRadioGroupId(releaseCurveGroupId);

        releaseLinearLabel.setText ("Lin", juce::dontSendNotification);
        releaseLinearLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLinearLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        releaseExpo->setRadioGroupId(releaseCurveGroupId);

        releaseExpoLabel.setText ("Exp", juce::dontSendNotification);
        releaseExpoLabel.setJustificationType (juce::Justification::centredLeft);
        releaseExpoLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        releaseLog->setRadioGroupId(releaseCurveGroupId);

        releaseLogLabel.setText ("Log", juce::dontSendNotification);
        releaseLogLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLogLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        releaseExpo->setToggleState(true, juce::dontSendNotification);

        auto updateReleaseCurve = [this]()
        {
            if (releaseLinear->getToggleState())
            {
                releaseCurveMode = CurveShape::Linear;
                if (releaseLong->getToggleState())
                {
                    releaseSlider.setLookAndFeel(&lookDarkGreen);
                }
                else
                {
                    releaseSlider.setLookAndFeel(&lookGreen);
                }
            }     
            else if (releaseExpo->getToggleState())
            {
                releaseCurveMode = CurveShape::Exponential;
                if (releaseLong->getToggleState())
                {
                    releaseSlider.setLookAndFeel(&lookDarkGreen);
                }
                else
                {
                    releaseSlider.setLookAndFeel(&lookGreen);
                }
            }
            else if (releaseLog->getToggleState()) 
            {
                releaseCurveMode = CurveShape::Logarithmic;
                if (releaseLong->getToggleState())
                {
                    releaseSlider.setLookAndFeel(&lookDarkGreen);
                }
                else
                {
                    releaseSlider.setLookAndFeel(&lookGreen);
                }
            }
        };

        releaseLinear->onClick = updateReleaseCurve;
        releaseExpo->onClick   = updateReleaseCurve;
        releaseLog->onClick    = updateReleaseCurve;

        addAndMakeVisible(*releaseLinear);
        addAndMakeVisible(releaseLinearLabel);

        addAndMakeVisible(*releaseExpo);
        addAndMakeVisible(releaseExpoLabel);

        addAndMakeVisible(*releaseLog);
        addAndMakeVisible(releaseLogLabel);

        releaseLong = std::make_unique<LedToggleButton>
        (
            "Long",
            SetupUI::LedColour::Orange
        );

        releaseLongLabel.setText ("Long", juce::dontSendNotification);
        releaseLongLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLongLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        addAndMakeVisible(*releaseLong);
        addAndMakeVisible(releaseLongLabel);


        releaseLong->onClick = [this]()
        {
            releaseLongMode = releaseLong->getToggleState();
            if (releaseLongMode)
            {
                releaseSlider.setLookAndFeel(&lookDarkGreen);
            }
            else
            {
                releaseSlider.setLookAndFeel(&lookGreen);
            }
            releaseSlider.updateText();
        };

        setupSlider(velocityAmountSlider, velocityAmountLabel, "Vel. Amount", juce::Slider::LinearHorizontal);

    }

    ~EnvelopeComponent() override
    {
        attackSlider.setLookAndFeel (nullptr);
        holdSlider.setLookAndFeel (nullptr);
        decaySlider.setLookAndFeel (nullptr);
        sustainSlider.setLookAndFeel (nullptr);
        releaseSlider.setLookAndFeel (nullptr);
        velocityAmountSlider.setLookAndFeel (nullptr);
    }

    void resized() override
    {
        if (getWidth() <= 0 || getHeight() <= 0)
            return;

        auto area = getLocalBounds();
        egGroup.setBounds(area);

        auto content = area.reduced(10, 24);

        constexpr int rowHeight  = 24;
        constexpr int labelWidth = 90;
        constexpr int spacing    = 6;

        auto placeRow = [&](juce::Label& label, juce::Component& comp)
        {
            auto row = content.removeFromTop(rowHeight);
            label.setBounds(row.removeFromLeft(labelWidth));
            row.removeFromLeft(spacing);
            comp.setBounds(row);
            content.removeFromTop(6);
        };

        // ---- routing rows ----
        placeRow(noteSourceEgChannelLabel, noteSourceEgChannelBox);

        content.removeFromTop(10);

        // ---- ADSR ----
        placeRow(attackLabel, attackSlider);

        auto attackOptionsRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox attackOptions;
        attackOptions.flexDirection = juce::FlexBox::Direction::row;
        attackOptions.alignItems    = juce::FlexBox::AlignItems::flexStart;
        attackOptions.justifyContent= juce::FlexBox::JustifyContent::flexStart;

        attackOptions.items.add(juce::FlexItem(*attackSnap)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        attackOptions.items.add(juce::FlexItem(attackSnapLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        attackOptions.items.add(juce::FlexItem(*attackFast)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        attackOptions.items.add(juce::FlexItem(attackFastLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        attackOptions.items.add(juce::FlexItem(*attackLong)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        attackOptions.items.add(juce::FlexItem(attackLongLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));

        attackOptions.performLayout(attackOptionsRow);

        content.removeFromTop(15);

        placeRow(holdLabel, holdSlider);

        content.removeFromTop(15);

        placeRow(decayLabel, decaySlider);

        auto decayCurveRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox decayCurveBox;
        decayCurveBox.flexDirection  = juce::FlexBox::Direction::row;
        decayCurveBox.alignItems     = juce::FlexBox::AlignItems::flexStart;
        decayCurveBox.justifyContent = juce::FlexBox::JustifyContent::flexStart;

        decayCurveBox.items.add(juce::FlexItem(*decayLinear)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        decayCurveBox.items.add(juce::FlexItem(decayLinearLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        decayCurveBox.items.add(juce::FlexItem(*decayExpo)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        decayCurveBox.items.add(juce::FlexItem(decayExpoLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        decayCurveBox.items.add(juce::FlexItem(*decayLog)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        decayCurveBox.items.add(juce::FlexItem(decayLogLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));

        decayCurveBox.performLayout(decayCurveRow);

        content.removeFromTop(15);

        placeRow(sustainLabel, sustainSlider);

        content.removeFromTop(15);

        placeRow(releaseLabel, releaseSlider);

        auto releaseCurveRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox releaseCurveBox;
        releaseCurveBox.flexDirection  = juce::FlexBox::Direction::row;
        releaseCurveBox.alignItems     = juce::FlexBox::AlignItems::flexStart;
        releaseCurveBox.justifyContent = juce::FlexBox::JustifyContent::flexStart;

        releaseCurveBox.items.add(juce::FlexItem(*releaseLinear)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(releaseLinearLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(*releaseExpo)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(releaseExpoLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(*releaseLog)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(releaseLogLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(*releaseLong)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        releaseCurveBox.items.add(juce::FlexItem(releaseLongLabel)
                                                .withWidth(50)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));

        releaseCurveBox.performLayout(releaseCurveRow);

        content.removeFromTop(20);

        placeRow(velocityAmountLabel, velocityAmountSlider);

        content.removeFromTop(20);

        // ---- routing rows ----
        placeRow(midiChannelLabel, midiChannelBox);  
        placeRow(destinationLabel, destinationBox);

    }

    void parentHierarchyChanged() override
    {
        // Forces initial layout once the component is attached & visible
        resized();
    }

    bool isEgEnabled() const noexcept
    {
        return egEnabled;
    }

    int selectedEgOutChannel() const noexcept
    {
        return egOutChannel;
    }

    int selectedEgOutParamsId() const noexcept
    {
        return egOutParamsId;
    }

    bool tick(double& outMidiValue)
    {
        if (!egEnabled)
            return false;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();

        if (!advanceEnvelope(
                eg,
                nowMs,
                attackMsFromSlider(attackSlider.getValue()),
                holdSliderToMs(holdSlider.getValue()),           // convert to ms
                decaySliderToMs(decaySlider.getValue()),
                sustainSlider.getValue(),
                releaseSliderToMs(releaseSlider.getValue()),
                velocityAmountSlider.getValue()
                )
            )
            return false;

        outMidiValue = juce::jlimit(0.0, 1.0, eg.currentValue);

        return true;
    }

    void noteOn(int ch, int note, float velocity)
    {
        if (ch == noteSourceEgChannel.load(std::memory_order_relaxed))
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            
            // Store velocity and reset peak computation flag
            eg.velocity = juce::jlimit(0.0, 1.0, velocity / 127.0);
            eg.attackPeakComputed = false; // Reset flag so peak will be computed on first tick
            
            eg.stage = EnvelopeState::Stage::Attack;
            eg.stageStartMs = now;
            eg.stageStartValue = eg.currentValue;
            eg.noteHeld = true;
        }     
    }

    void noteOff(int ch, int note)
    {
        if (ch == noteSourceEgChannel.load(std::memory_order_relaxed))
        {
            const double now = juce::Time::getMillisecondCounterHiRes();

            eg.stage = EnvelopeState::Stage::Release;
            eg.stageStartMs = now;
            eg.stageStartValue = eg.currentValue;
            eg.noteHeld = false;
        }
    }

private:
   
    // ==== UI ==========================================================
    // ---- Group ----
    juce::GroupComponent egGroup;

    std::atomic<int> noteSourceEgChannel { 17 }; // default OFF
    std::atomic<bool> egEnabled { false };

    int egOutChannel;
    int egOutParamsId;

    // ---- Routing ----
    juce::Label   noteSourceEgChannelLabel;
    juce::ComboBox noteSourceEgChannelBox; // source channel for Note-On listening
    juce::Label   midiChannelLabel;
    juce::ComboBox midiChannelBox;

    // MIDI Input selected in mainComponent
    int currentMidiInput;

    juce::Label   destinationLabel;
    juce::ComboBox destinationBox;

    // ---- ADSR ----
    juce::Label attackLabel;
    juce::Label holdLabel;
    juce::Label decayLabel;
    juce::Label sustainLabel;
    juce::Label releaseLabel;

    juce::Label velocityAmountLabel;

    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookDarkGreen  { SetupUI::sliderTrackDarkGreen };
    ModzTaktLookAndFeel lookOrange { SetupUI::sliderTrackOrange };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };

    juce::Slider attackSlider, holdSlider, decaySlider, sustainSlider, releaseSlider, velocityAmountSlider;

    enum class AttackMode
    {
        Fast,
        Long,
        Snap
    };

    AttackMode attackMode = AttackMode::Fast;

    std::unique_ptr<LedToggleButton> attackFast, attackLong, attackSnap;
    
    juce::Label attackFastLabel, attackLongLabel, attackSnapLabel;

     //DEBUG
    #if JUCE_DEBUG
    // Debug: show last Note-On received
    juce::Label noteDebugTitle { {}, "Last Note-On:" };
    juce::Label noteDebugLabel;
    #endif

    std::unique_ptr<juce::MidiInput> noteInput;

    // Destination parameters
    std::vector<const SyntaktParameter*> egDestinations;

    std::unique_ptr<juce::MidiInputCallback> noteInputCallback;

    // EG curves
    enum class CurveShape
    {
        Linear,
        Exponential,
        Logarithmic
    };

    CurveShape decayCurveMode   = CurveShape::Exponential; // default
    CurveShape releaseCurveMode = CurveShape::Exponential;

    std::unique_ptr<LedToggleButton> decayLinear, decayExpo, decayLog;
    juce::Label decayLinearLabel, decayExpoLabel, decayLogLabel;

    std::unique_ptr<LedToggleButton> releaseLinear, releaseExpo, releaseLog, releaseLong;
    juce::Label releaseLinearLabel, releaseExpoLabel, releaseLogLabel, releaseLongLabel;

    bool releaseLongMode = false;

    //EG state 
    struct EnvelopeState
    {
        enum class Stage
        {
            Idle,
            Attack,
            Hold,
            Decay,
            Sustain,
            Release
        };

        Stage stage = Stage::Idle;
        double currentValue = 0.0;

        double stageStartMs = 0.0;
        double stageStartValue = 0.0;

        bool noteHeld = false;

        //Velocity to EG
        double velocity = 1.0;       // normalized 0..1
        double attackPeak = 1.0;     // computed per note
        bool attackPeakComputed = false;
    };

    EnvelopeState eg;

    struct EgRoute
    {
        int midiChannel = 1;
        int parameterIndex = -1; // index into syntaktParameters
    };

    // Compute attack peak based on velocity and velocity amount
    inline double computeAttackPeak(double velocity, double velAmount) const
    {
        // velAmount = 0 → peak is always 1.0 (no velocity sensitivity)
        // velAmount = 1 → peak follows velocity exactly
        return juce::jlimit(0.0, 1.0,
            juce::jmap(velAmount, 0.0, 1.0, 1.0, velocity));
    }

    //EG tick function
    bool advanceEnvelope(
                        EnvelopeState& eg,
                        double nowMs,
                        double attackMs,
                        double holdMs,
                        double decayMs,
                        double sustainLevel,
                        double releaseMs,
                        double velocityAmount)
    {
        constexpr double epsilon = 0.001; // 1 microsecond threshold

        auto elapsed = nowMs - eg.stageStartMs;

        switch (eg.stage)
        {
            case EnvelopeState::Stage::Idle:
                eg.currentValue = 0.0;
                return false;

            case EnvelopeState::Stage::Attack:
            {
                // Compute attack peak once at the start of Attack stage
                if (!eg.attackPeakComputed)
                {
                    eg.attackPeak = computeAttackPeak(eg.velocity, velocityAmount);
                    eg.attackPeakComputed = true;
                }
                
                if (attackMs <= epsilon)
                {
                    eg.currentValue = eg.attackPeak;
                }
                else
                {
                    double t = juce::jlimit(0.0, 1.0, elapsed / attackMs);

                    if (attackMode == AttackMode::Snap)
                    {
                        constexpr double snapAmount = 6.0;
                        t = 1.0 - std::exp(-snapAmount * t);
                    }

                    eg.currentValue = eg.stageStartValue + (eg.attackPeak - eg.stageStartValue) * t;
                }

                // Check if we've reached the peak
                if (elapsed >= attackMs || eg.currentValue >= (eg.attackPeak - 0.0001))
                {
                    eg.currentValue = eg.attackPeak;
                    eg.stageStartMs = nowMs;
                    eg.stageStartValue = eg.attackPeak;

                    // Check if hold time is meaningful
                    if (holdMs > epsilon)
                        eg.stage = EnvelopeState::Stage::Hold;
                    else
                        eg.stage = EnvelopeState::Stage::Decay;
                }
                return true;
            }

            case EnvelopeState::Stage::Hold:
            {
                // Hold at attack peak value
                eg.currentValue = eg.attackPeak;

                if (elapsed >= holdMs)
                {
                    eg.stage = EnvelopeState::Stage::Decay;
                    eg.stageStartMs = nowMs;
                    eg.stageStartValue = eg.attackPeak; // Start decay from actual peak
                }
                return true;
            }

            case EnvelopeState::Stage::Decay:
            {
                // Calculate actual sustain level relative to attack peak
                // sustainLevel is 0..1 from slider, scale it to 0..attackPeak
                const double actualSustainLevel = sustainLevel * eg.attackPeak;
                
                if (decayMs <= epsilon)
                {
                    eg.currentValue = actualSustainLevel;
                    eg.stage = EnvelopeState::Stage::Sustain;
                }
                else
                {
                    const double t = juce::jlimit(0.0, 1.0, elapsed / decayMs);

                    double kDecay = 0.0;

                    if (decayCurveMode == CurveShape::Exponential)
                    {
                        kDecay = 0.30;
                    }
                    else if (decayCurveMode == CurveShape::Logarithmic)
                    {
                        kDecay = 0.45;
                    }

                    const double shapedT = shapeCurve(t, decayCurveMode, kDecay);

                    eg.currentValue = eg.stageStartValue + (actualSustainLevel - eg.stageStartValue) * shapedT;

                    if (elapsed >= decayMs)
                    {
                        eg.currentValue = actualSustainLevel;
                        eg.stage = EnvelopeState::Stage::Sustain;
                        eg.stageStartMs = nowMs;
                        eg.stageStartValue = actualSustainLevel;
                    }
                }

                return true;
            }

            case EnvelopeState::Stage::Sustain:
            {
                // Sustain at level relative to attack peak
                eg.currentValue = sustainLevel * eg.attackPeak;

                if (!eg.noteHeld)
                {
                    eg.stage = EnvelopeState::Stage::Release;
                    eg.stageStartMs = nowMs;
                    eg.stageStartValue = eg.currentValue;
                }
                return true;
            }

            case EnvelopeState::Stage::Release:
            {
                if (releaseMs <= epsilon)
                {
                    eg.currentValue = 0.0;
                    eg.stage = EnvelopeState::Stage::Idle;
                }
                else
                {
                    const double t = juce::jlimit(0.0, 1.0, elapsed / releaseMs);

                    double kRelease = 0.0;

                    if (releaseCurveMode == CurveShape::Exponential)
                    {
                        kRelease = 0.35;
                    }
                    else if (releaseCurveMode == CurveShape::Logarithmic)
                    {
                        kRelease = 0.50;
                    }

                    const double shapedT = shapeCurve(t, releaseCurveMode, kRelease);

                    eg.currentValue = eg.stageStartValue * (1.0 - shapedT);

                    if (elapsed >= releaseMs || eg.currentValue <= 0.0001)
                    {
                        eg.currentValue = 0.0;
                        eg.stage = EnvelopeState::Stage::Idle;
                    }
                }

                return true;
            }
        }

        return false;
    }

    inline double shapeCurve(double t, CurveShape mode, double k)
    {
        t = juce::jlimit(0.0, 1.0, t);

        if (mode == CurveShape::Linear || k <= 0.0)
            return t;

          const double p = 1.0 + 5.0 * k;

        if (mode == CurveShape::Exponential)
        {
            // Slow start, fast end
            return std::pow(t, p);
        }
        else // Logarithmic
        {
            // Fast start, slow end
            return 1.0 - std::pow(1.0 - t, p);
        }
    }

    // Convert attack slider value to milliseconds based on mode
    double attackMsFromSlider(double sliderValue) const
    {
        double seconds = sliderValue; // Slider already in seconds (0.0005 to 10.0)

        switch (attackMode)
        {
            case AttackMode::Fast:
                // Use slider value as-is: 0.5ms to 10s
                return seconds * 1000.0;

            case AttackMode::Long:
                // Extend range: multiply by 3 for up to 30s
                return seconds * 1000.0 * 3.0;

            case AttackMode::Snap:
                // Shorter range for snappy attacks: 0.2ms to 3s
                return seconds * 1000.0 * 0.3;
        }

        return sliderValue * 1000.0;
    }

    // Convert hold slider value to milliseconds
    double holdSliderToMs(double sliderValue) const
    {
        // Slider is in seconds (0 to 5.0)
        return sliderValue * 1000.0;
    }

    // Convert decay slider value to milliseconds
    double decaySliderToMs(double sliderValue) const
    {
        // Slider is already in seconds (0.001 to 10.0)
        // Just convert to milliseconds
        return sliderValue * 1000.0;
    }

    // Convert release slider value to milliseconds
    double releaseSliderToMs(double sliderValue) const
    {
        // Slider is in seconds (0.005 to 10.0)
        double seconds = sliderValue;
        
        if (releaseLongMode)
        {
            // Extend to 30 seconds in Long mode
            seconds *= 3.0;
        }
        
        return seconds * 1000.0;
    }

    // ==== Helpers =====================================================
    void setupSlider(juce::Slider& slider, juce::Label& label, const juce::String& name, const juce::Slider::SliderStyle& style)
    {
        addAndMakeVisible(slider);
        addAndMakeVisible(label);

        slider.setSliderStyle(style);

        if (style == juce::Slider::LinearHorizontal)
            slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);

        // if (style == juce::Slider::LinearVertical)
        //     slider.setTextBoxStyle(juce::Slider::TextBoxBottom, false, 60, 20);

        slider.setLookAndFeel(&lookGreen);
        
        label.setText(name, juce::dontSendNotification);

        slider.setNumDecimalPlacesToDisplay(2);

            if (name == "Attack")
            {
                // Range: 0.5ms to 10s with skew for more precision at lower values
                slider.setNormalisableRange(juce::NormalisableRange<double>(0.0005, 10.0, 0.0, 0.4));

                // Display in ms or seconds based on value
                slider.textFromValueFunction = [this](double value) -> juce::String
                {
                    double actualMs = 0.0;
                    
                    switch (attackMode)
                    {
                        case AttackMode::Fast:
                            actualMs = value * 1000.0;
                            break;
                        case AttackMode::Long:
                            actualMs = value * 1000.0 * 3.0;
                            break;
                        case AttackMode::Snap:
                            actualMs = value * 1000.0 * 0.3;
                            break;
                    }
                    
                    if (actualMs < 1000.0)
                        return juce::String(actualMs, 1) + " ms";
                    else
                        return juce::String(actualMs / 1000.0, 2) + " s";
                };
            }
                    
            if (name == "Hold")
            {
                // Range: 0 to 5s, linear
                slider.setNormalisableRange(juce::NormalisableRange<double>(0.0, 5.0));

                slider.textFromValueFunction = [](double value) -> juce::String
                {
                    if (value == 0.0)
                        return "Off";
                    else if (value < 1.0)
                        return juce::String(value * 1000.0, 0) + " ms";
                    else
                        return juce::String(value, 2) + " s";
                };
            }

            if (name == "Decay")
            {
                // Range: 1ms to 10s with skew for more precision at lower values
                slider.setNormalisableRange(juce::NormalisableRange<double>(0.001, 10.0, 0.0, 0.45));

                slider.textFromValueFunction = [](double value) -> juce::String
                {
                    if (value < 1.0)
                        return juce::String(value * 1000.0, 1) + " ms";
                    else
                        return juce::String(value, 2) + " s";
                };
            }
            
            if (name == "Sustain")
            {
                // Range: 0 to 1 (level, not time)
                slider.setRange(0.0, 1.0, 0.001);

                // Display as percentage
                slider.textFromValueFunction = [](double value) -> juce::String
                {
                    return juce::String(value * 100.0, 1) + " %";
                };
            }
            
            if (name == "Release")
            {
                // Range: 5ms to 10s (or 30s with Long mode) with skew
                slider.setNormalisableRange(juce::NormalisableRange<double>(0.005, 10.0, 0.0, 0.45));

                // Note: This shows base value. Long mode multiplier happens in conversion function
                slider.textFromValueFunction = [this](double value) -> juce::String
                {
                    double actualValue = value;
                    if (releaseLongMode)
                        actualValue *= 3.0;
                        
                    if (actualValue < 1.0)
                        return juce::String(actualValue * 1000.0, 1) + " ms";
                    else
                        return juce::String(actualValue, 2) + " s";
                };
            }

            if (name == "Vel. Amount")
            {
                slider.setRange(0.0, 1.0, 0.001);

                // Display as percentage
                slider.textFromValueFunction = [](double value) -> juce::String
                {
                    return juce::String(value * 100.0, 1) + " %";
                };
                slider.setLookAndFeel(&lookPurple);
            }

        label.setJustificationType(juce::Justification::centredLeft);
        label.attachToComponent(&slider, false); // semantic link only

    }

    //Parameter destinations
    void populateEgDestinationBox()
    {
        destinationBox.clear();

        int itemId = 1;

        for (const auto& p : syntaktParameters)
        {
            if (p.egDestination)
            {
                destinationBox.addItem(p.name, itemId);
            }
            ++itemId;
        }

        destinationBox.setSelectedId(15, juce::dontSendNotification); // set a default value
    }
};
