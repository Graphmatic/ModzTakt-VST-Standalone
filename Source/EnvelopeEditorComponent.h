#pragma once
#include <JuceHeader.h>

#include "Cosmetic.h"
#include "SyntaktParameterTable.h"

class EnvelopeEditorComponent : public juce::Component, private juce::Timer
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;
    using SliderAttachment   = APVTS::SliderAttachment;
    using ButtonAttachment   = APVTS::ButtonAttachment;
    using ChoiceAttachment   = APVTS::ComboBoxAttachment;


    EnvelopeEditorComponent (APVTS& apvtsRef)
        : apvts(apvtsRef)
    {
        setName("Envelope");

        addAndMakeVisible(egGroup);
        egGroup.setText("EG");
        egGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::white);
        egGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::white);

        // --- Enabled
        egEnable = std::make_unique<LedToggleButton>("EG", SetupUI::LedColour::Red);

        egEnable->onClick = [this]{ 
                const bool egIsEnabled = egEnable->getToggleState();

                if (egIsEnabled)
                    egEnableLabel.setText ("Enabled", juce::dontSendNotification);
                else
                    egEnableLabel.setText ("Disabled", juce::dontSendNotification);
        };
        addAndMakeVisible(*egEnable);
        //apvts
        egEnableAttach = std::make_unique<ButtonAttachment>(apvts, "egEnabled", *egEnable);

        egEnableLabel.setText ("Disabled", juce::dontSendNotification);
        egEnableLabel.setJustificationType (juce::Justification::centredLeft);
        egEnableLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(egEnableLabel);

        // ---- MIDI note source channel
        noteSourceEgChannelLabel.setText("Note Source", juce::dontSendNotification);
        addAndMakeVisible(noteSourceEgChannelLabel);
        addAndMakeVisible(noteSourceEgChannelBox);
        for (int ch = 1; ch <= 16; ++ch)
            noteSourceEgChannelBox.addItem("Ch " + juce::String(ch), ch);

        noteSourceChannelAttach = std::make_unique<ChoiceAttachment>(apvts, "egNoteSourceChannel", noteSourceEgChannelBox);

        // ---- EG Out channel + destination
        midiChannelLabel.setText("Dest. Channel", juce::dontSendNotification);
        addAndMakeVisible(midiChannelLabel);
        addAndMakeVisible(midiChannelBox);
        for (int ch = 1; ch <= 16; ++ch)
            midiChannelBox.addItem("Ch " + juce::String(ch), ch);

        egOutChannelAttach = std::make_unique<ChoiceAttachment>(apvts, "egOutChannel", midiChannelBox);

        destinationLabel.setText("Dest. CC", juce::dontSendNotification);
        addAndMakeVisible(destinationLabel);
        addAndMakeVisible(destinationBox);

        populateEgDestinationBox();
        //apvts
        egDestAttach = std::make_unique<ChoiceAttachment>(apvts, "egDestParamIndex", destinationBox);

        // ---- Sliders 
        // ---- apvts
        attackAttach  = std::make_unique<SliderAttachment>(apvts, "egAttackSec",  attackSlider);
        setupAttackSlider();
        holdAttach    = std::make_unique<SliderAttachment>(apvts, "egHoldSec",    holdSlider);
        setupHoldSlider();
        decayAttach   = std::make_unique<SliderAttachment>(apvts, "egDecaySec",   decaySlider);
        setupDecaySlider();
        sustainAttach = std::make_unique<SliderAttachment>(apvts, "egSustain",    sustainSlider);
        setupSustainSlider();
        releaseAttach = std::make_unique<SliderAttachment>(apvts, "egReleaseSec", releaseSlider);
        setupReleaseSlider();
        velAttach     = std::make_unique<SliderAttachment>(apvts, "egVelAmount",  velocityAmountSlider);
        setupVelocitySlider();


        // ---- Attack mode buttons (3-state choice egAttackMode: 0/1/2)
        attackFast = std::make_unique<LedToggleButton>("Fast", SetupUI::LedColour::Green);
        attackLong = std::make_unique<LedToggleButton>("Long", SetupUI::LedColour::Blue);
        attackSnap = std::make_unique<LedToggleButton>("Snap", SetupUI::LedColour::Purple);

        attackFast->setClickingTogglesState(true);
        attackLong->setClickingTogglesState(true);
        attackSnap->setClickingTogglesState(true);

        addAndMakeVisible(*attackFast);
        addAndMakeVisible(*attackLong);
        addAndMakeVisible(*attackSnap);
        syncChoiceButtons("egAttackMode",  attackFast.get(), attackLong.get(), attackSnap.get());

        attackFast->onClick = [this]{ 
            setChoiceParam("egAttackMode", 0); 
            attackSlider.updateText();
        };
        attackLong->onClick = [this]{ 
            setChoiceParam("egAttackMode", 1); 
            attackSlider.updateText();
        };
        attackSnap->onClick = [this]{ 
            setChoiceParam("egAttackMode", 2); 
            attackSlider.updateText();
        };

        attackFastLabel.setText ("Fast", juce::dontSendNotification);
        attackFastLabel.setJustificationType (juce::Justification::centredLeft);
        attackFastLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(attackFastLabel);

        attackLongLabel.setText ("Long", juce::dontSendNotification);
        attackLongLabel.setJustificationType (juce::Justification::centredLeft);
        attackLongLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(attackLongLabel);

        attackSnapLabel.setText ("Snap", juce::dontSendNotification);
        attackSnapLabel.setJustificationType (juce::Justification::centredLeft);
        attackSnapLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(attackSnapLabel);

        // ---- Release long toggle (bool)
        releaseLong = std::make_unique<LedToggleButton>("Long", SetupUI::LedColour::Blue);
        releaseLong->setButtonText("Long");
        addAndMakeVisible(*releaseLong);
        releaseLongAttach = std::make_unique<ButtonAttachment>(apvts, "egReleaseLong", *releaseLong);
        
        // Add callback to update release slider outline when releaseLong changes
        releaseLong->onClick = [this]() {
            updateReleaseSliderOutline();
            releaseSlider.updateText();
        };

        // ---- Decay curve mode (choice egDecayCurve: 0/1/2)
        decayLinear = std::make_unique<LedToggleButton>("Lin", SetupUI::LedColour::Green);
        decayExpo   = std::make_unique<LedToggleButton>("Exp", SetupUI::LedColour::Orange);
        decayLog    = std::make_unique<LedToggleButton>("Log", SetupUI::LedColour::Purple);

        addAndMakeVisible(*decayLinear);
        addAndMakeVisible(*decayExpo);
        addAndMakeVisible(*decayLog);

        decayLinear->onClick = [this]{ setChoiceParam("egDecayCurve", 0); };
        decayExpo->onClick   = [this]{ setChoiceParam("egDecayCurve", 1); };
        decayLog->onClick    = [this]{ setChoiceParam("egDecayCurve", 2); };


        decayLinearLabel.setText ("Lin", juce::dontSendNotification);
        decayLinearLabel.setJustificationType (juce::Justification::centredLeft);
        decayLinearLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(decayLinearLabel);

        decayExpoLabel.setText ("Exp", juce::dontSendNotification);
        decayExpoLabel.setJustificationType (juce::Justification::centredLeft);
        decayExpoLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(decayExpoLabel);

        decayLogLabel.setText ("Log", juce::dontSendNotification);
        decayLogLabel.setJustificationType (juce::Justification::centredLeft);
        decayLogLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(decayLogLabel);

        // ---- Release curve mode (choice egReleaseCurve: 0/1/2)
        releaseLinear = std::make_unique<LedToggleButton>("Lin", SetupUI::LedColour::Green);
        releaseExpo   = std::make_unique<LedToggleButton>("Exp", SetupUI::LedColour::Orange);
        releaseLog    = std::make_unique<LedToggleButton>("Log", SetupUI::LedColour::Purple);

        addAndMakeVisible(*releaseLinear);
        addAndMakeVisible(*releaseExpo);
        addAndMakeVisible(*releaseLog);

        releaseLinear->onClick = [this]{ setChoiceParam("egReleaseCurve", 0); updateReleaseSliderOutline(); };
        releaseExpo->onClick   = [this]{ setChoiceParam("egReleaseCurve", 1); updateReleaseSliderOutline(); };
        releaseLog->onClick    = [this]{ setChoiceParam("egReleaseCurve", 2); updateReleaseSliderOutline(); };

        releaseLinearLabel.setText ("Lin", juce::dontSendNotification);
        releaseLinearLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLinearLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(releaseLinearLabel);

        releaseExpoLabel.setText ("Exp", juce::dontSendNotification);
        releaseExpoLabel.setJustificationType (juce::Justification::centredLeft);
        releaseExpoLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(releaseExpoLabel);

        releaseLogLabel.setText ("Log", juce::dontSendNotification);
        releaseLogLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLogLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(releaseLogLabel);

        // EG -> LFO cross-mod toggles
        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);

            egToLfoToggles[r] = std::make_unique<LedToggleButton>(
                "EG->LFO " + juce::String(r + 1),
                SetupUI::LedColour::Orange
            );

            addAndMakeVisible(*egToLfoToggles[r]);
            egToLfoToggles[r]->setButtonText("");

            egToLfoLabels[r].setText("EG to LFO route " + juce::String(r + 1), juce::dontSendNotification);
            egToLfoLabels[r].setJustificationType(juce::Justification::centredLeft);
            egToLfoLabels[r].setColour(juce::Label::textColourId, SetupUI::labelsColor);
            addAndMakeVisible(egToLfoLabels[r]);

            egToLfoAttach[r] = std::make_unique<ButtonAttachment>(apvts, "egToLfoRoute" + rs, *egToLfoToggles[r]);
        }

        // Keep LED states in sync with automation/preset changes
        startTimerHz(20);
        
        // Initialize release slider outline based on current releaseLong state
        updateReleaseSliderOutline();
    }

    ~EnvelopeEditorComponent() override
    {
        stopTimer();
        attackAttach.reset(); holdAttach.reset(); decayAttach.reset(); sustainAttach.reset(); releaseAttach.reset(); velAttach.reset();
        egEnableAttach.reset(); noteSourceChannelAttach.reset(); egOutChannelAttach.reset(); egDestAttach.reset();
        releaseLongAttach.reset();
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

        // EG ON/OFF
        auto egEnableRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox egEnableFlex;
        egEnableFlex.flexDirection = juce::FlexBox::Direction::row;
        egEnableFlex.alignItems    = juce::FlexBox::AlignItems::center;
        egEnableFlex.justifyContent= juce::FlexBox::JustifyContent::center;

        egEnableFlex.items.add(juce::FlexItem(*egEnable)
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
        egEnableFlex.items.add(juce::FlexItem(egEnableLabel)
                                                .withWidth(100)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        
        egEnableFlex.performLayout(egEnableRow);

        content.removeFromTop(20);

        // ---- routing rows ----
        placeRow(noteSourceEgChannelLabel, noteSourceEgChannelBox);

        content.removeFromTop(20);

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

        // routing rows
        placeRow(midiChannelLabel, midiChannelBox);  
        placeRow(destinationLabel, destinationBox);

        content.removeFromTop(30);

        // EG -> LFO cross-mod rows
        for (int r = 0; r < maxRoutes; ++r)
        {
            auto egToLfoRoute = content.removeFromTop(rowHeight + 4);

            juce::FlexBox egToLfoRouteFlex;
            egToLfoRouteFlex.flexDirection = juce::FlexBox::Direction::row;
            egToLfoRouteFlex.alignItems    = juce::FlexBox::AlignItems::flexStart;
            egToLfoRouteFlex.justifyContent= juce::FlexBox::JustifyContent::flexStart;

            egToLfoRouteFlex.items.add(juce::FlexItem(*egToLfoToggles[r])
                                                .withWidth(22)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 4, 0, 0 }));
            egToLfoRouteFlex.items.add(juce::FlexItem(egToLfoLabels[r])
                                                .withWidth(100)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));

            egToLfoRouteFlex.performLayout(egToLfoRoute);

        }
        //    placeRow(egToLfoLabels[r], *egToLfoToggles[r]);

        

    }

private:
    void timerCallback() override
    {
        syncChoiceButtons("egAttackMode",  attackFast.get(), attackLong.get(), attackSnap.get());
        syncChoiceButtons("egDecayCurve",  decayLinear.get(), decayExpo.get(), decayLog.get());
        syncChoiceButtons("egReleaseCurve", releaseLinear.get(), releaseExpo.get(), releaseLog.get());

        // Update attack slider look based on long mode
        const int idxAttack = (int) apvts.getRawParameterValue("egAttackMode")->load();

        switch (idxAttack)
        {
        case 0:
            attackSlider.setLookAndFeel(&lookGreen);
            break;
        case 1:
            attackSlider.setLookAndFeel(&lookBlue);
            break;
        case 2:
            attackSlider.setLookAndFeel(&lookPurple);
            break;
        }
        attackSlider.repaint();


        // Update decay slider look based on long mode
        const int idxDecay = (int) apvts.getRawParameterValue("egDecayCurve")->load();

        switch (idxDecay)
        {
        case 0:
            decaySlider.setLookAndFeel(&lookGreen);
            break;
        case 1:
            decaySlider.setLookAndFeel(&lookOrange);
            break;
        case 2:
            decaySlider.setLookAndFeel(&lookPurple);
            break;
        }
        decaySlider.repaint();

        // Update release slider look based on long mode
        const int idxRelease = (int) apvts.getRawParameterValue("egReleaseCurve")->load();

        switch (idxRelease)
        {
        case 0:
            releaseSlider.setLookAndFeel(&lookGreen);
            break;
        case 1:
            releaseSlider.setLookAndFeel(&lookOrange);
            break;
        case 2:
            releaseSlider.setLookAndFeel(&lookPurple);
            break;
        }
        releaseSlider.repaint();

        // Update release slider outline based on releaseLong parameter
        updateReleaseSliderOutline();
    }

    void setChoiceParam(const char* paramID, int choiceIndex)
    {
        if (auto* p = apvts.getParameter(paramID))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost((float) choiceIndex / (float) juce::jmax(1, p->getNumSteps() - 1));
            p->endChangeGesture();
        }
    }

    void syncChoiceButtons(const char* paramID, juce::Button* a, juce::Button* b, juce::Button* c)
    {
        const int idx = (int) apvts.getRawParameterValue(paramID)->load();

        if (a) a->setToggleState(idx == 0, juce::dontSendNotification);
        if (b) b->setToggleState(idx == 1, juce::dontSendNotification);
        if (c) c->setToggleState(idx == 2, juce::dontSendNotification);
    }

    void updateReleaseSliderOutline()
    {
        // Get the current release curve mode to determine which LookAndFeel is active
        const int idxRelease = (int) apvts.getRawParameterValue("egReleaseCurve")->load();
        const bool releaseLongMode = apvts.getRawParameterValue("egReleaseLong")->load() > 0.5f;
        
        // Clear outlines from all possible LookAndFeels first
        lookGreen.clearSliderOutline(&releaseSlider);
        lookOrange.clearSliderOutline(&releaseSlider);
        lookPurple.clearSliderOutline(&releaseSlider);
        
        // Add outline to the currently active LookAndFeel if releaseLong is active
        if (releaseLongMode)
        {
            switch (idxRelease)
            {
            case 0:  // Linear - using lookGreen
                lookGreen.setSliderOutline(&releaseSlider, SetupUI::sliderTrackBlue, 1.3f);
                break;
            case 1:  // Exponential - using lookOrange
                lookOrange.setSliderOutline(&releaseSlider, SetupUI::sliderTrackBlue, 1.3f);
                break;
            case 2:  // Logarithmic - using lookPurple
                lookPurple.setSliderOutline(&releaseSlider, SetupUI::sliderTrackBlue, 1.3f);
                break;
            }
        }
        
        releaseSlider.repaint();
    }

    void populateEgDestinationBox()
    {
        destinationBox.clear();
        
        // Change .size() to numElementsInArray
        for (int globalIdx = 0; globalIdx < juce::numElementsInArray(syntaktParameters); ++globalIdx)
        {
            if (syntaktParameters[globalIdx].egDestination)
            {
                destinationBox.addItem(syntaktParameters[globalIdx].name, globalIdx + 1);
            }
        }
    }

    void setupAttackSlider()
    {
        addAndMakeVisible(attackSlider);
        addAndMakeVisible(attackLabel);
        attackLabel.setText("Attack", juce::dontSendNotification);

        attackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        attackSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        attackSlider.setLookAndFeel(&lookGreen);
        attackSlider.setNumDecimalPlacesToDisplay(2);

        attackSlider.setNormalisableRange(juce::NormalisableRange<double>(0.0005, 10.0, 0.0, 0.4));

        attackSlider.textFromValueFunction = [this](double value) -> juce::String
        {
            // Display depends on egAttackMode
            const int mode = (int) apvts.getRawParameterValue("egAttackMode")->load();
            double actualMs = 0.0;

            if (mode == 0) actualMs = value * 1000.0;
            if (mode == 1) actualMs = value * 1000.0 * 3.0;
            if (mode == 2) actualMs = value * 1000.0 * 0.3;

            if (actualMs < 1000.0) return juce::String(actualMs, 1) + " ms";
            return juce::String(actualMs / 1000.0, 2) + " s";
        };
        attackSlider.updateText();
    }

    void setupHoldSlider()
    {
        addAndMakeVisible(holdSlider);
        addAndMakeVisible(holdLabel);
        holdLabel.setText("Hold", juce::dontSendNotification);

        holdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        holdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        holdSlider.setLookAndFeel(&lookGreen);
        holdSlider.setNumDecimalPlacesToDisplay(2);

        holdSlider.setNormalisableRange(juce::NormalisableRange<double>(0.0, 5.0));
        holdSlider.textFromValueFunction = [](double value) -> juce::String
        {
            if (value == 0.0) return "Off";
            if (value < 1.0) return juce::String(value * 1000.0, 0) + " ms";
            return juce::String(value, 2) + " s";
        };
        holdSlider.updateText();
    }

    void setupDecaySlider()
    {
        addAndMakeVisible(decaySlider);
        addAndMakeVisible(decayLabel);
        decayLabel.setText("Decay", juce::dontSendNotification);

        decaySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        decaySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        decaySlider.setLookAndFeel(&lookGreen);
        decaySlider.setNumDecimalPlacesToDisplay(2);

        decaySlider.setNormalisableRange(juce::NormalisableRange<double>(0.001, 10.0, 0.0, 0.45));
        decaySlider.textFromValueFunction = [](double value) -> juce::String
        {
            if (value < 1.0) return juce::String(value * 1000.0, 1) + " ms";
            return juce::String(value, 2) + " s";
        };
        decaySlider.updateText();
    }

    void setupSustainSlider()
    {
        addAndMakeVisible(sustainSlider);
        addAndMakeVisible(sustainLabel);
        sustainLabel.setText("Sustain", juce::dontSendNotification);

        sustainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        sustainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        sustainSlider.setLookAndFeel(&lookGreen);
        sustainSlider.setNumDecimalPlacesToDisplay(2);

        sustainSlider.setRange(0.0, 1.0, 0.001);
        sustainSlider.textFromValueFunction = [](double value) -> juce::String
        {
            return juce::String(value * 100.0, 1) + " %";
        };
        sustainSlider.updateText();
    }

    void setupReleaseSlider()
    {
        addAndMakeVisible(releaseSlider);
        addAndMakeVisible(releaseLabel);
        releaseLabel.setText("Release", juce::dontSendNotification);

        releaseSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        releaseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        releaseSlider.setLookAndFeel(&lookGreen);
        releaseSlider.setNumDecimalPlacesToDisplay(2);

        releaseSlider.setNormalisableRange(juce::NormalisableRange<double>(0.005, 10.0, 0.0, 0.45));
        releaseSlider.textFromValueFunction = [this](double value) -> juce::String
        {
            double actualValue = value;
            if (apvts.getRawParameterValue("egReleaseLong")->load() > 0.5f)
                actualValue *= 3.0;

            if (actualValue < 1.0) return juce::String(actualValue * 1000.0, 1) + " ms";
            return juce::String(actualValue, 2) + " s";
        };
        releaseSlider.updateText();
    }

    void setupVelocitySlider()
    {
        addAndMakeVisible(velocityAmountSlider);
        addAndMakeVisible(velocityAmountLabel);
        velocityAmountLabel.setText("Vel. Amount", juce::dontSendNotification);

        velocityAmountSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        velocityAmountSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
        velocityAmountSlider.setLookAndFeel(&lookPurple);
        velocityAmountSlider.setNumDecimalPlacesToDisplay(2);

        velocityAmountSlider.setRange(0.0, 1.0, 0.001);
        velocityAmountSlider.textFromValueFunction = [](double value) -> juce::String
        {
            return juce::String(value * 100.0, 1) + " %";
        };

    }

    APVTS& apvts;

    // ---- group
    juce::GroupComponent egGroup;

    // ---- EG On/Off
    juce::Label egEnableLabel;

    // ---- routing
    juce::Label noteSourceEgChannelLabel;
    juce::ComboBox noteSourceEgChannelBox;
    std::unique_ptr<ChoiceAttachment> noteSourceChannelAttach;

    juce::Label midiChannelLabel;
    juce::ComboBox midiChannelBox;
    std::unique_ptr<ChoiceAttachment> egOutChannelAttach;

    juce::Label destinationLabel;
    juce::ComboBox destinationBox;
    std::unique_ptr<ChoiceAttachment> egDestAttach;

    // ---- enable
    std::unique_ptr<LedToggleButton> egEnable;
    std::unique_ptr<ButtonAttachment> egEnableAttach;

    // ---- sliders
    juce::Slider attackSlider, holdSlider, decaySlider, sustainSlider, releaseSlider, velocityAmountSlider;
    juce::Label  attackLabel,  holdLabel,  decayLabel,  sustainLabel,  releaseLabel,  velocityAmountLabel;

    std::unique_ptr<SliderAttachment> attackAttach, holdAttach, decayAttach, sustainAttach, releaseAttach, velAttach;

    // ---- modes
    std::unique_ptr<LedToggleButton> attackFast, attackLong, attackSnap;
    juce::Label attackFastLabel, attackLongLabel, attackSnapLabel;
    std::unique_ptr<LedToggleButton> releaseLong;

    std::unique_ptr<LedToggleButton> decayLinear, decayExpo, decayLog;
    juce::Label decayLinearLabel, decayExpoLabel, decayLogLabel;

    std::unique_ptr<LedToggleButton> releaseLinear, releaseExpo, releaseLog;
    std::unique_ptr<ButtonAttachment> releaseLongAttach;
    juce::Label releaseLinearLabel, releaseExpoLabel, releaseLogLabel, releaseLongLabel;

    // --- EG -> LFO cross-mod UI ---
    static constexpr int maxRoutes = 3;

    std::array<std::unique_ptr<LedToggleButton>, maxRoutes> egToLfoToggles;
    std::array<juce::Label, maxRoutes>                      egToLfoLabels;
    std::array<std::unique_ptr<ButtonAttachment>, maxRoutes> egToLfoAttach;


    // look & feel from your Cosmetic.h
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookDarkGreen  { SetupUI::sliderTrackDarkGreen };
    ModzTaktLookAndFeel lookOrange { SetupUI::sliderTrackOrange };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    ModzTaktLookAndFeel lookBlue { SetupUI::sliderTrackBlue };
};