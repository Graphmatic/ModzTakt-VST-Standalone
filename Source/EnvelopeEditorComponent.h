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
        noteSourceEgChannelLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(noteSourceEgChannelLabel);
        addAndMakeVisible(noteSourceEgChannelBox);
        for (int ch = 1; ch <= 16; ++ch)
            noteSourceEgChannelBox.addItem("Ch " + juce::String(ch), ch);

        noteSourceChannelAttach = std::make_unique<ChoiceAttachment>(apvts, "egNoteSourceChannel", noteSourceEgChannelBox);

        // ---- EG Out channel + destination
        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);

            egRouteChannelLabel[r].setText("Route " + juce::String(r+1), juce::dontSendNotification);
            egRouteChannelLabel[r].setColour (juce::Label::textColourId, SetupUI::labelsColor);
            addAndMakeVisible(egRouteChannelLabel[r]);

            addAndMakeVisible(egRouteChannelBox[r]);
            egRouteChannelBox[r].addItem("Disabled", 1);

            for (int ch = 1; ch <= 16; ++ch)
                egRouteChannelBox[r].addItem("Ch " + juce::String(ch), ch + 1); // 2..17

            egRouteChannelAttach[r] = std::make_unique<ChoiceAttachment>(
                apvts, "egRoute" + rs + "_channel", egRouteChannelBox[r]
            );

            egRouteDestLabel[r].setText("Dest. CC", juce::dontSendNotification);
            egRouteDestLabel[r].setColour (juce::Label::textColourId, SetupUI::labelsColor);
            addAndMakeVisible(egRouteDestLabel[r]);

            addAndMakeVisible(egRouteDestBox[r]);

            // Initial population (will be updated by onChange handler)
            populateEgDestinationBox(egRouteDestBox[r], ChannelType::Disabled);

            egRouteDestAttach[r] = std::make_unique<ChoiceAttachment>(
                apvts, "egRoute" + rs + "_dest", egRouteDestBox[r]
            );

            egRouteChannelBox[r].onChange = [this, r]() { 
                updateDestinationBoxForRoute(r);

                refreshEgRouteAvailability(); 

            };
            egRouteDestBox[r].onChange = [this]() { refreshEgRouteAvailability(); };

        }

        refreshEgRouteAvailability();

        // Initial update of destination boxes based on channel selection
        for (int r = 0; r < maxRoutes; ++r)
            updateDestinationBoxForRoute(r);

        refreshEgRouteAvailability();

        // ---- Sliders 
        // ---- apvts
        attackAttach  = std::make_unique<SliderAttachment>(apvts, "egAttack",  attackSlider);
        setupAttackSlider();
        holdAttach    = std::make_unique<SliderAttachment>(apvts, "egHold",    holdSlider);
        setupHoldSlider();
        decayAttach   = std::make_unique<SliderAttachment>(apvts, "egDecay",   decaySlider);
        setupDecaySlider();
        sustainAttach = std::make_unique<SliderAttachment>(apvts, "egSustain",    sustainSlider);
        setupSustainSlider();
        releaseAttach = std::make_unique<SliderAttachment>(apvts, "egRelease", releaseSlider);
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

        // ---- Release long toggle (bool)
        releaseLong = std::make_unique<LedToggleButton>("Long", SetupUI::LedColour::Blue);
        addAndMakeVisible(*releaseLong);
        releaseLongAttach = std::make_unique<ButtonAttachment>(apvts, "egReleaseLong", *releaseLong);

        releaseLongLabel.setText ("Long", juce::dontSendNotification);
        releaseLongLabel.setJustificationType (juce::Justification::centredLeft);
        releaseLongLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(releaseLongLabel);
        
        // Add callback to update release slider outline when releaseLong changes
        releaseLong->onClick = [this]() {
            updateReleaseSliderOutline();
            releaseSlider.updateText();
        };

        // ---- EG to LFO Depth
        egToLfoDepth = std::make_unique<LedToggleButton>("egToLfoDepth", SetupUI::LedColour::Blue);
        addAndMakeVisible(*egToLfoDepth);
        egToLfoDepthAttach = std::make_unique<ButtonAttachment>(apvts, "egToLfoDepth", *egToLfoDepth);

        egToLfoDepthLabel.setText ("EG to LFO Depth", juce::dontSendNotification);
        egToLfoDepthLabel.setJustificationType (juce::Justification::centredLeft);
        egToLfoDepthLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(egToLfoDepthLabel);

        // ---- EG to LFO Rate
        egToLfoRate = std::make_unique<LedToggleButton>("egToLfoRate", SetupUI::LedColour::Blue);
        addAndMakeVisible(*egToLfoRate);
        egToLfoRateAttach = std::make_unique<ButtonAttachment>(apvts, "egToLfoRate", *egToLfoRate);

        egToLfoRateLabel.setText ("EG to LFO Rate", juce::dontSendNotification);
        egToLfoRateLabel.setJustificationType (juce::Justification::centredLeft);
        egToLfoRateLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(egToLfoRateLabel);

        // Keep LED states in sync with automation/preset changes
        startTimerHz(20);
        
        // Initialize release slider outline based on current releaseLong state
        updateReleaseSliderOutline();

        // Refresh EG dest. param list to avoid conflict with LFO dest. parameters
        refreshEgRouteAvailability();
    }

    ~EnvelopeEditorComponent() override
    {
        stopTimer();
        attackAttach.reset(); holdAttach.reset(); decayAttach.reset(); sustainAttach.reset(); releaseAttach.reset(); velAttach.reset();
        egEnableAttach.reset(); noteSourceChannelAttach.reset();
        releaseLongAttach.reset();
        for (int r = 0; r < maxRoutes; ++r)
        {
            egRouteChannelAttach[r].reset();
        }
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
        attackOptions.alignItems    = juce::FlexBox::AlignItems::center;
        attackOptions.justifyContent= juce::FlexBox::JustifyContent::center;

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

        content.removeFromTop(20);

        placeRow(holdLabel, holdSlider);

        content.removeFromTop(20);

        placeRow(decayLabel, decaySlider);

        auto decayCurveRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox decayCurveBox;
        decayCurveBox.flexDirection  = juce::FlexBox::Direction::row;
        decayCurveBox.alignItems     = juce::FlexBox::AlignItems::center;
        decayCurveBox.justifyContent = juce::FlexBox::JustifyContent::center;

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

        content.removeFromTop(20);

        placeRow(sustainLabel, sustainSlider);

        content.removeFromTop(20);

        placeRow(releaseLabel, releaseSlider);

        auto releaseCurveRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox releaseCurveBox;
        releaseCurveBox.flexDirection  = juce::FlexBox::Direction::row;
        releaseCurveBox.alignItems     = juce::FlexBox::AlignItems::center;
        releaseCurveBox.justifyContent = juce::FlexBox::JustifyContent::center;

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

        content.removeFromTop(24);

        placeRow(velocityAmountLabel, velocityAmountSlider);

        content.removeFromTop(20);

        // EG to LFO
        auto egToLfoRow = content.removeFromTop(rowHeight + 4);

        juce::FlexBox egToLfoBox;
        egToLfoBox.flexDirection  = juce::FlexBox::Direction::row;
        egToLfoBox.alignItems     = juce::FlexBox::AlignItems::center;
        egToLfoBox.justifyContent = juce::FlexBox::JustifyContent::center;

        egToLfoBox.items.add(juce::FlexItem(*egToLfoDepth)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        egToLfoBox.items.add(juce::FlexItem(egToLfoDepthLabel)
                                                .withWidth(100)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));
        egToLfoBox.items.add(juce::FlexItem(*egToLfoRate)
                                                    .withWidth(22)
                                                    .withHeight(rowHeight)
                                                    .withMargin({ 0, 8, 0, 0 }));
        egToLfoBox.items.add(juce::FlexItem(egToLfoRateLabel)
                                                .withWidth(100)
                                                .withHeight(rowHeight)
                                                .withMargin({ 0, 8, 0, 0 }));

        egToLfoBox.performLayout(egToLfoRow);

        content.removeFromTop(18);

        // EG routes
        auto layoutRouteRow = [&](juce::Rectangle<int> row, int r)
        {
            juce::FlexBox fb;
            fb.flexDirection  = juce::FlexBox::Direction::row;
            fb.alignItems     = juce::FlexBox::AlignItems::center;
            fb.justifyContent = juce::FlexBox::JustifyContent::center;

            const float labelW     = 64.0f;
            const float chanW      = 90.0f;
            const float destLabelW = 64.0f;

            fb.items.add(juce::FlexItem(egRouteChannelLabel[r]).withWidth(labelW).withHeight((float)rowHeight));
            fb.items.add(juce::FlexItem(egRouteChannelBox[r]).withWidth(chanW).withHeight((float)rowHeight)
                            .withMargin({ 0, 16, 0, 0 }));

            fb.items.add(juce::FlexItem(egRouteDestLabel[r]).withWidth(destLabelW).withHeight((float)rowHeight)
                            .withMargin({ 0, 8, 0, 0 }));

            fb.items.add(juce::FlexItem(egRouteDestBox[r]).withFlex(1.0f).withHeight((float)rowHeight)
                            .withMargin({ 0, 8, 0, 0 }));

            fb.performLayout(row.toFloat());
        };

        const int routesHeight = (rowHeight + 8) * maxRoutes - 8;

        if (content.getHeight() > routesHeight + 10)
            content.removeFromBottom(10);

        auto routesArea = content.removeFromBottom(routesHeight);

        for (int r = 0; r < maxRoutes; ++r)
        {
            auto row = routesArea.removeFromTop(rowHeight);
            layoutRouteRow(row, r);
            routesArea.removeFromTop(8);
        }
    }

private:
    void timerCallback() override
    {
        updateEgUiEnabledState();

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

        // Refresh dest box filtering every tick so changes made in LFO or Delay
        // routes are reflected immediately without requiring a channel re-select.
        refreshEgRouteAvailability();
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

    void updateEgUiEnabledState()
    {
        const bool enabled = apvts.getRawParameterValue("egEnabled")->load() > 0.5f;

        // Master stays enabled so you can turn it back on
        if (egEnable)
            egEnable->setEnabled(true);

        // Gate the rest
        noteSourceEgChannelBox.setEnabled(enabled);
        noteSourceEgChannelLabel.setEnabled(enabled);

        for (int r = 0; r < maxRoutes; ++r)
        {
            egRouteChannelBox[r].setEnabled(enabled);
            egRouteDestBox[r].setEnabled(enabled);
        }

        attackSlider.setEnabled(enabled);
        holdSlider.setEnabled(enabled);
        decaySlider.setEnabled(enabled);
        sustainSlider.setEnabled(enabled);
        releaseSlider.setEnabled(enabled);
        velocityAmountSlider.setEnabled(enabled);

        attackFast->setEnabled(enabled);
        attackLong->setEnabled(enabled);
        attackSnap->setEnabled(enabled);

        decayLinear->setEnabled(enabled);
        decayExpo->setEnabled(enabled);
        decayLog->setEnabled(enabled);

        releaseLinear->setEnabled(enabled);
        releaseExpo->setEnabled(enabled);
        releaseLog->setEnabled(enabled);

        releaseLong->setEnabled(enabled);


        // Optional: alpha fade to make it obvious
        const float a = enabled ? 1.0f : 0.45f;
        noteSourceEgChannelBox.setAlpha(a);
        for (int r = 0; r < maxRoutes; ++r)
        {
            egRouteChannelLabel[r].setAlpha(enabled ? 1.0f : 0.6f);
            egRouteDestLabel[r].setAlpha(enabled ? 1.0f : 0.6f);
        }
        attackSlider.setAlpha(a);
        holdSlider.setAlpha(a);
        decaySlider.setAlpha(a);
        sustainSlider.setAlpha(a);
        releaseSlider.setAlpha(a);
        velocityAmountSlider.setAlpha(a);

        attackFast->setAlpha(a);
        attackLong->setAlpha(a);
        attackSnap->setAlpha(a);

        decayLinear->setAlpha(a);
        decayExpo->setAlpha(a);
        decayLog->setAlpha(a);

        releaseLinear->setAlpha(a);
        releaseExpo->setAlpha(a);
        releaseLog->setAlpha(a);

        releaseLong->setAlpha(a);
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
    
    // New enum for channel type
    enum class ChannelType {
            Disabled,  // ID 1
            MIDI       // ID 2..17
    };

    // Determine channel type from selected ID
    ChannelType getChannelType(int routeIndex) const
    {
        const int id = egRouteChannelBox[routeIndex].getSelectedId();
        if (id == 1) return ChannelType::Disabled;
        return ChannelType::MIDI;
    }

    void updateDestinationBoxForRoute (int routeIndex)
    {
        if (updatingEgRouteCombos)
            return;

        const ChannelType channelType = getChannelType(routeIndex);
        const ChannelType previousChannelType = lastChannelType[routeIndex];
        const bool channelTypeChanged = (channelType != previousChannelType);

        auto& destBox = egRouteDestBox[routeIndex];

        const int previousSelectedId = destBox.getSelectedId();

        // detach
        egRouteDestAttach[routeIndex].reset();

        populateEgDestinationBox(destBox, channelType);

        const int midiCount = ModzTaktAudioProcessor::egMidiDestCount();

        int desiredMaster = 0;

        if (channelType == ChannelType::MIDI)
        {
            const int prevMaster = (previousSelectedId > 0) ? (previousSelectedId - 1) : -1;
            desiredMaster = (!channelTypeChanged && prevMaster >= 0 && prevMaster < midiCount) ? prevMaster : 0;

            if (!egDestAllowedForRoute(routeIndex, desiredMaster))
                desiredMaster = findFirstAllowedMaster(routeIndex);
        }

        const auto rs = juce::String(routeIndex);
        const juce::String paramID = "egRoute" + rs + "_dest";

        // reattach FIRST (so it controls the ComboBox)
        egRouteDestAttach[routeIndex] =
            std::make_unique<ChoiceAttachment>(apvts, paramID, destBox);

        // now set the PARAM (attachment updates ComboBox consistently)
        updatingEgRouteCombos = true;
        setChoiceParamIndex(paramID, desiredMaster);
        updatingEgRouteCombos = false;

        lastChannelType[routeIndex] = channelType;
    }

    void populateEgDestinationBox(juce::ComboBox& box, ChannelType channelType)
    {
        box.clear();
        int itemId = 1;

        if (channelType == ChannelType::MIDI)
        {
            // Regular EG MIDI destinations
            for (int globalIdx = 0; globalIdx < juce::numElementsInArray(syntaktParameters); ++globalIdx)
                if (syntaktParameters[globalIdx].egDestination)
                    box.addItem(syntaktParameters[globalIdx].name, itemId++);
        }
        // Disabled: leave box empty
    }

    void setChoiceParamIndex (const juce::String& paramID, int index)
    {
        if (auto* p = apvts.getParameter(paramID))
        {
            const int steps = p->getNumSteps();          // for choice params: number of items
            const int maxIndex = juce::jmax(0, steps - 1);

            const int clampedIndex = juce::jlimit(0, maxIndex, index);
            const float norm = (maxIndex > 0) ? (float) clampedIndex / (float) maxIndex : 0.0f;

            p->beginChangeGesture();
            p->setValueNotifyingHost(norm);
            p->endChangeGesture();
        }
    }

    int getSelectedEgDestMasterIndex (int r) const
    {
        const int id = egRouteDestBox[r].getSelectedId();  // itemId
        return (id > 0) ? (id - 1) : -1;                    // master index
    }

    bool egDestAllowedForRoute (int r, int masterIdx) const
    {
        const auto type = getChannelType(r);

        if (type == ChannelType::Disabled)
            return true;

        // MIDI
        const int ch = getEgRouteChannelNumber(r); // 1..16
        const int globalParamIdx = mapEgChoiceToGlobalParamIndex(masterIdx);
        if (globalParamIdx < 0)
            return false;

        // conflict with LFO routes using same (ch,param)
        if (lfoUsesChannelParam(ch, globalParamIdx))
            return false;

        // conflict with other EG MIDI routes on same channel
        for (int other = 0; other < maxRoutes; ++other)
        {
            if (other == r) continue;
            if (getChannelType(other) != ChannelType::MIDI) continue;

            if (getEgRouteChannelNumber(other) != ch)
                continue;

            const int otherMaster = getSelectedEgDestMasterIndex(other);
            const int otherParam  = mapEgChoiceToGlobalParamIndex(otherMaster);

            if (otherParam == globalParamIdx)
                return false;
        }

        // conflict with Delay EG shaping:
        // when delayEgShape > 0 the delay engine owns (delayRouteCh, targetParam).
        {
            const int delayEgShape =
                (int) apvts.getRawParameterValue ("delayEgShape")->load();

            if (delayEgShape > 0)
            {
                const int delayTargetParam =
                    (delayEgShape == 1)
                        ? findGlobalParamByName ("Amp: Volume")
                        : findGlobalParamByName ("Track Level");

                if (globalParamIdx == delayTargetParam)
                {
                    for (int dr = 0; dr < maxRoutes; ++dr)
                    {
                        const int dCh = (int) apvts.getRawParameterValue (
                            "delayRoute" + juce::String (dr) + "_channel")->load();
                        if (dCh == ch)
                            return false;
                    }
                }
            }
        }

        return true;
    }

    int findFirstAllowedMaster (int r) const
    {
        auto& box = egRouteDestBox[r];
        for (int itemIndex = 0; itemIndex < box.getNumItems(); ++itemIndex)
        {
            const int itemId    = box.getItemId(itemIndex);
            const int masterIdx = itemId - 1;
            if (egDestAllowedForRoute(r, masterIdx))
                return masterIdx;
        }
        return 0; // fallback
    }

    // helpers used to prevent conflicts between LFO and EG dest. param

    // Find global syntaktParameters[] index by exact name. Returns -1 if not found.
    static int findGlobalParamByName (const char* name) noexcept
    {
        for (int i = 0; i < juce::numElementsInArray (syntaktParameters); ++i)
            if (juce::String (syntaktParameters[i].name) == name)
                return i;
        return -1;
    }

    int getEgMidiDestCount() const
    {
        int count = 0;
        for (int i = 0; i < juce::numElementsInArray(syntaktParameters); ++i)
            if (syntaktParameters[i].egDestination)
                ++count;
        return count;
    }

    int getEgRouteChannelNumber(int r) const
    {
        const int id = egRouteChannelBox[r].getSelectedId();
        if (id <= 1) return 0;         // Disabled
        return id - 1;                 // Ch 1..16 (IDs 2..17 → 1..16)
    }

    int getEgToLfoRouteIndex(int destChoice) const
    {
        return destChoice - getEgMidiDestCount(); // 0..2
    }

    bool lfoUsesChannelParam(int ch, int globalParamIdx) const
    {
        // LFO routes: route{r}_channel is Choice index 0=Disabled or 1..16
        // route{r}_param is 0..N-1 (global param index)
        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String(r);
            const int lfoCh = (int) apvts.getRawParameterValue("route" + rs + "_channel")->load();
            const int lfoP  = (int) apvts.getRawParameterValue("route" + rs + "_param")->load();
            if (lfoCh == ch && lfoP == globalParamIdx)
                return true;
        }
        return false;
    }

    // Map an EG destination choice index (0..egMidiDestCount-1) to global syntakt param index.
    // Returns -1 if out of range.
    int mapEgChoiceToGlobalParamIndex(int egChoiceIndex) const
    {
        int k = 0;
        for (int globalIdx = 0; globalIdx < juce::numElementsInArray(syntaktParameters); ++globalIdx)
        {
            if (!syntaktParameters[globalIdx].egDestination)
                continue;

            if (k == egChoiceIndex)
                return globalIdx;

            ++k;
        }
        return -1;
    }

    void refreshEgRouteAvailability()
    {
        if (updatingEgRouteCombos)
            return;

        updatingEgRouteCombos = true;

        for (int r = 0; r < maxRoutes; ++r)
        {
            const ChannelType channelType = getChannelType(r);
            auto& destBox = egRouteDestBox[r];

            if (channelType == ChannelType::Disabled || destBox.getNumItems() == 0)
            {
                // enable everything that exists
                for (int itemIndex = 0; itemIndex < destBox.getNumItems(); ++itemIndex)
                    destBox.setItemEnabled(destBox.getItemId(itemIndex), true);
                continue;
            }

            const int selectedMaster = getSelectedEgDestMasterIndex(r);

            // 1) enable/disable items based on rules
            for (int itemIndex = 0; itemIndex < destBox.getNumItems(); ++itemIndex)
            {
                const int itemId    = destBox.getItemId(itemIndex);
                const int masterIdx = itemId - 1;

                const bool allowed = egDestAllowedForRoute(r, masterIdx);
                destBox.setItemEnabled(itemId, allowed);
            }

            // 2) if current selection is now illegal, move to first allowed
            const int selectedId = destBox.getSelectedId();
            const bool noSelection = (selectedId == 0);
            const bool selectionIllegal = (!noSelection && !destBox.isItemEnabled(selectedId));
            const bool masterIllegal = (!noSelection && !egDestAllowedForRoute(r, selectedMaster));

            if (noSelection || selectionIllegal || masterIllegal)
            {
                const int newMaster = findFirstAllowedMaster(r);
                const int newId     = newMaster + 1;

                destBox.setSelectedId(newId, juce::dontSendNotification);
                setChoiceParamIndex("egRoute" + juce::String(r) + "_dest", newMaster);
            }
        }

        updatingEgRouteCombos = false;
    }

    // AHDSR Sliders setup
    void setupAttackSlider()
    {
        addAndMakeVisible(attackSlider);
        addAndMakeVisible(attackLabel);
        attackLabel.setText("Attack", juce::dontSendNotification);
        attackLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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
        holdLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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
        decayLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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
        sustainLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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
        releaseLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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
        velocityAmountLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

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

    // group
    juce::GroupComponent egGroup;

    // EG On/Off
    juce::Label egEnableLabel;

    // routing
    juce::Label noteSourceEgChannelLabel;
    juce::ComboBox noteSourceEgChannelBox;
    std::unique_ptr<ChoiceAttachment> noteSourceChannelAttach;

    static constexpr int maxRoutes = 3;

    std::array<juce::Label,   maxRoutes> egRouteChannelLabel;
    std::array<juce::ComboBox,maxRoutes> egRouteChannelBox;
    std::array<std::unique_ptr<ChoiceAttachment>, maxRoutes> egRouteChannelAttach;

    std::array<juce::Label,   maxRoutes> egRouteDestLabel;
    std::array<juce::ComboBox,maxRoutes> egRouteDestBox;
    std::array<std::unique_ptr<ChoiceAttachment>, maxRoutes> egRouteDestAttach;

    // For conflict handling / exclusion UI
    bool updatingEgRouteCombos = false;

    std::array<ChannelType, maxRoutes> lastChannelType { 
        ChannelType::Disabled, 
        ChannelType::Disabled, 
        ChannelType::Disabled 
    };

    // enable
    std::unique_ptr<LedToggleButton> egEnable;
    std::unique_ptr<ButtonAttachment> egEnableAttach;

    // sliders
    juce::Slider attackSlider, holdSlider, decaySlider, sustainSlider, releaseSlider, velocityAmountSlider;
    juce::Label  attackLabel,  holdLabel,  decayLabel,  sustainLabel,  releaseLabel,  velocityAmountLabel;

    std::unique_ptr<SliderAttachment> attackAttach, holdAttach, decayAttach, sustainAttach, releaseAttach, velAttach;

    // modes
    std::unique_ptr<LedToggleButton> attackFast, attackLong, attackSnap;
    juce::Label attackFastLabel, attackLongLabel, attackSnapLabel;

    std::unique_ptr<ButtonAttachment> attackFastAttach, attackLongAttach, attackSnapAttach;

    std::unique_ptr<LedToggleButton> decayLinear, decayExpo, decayLog;
    juce::Label decayLinearLabel, decayExpoLabel, decayLogLabel;

    std::unique_ptr<ButtonAttachment> decayLinearAttach, decayExpoAttach, decayLogAttach;

    std::unique_ptr<LedToggleButton> releaseLinear, releaseExpo, releaseLog, releaseLong;
    juce::Label releaseLinearLabel, releaseExpoLabel, releaseLogLabel, releaseLongLabel;

    std::unique_ptr<ButtonAttachment> releaseLinearAttach, releaseExpoAttach, releaseLogAttach, releaseLongAttach;


    //eg to lfo
    std::unique_ptr<LedToggleButton> egToLfoDepth, egToLfoRate;
    juce::Label egToLfoDepthLabel, egToLfoRateLabel;
    std::unique_ptr<ButtonAttachment> egToLfoDepthAttach, egToLfoRateAttach;


    // look & feel from your Cosmetic.h
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookDarkGreen  { SetupUI::sliderTrackDarkGreen };
    ModzTaktLookAndFeel lookOrange { SetupUI::sliderTrackOrange };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    ModzTaktLookAndFeel lookBlue { SetupUI::sliderTrackBlue };


};