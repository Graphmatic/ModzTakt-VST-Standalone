#pragma once
#include <JuceHeader.h>
#include "SyntaktParameterTable.h"
#include "MidiInput.h"
#include "EnvelopeComponent.h"
#include "ScopeModalComponent.h"
#include "Cosmetic.h"

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::AudioProcessorValueTreeState::Listener
{
public:
    MainComponent (ModzTaktAudioProcessor& p)
                                            : processor (p),
                                              apvts (p.getAPVTS())
    {
        // frame
        lfoGroup.setText("LFO");
        lfoGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::white);
        lfoGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::white);

        addAndMakeVisible(lfoGroup);

        // Envelop Generator
        envelopeComponent = std::make_unique<EnvelopeComponent>();
        addAndMakeVisible(*envelopeComponent);

        // Sync Mode
        syncModeLabel.setText("Sync Source:", juce::dontSendNotification);
        addAndMakeVisible(syncModeLabel);
        addAndMakeVisible(syncModeBox);
        syncModeBox.addItem("Free", 1);
        const bool isStandaloneWrapper = (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::WrapperType::wrapperType_Standalone);
        if (isStandaloneWrapper)
        {
            syncModeBox.addItem("MIDI Clock", 2);
        }
        else
        {
            syncModeBox.addItem("Host Clock", 2);
        }
        // apvts
        syncModeAttach = std::make_unique<ChoiceAttachment>(apvts, "syncMode", syncModeBox);
        apvts.addParameterListener ("syncMode", this);

        // Set default selections AFTER everything is wired up
        syncModeBox.setSelectedId(1); // Free mode by default      

        // BPM Display
        bpmLabelTitle.setText("Detected BPM:", juce::dontSendNotification);
        addAndMakeVisible(bpmLabelTitle);
        bpmLabel.setText("--", juce::dontSendNotification);
        bpmLabel.setColour(juce::Label::textColourId, juce::Colours::aqua);
        addAndMakeVisible(bpmLabel);

        // Sync Division
        divisionLabel.setText("Tempo Divider:", juce::dontSendNotification);
        addAndMakeVisible(divisionLabel);

        divisionBox.addItem("1/1", 1);
        divisionBox.addItem("1/2", 2);
        divisionBox.addItem("1/4", 3);
        divisionBox.addItem("1/8", 4);
        divisionBox.addItem("1/16", 5);
        divisionBox.addItem("1/32", 6);
        divisionBox.addItem("1/8 dotted", 7);
        divisionBox.addItem("1/16 dotted", 8);
        //divisionBox.onChange = [this]() { updateLfoRateFromBpm(rateSlider.getValue()); };
        addAndMakeVisible(divisionBox);
        // apvts
        syncDivisionAttach = std::make_unique<ChoiceAttachment>(apvts, "syncDivision", divisionBox);

        divisionBox.setSelectedId(3); // default quarter note

         // Shape
        shapeLabel.setText("LFO Shape:", juce::dontSendNotification);
        addAndMakeVisible(shapeLabel);
        addAndMakeVisible(shapeBox);
        shapeBox.addItem("Sine", 1);
        shapeBox.addItem("Triangle", 2);
        shapeBox.addItem("Square", 3);
        shapeBox.addItem("Saw", 4);
        shapeBox.addItem("Random", 5);
        shapeBox.setSelectedId(1);
        // apvts
        shapeAttach = std::make_unique<ChoiceAttachment>(apvts, "lfoShape", shapeBox);

        // Rate
        rateLabel.setText("Rate:", juce::dontSendNotification);
        addAndMakeVisible(rateLabel);
        
        addAndMakeVisible(rateSlider);
        rateSlider.setRange(0.1, 20.0, 0.01);
        rateSlider.setValue(2.0);
        rateSlider.setTextValueSuffix(" Hz");
        rateSlider.setLookAndFeel(&lookGreen);
        rateSlider.setNumDecimalPlacesToDisplay(2);
        // apvts
        rateAttach = std::make_unique<SliderAttachment>(apvts, "lfoRateHz", rateSlider);

        // Depth
        depthLabel.setText("Depth:", juce::dontSendNotification);
        addAndMakeVisible(depthLabel);
        addAndMakeVisible(depthSlider);
        depthSlider.setRange(0.0, 1.0, 0.01);
        depthSlider.setValue(1.0);
        depthSlider.setLookAndFeel(&lookPurple);
        depthSlider.setNumDecimalPlacesToDisplay(2);
        // apvts
        depthAttach = std::make_unique<SliderAttachment>(apvts, "lfoDepth", depthSlider);

          // Start Button
        addAndMakeVisible(startButton);
        startButton.setButtonText("Start LFO");
        startButton.setClickingTogglesState(true);  // CRITICAL: lets ButtonAttachment change APVTS

        // apvts
        lfoActiveAttach = std::make_unique<ButtonAttachment>(apvts, "lfoActive", startButton);

        // Note-On Restart
        noteRestartToggle = std::make_unique<LedToggleButton>
        (
            "Restart on Note-On",
            SetupUI::LedColour::Orange
        );

        addAndMakeVisible (*noteRestartToggle);
        noteRestartToggle->setToggleState (false, juce::sendNotification);
        noteRestartToggle->setButtonText ("");
        // apvts
        noteRestartAttach = std::make_unique<ButtonAttachment>(apvts, "noteRestart", *noteRestartToggle);

        noteRestartToggleLabel.setText ("Restart on Note-On", juce::dontSendNotification);
        noteRestartToggleLabel.setJustificationType (juce::Justification::centredLeft);
        noteRestartToggleLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        addAndMakeVisible (noteRestartToggleLabel);

        noteSourceChannelBox.setEnabled(false);

        addAndMakeVisible(noteSourceChannelBox);
        
        // noteSourceChannelBox.setTextWhenNothingSelected("Source Channel");
        for (int ch = 1; ch <= 16; ++ch)
                noteSourceChannelBox.addItem("Ch " + juce::String(ch), ch);

        // apvts
        noteSourceChannelAttach = std::make_unique<ChoiceAttachment>(apvts, "noteSourceChannel", noteSourceChannelBox);

        noteRestartToggle->onClick = [this]()
        {
            const bool enabled = noteRestartToggle->getToggleState();

            for (int i = 0; i < maxRoutes; ++i)
            {
                // Hide One-Shot UI if restart is OFF
                if (routeChannelBoxes[i].getSelectedId() != 1)
                    routeOneShotToggles[i]->setVisible(enabled);

                if (!enabled)
                {
                    // Hard-disable one-shot state
                    routeOneShotToggles[i]->setToggleState(false, juce::dontSendNotification);
                }
            }

            noteSourceChannelBox.setVisible(enabled);
            noteSourceChannelBox.setEnabled(enabled);
            addAndMakeVisible(noteSourceChannelBox);


            // Stop-on-Note-Off UI logic
            noteOffStopToggle->setVisible(enabled);
            noteOffStopToggle->setEnabled(enabled);

            noteOffStopToggleLabel.setVisible(enabled);

            addAndMakeVisible(*noteOffStopToggle);
            addAndMakeVisible (noteOffStopToggleLabel);

            if (!enabled)
            {
                noteOffStopToggle->setToggleState(false, juce::dontSendNotification);
                noteOffStopToggle->setVisible(enabled);
                noteOffStopToggle->setEnabled(enabled);
                noteOffStopToggleLabel.setVisible(enabled);
            }

            // layout refresh
            juce::MessageManager::callAsync([this]() { resized(); });
        };

        // noteOffStopToggle
        noteOffStopToggle = std::make_unique<LedToggleButton>
        (
            "Stop on Note-Off",
            SetupUI::LedColour::Orange
        );
        addAndMakeVisible(*noteOffStopToggle);
        noteOffStopToggle->setVisible(noteRestartToggle->getToggleState());
        noteOffStopToggle->setButtonText ("");
        noteOffStopToggle->setEnabled(false);
        // apvts
        noteOffStopAttach = std::make_unique<ButtonAttachment>(apvts, "noteOffStop", *noteOffStopToggle);

        noteOffStopToggleLabel.setText ("Stop on Note-Off", juce::dontSendNotification);
        noteOffStopToggleLabel.setJustificationType (juce::Justification::centredLeft);
        noteOffStopToggleLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        // LFO routes checkbox labels
        bipolarLabel.setText("+/-", juce::dontSendNotification);
        bipolarLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(bipolarLabel);

        invertPhaseLabel.setText("inv.", juce::dontSendNotification);
        invertPhaseLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(invertPhaseLabel);

        oneShotLabel.setText("1-s", juce::dontSendNotification);
        oneShotLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible(oneShotLabel);

        // Multi-CC Routing (3 routes)
        for (int i = 0; i < maxRoutes; ++i)
        {
            const auto rs = juce::String(i);

            // Label
            routeLabels[i].setText("Route " + juce::String(i + 1), juce::dontSendNotification);
            addAndMakeVisible(routeLabels[i]);

            // Channel box: must match APVTS choice order: Disabled, Ch1..Ch16
            routeChannelBoxes[i].clear();

            routeChannelBoxes[i].addItem("Disabled", 1);
            
            for (int ch = 1; ch <= 16; ++ch)
                routeChannelBoxes[i].addItem("Ch " + juce::String(ch), ch + 1);
            addAndMakeVisible(routeChannelBoxes[i]);

            // Parameter box: must match syntaktParamNames order (p+1 IDs)
            routeParameterBoxes[i].clear();
            for (int p = 0; p < juce::numElementsInArray(syntaktParameters); ++p)
                routeParameterBoxes[i].addItem(syntaktParameters[p].name, p + 1);
            addAndMakeVisible(routeParameterBoxes[i]);

            // Toggles
            routeBipolarToggles[i] = std::make_unique<LedToggleButton>("+/-", SetupUI::LedColour::Green);
            routeBipolarToggles[i]->setButtonText("+/-");
            addAndMakeVisible(*routeBipolarToggles[i]);

            routeInvertToggles[i] = std::make_unique<LedToggleButton>("Inv", SetupUI::LedColour::Green);
            routeInvertToggles[i]->setButtonText("Inv");
            addAndMakeVisible(*routeInvertToggles[i]);

            routeOneShotToggles[i] = std::make_unique<LedToggleButton>("1-Shot", SetupUI::LedColour::Orange);
            routeOneShotToggles[i]->setButtonText("1-Shot");
            addAndMakeVisible(*routeOneShotToggles[i]);

            // --- Attachments (must exist BEFORE you rely on parameter-driven state) ---
            routeChannelAttach[i] = std::make_unique<ChoiceAttachment>(apvts, "route" + rs + "_channel", routeChannelBoxes[i]);
            routeParamAttach[i]   = std::make_unique<ChoiceAttachment>(apvts, "route" + rs + "_param",    routeParameterBoxes[i]);

            routeBipolarAttach[i] = std::make_unique<ButtonAttachment>(apvts, "route" + rs + "_bipolar", *routeBipolarToggles[i]);
            routeInvertAttach[i]  = std::make_unique<ButtonAttachment>(apvts, "route" + rs + "_invert",  *routeInvertToggles[i]);
            routeOneShotAttach[i] = std::make_unique<ButtonAttachment>(apvts, "route" + rs + "_oneshot", *routeOneShotToggles[i]);

            // --- UI-only behavior on channel change (visibility etc.) ---
            routeChannelBoxes[i].onChange = [this, i]()
            {
                const int comboId = routeChannelBoxes[i].getSelectedId(); // 1=Disabled, 2..17=Ch1..16
                const bool enabled = (comboId != 1);

                routeParameterBoxes[i].setVisible(enabled);
                routeBipolarToggles[i]->setVisible(enabled);
                routeInvertToggles[i]->setVisible(enabled);

                // If disabling the route, force OneShot param OFF (optional but matches your old UX)
                if (!enabled)
                {
                    if (auto* p = apvts.getParameter("route" + juce::String(i) + "_oneshot"))
                    {
                        p->beginChangeGesture();
                        p->setValueNotifyingHost(0.0f);
                        p->endChangeGesture();
                    }
                }

                // Only show oneshot if route enabled AND noteRestart is enabled
                const bool noteRestartOn = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
                routeOneShotToggles[i]->setVisible(enabled && noteRestartOn);

                // // Initialize note source channel list
                // updateNoteSourceChannel();
        
                // Layout update
                juce::MessageManager::callAsync([this]() { resized(); });
            };

            // When parameter changes: optionally force bipolar according to parameter.isBipolar
            routeParameterBoxes[i].onChange = [this, i]()
            {
                const int idx = routeParameterBoxes[i].getSelectedId() - 1;
                if (idx < 0 || idx >= juce::numElementsInArray(syntaktParameters))
                    return;

                const bool paramIsBipolar = syntaktParameters[idx].isBipolar;

                // If you want "auto-init bipolar" each time user picks a param:
                if (auto* p = apvts.getParameter("route" + juce::String(i) + "_bipolar"))
                {
                    p->beginChangeGesture();
                    p->setValueNotifyingHost(paramIsBipolar ? 1.0f : 0.0f);
                    p->endChangeGesture();
                }
            };

            // Initial visibility based on current parameter values (after attachments exist)
            const bool enabledNow = (routeChannelBoxes[i].getSelectedId() != 1);
            routeParameterBoxes[i].setVisible(enabledNow);
            routeBipolarToggles[i]->setVisible(enabledNow);
            routeInvertToggles[i]->setVisible(enabledNow);

            const bool noteRestartNow = apvts.getRawParameterValue("noteRestart")->load() > 0.5f;
            routeOneShotToggles[i]->setVisible(enabledNow && noteRestartNow);
        }

        // scope image button
        scopeIcon = juce::ImageCache::getFromMemory(
            BinaryData::scope_png,
            BinaryData::scope_pngSize
        );

        scopeButton.setClickingTogglesState(true);
        scopeButton.setToggleable(true);

        // Assign images
        scopeButton.setImages(
            false,  // resizeButtonNow
            true,   // rescaleImageToFit
            true,   // preserveProportions

            scopeIcon, 1.0f, juce::Colours::transparentBlack,  // normal
            scopeIcon, 0.85f, juce::Colours::white.withAlpha(0.15f), // hover
            scopeIcon, 0.7f, juce::Colours::black.withAlpha(0.25f),  // pressed
            0.4f    // disabled opacity
        );

        scopeButton.onStateChange = [this]
        {
            scopeButton.setAlpha(scopeButton.getToggleState() ? 1.0f : 0.6f);
        };

        // Click action
        scopeButton.onClick = [this]
        {
            toggleScope();
        };

        addAndMakeVisible(scopeButton);

        // Settings Button
        addAndMakeVisible(settingsButton);
        settingsButton.setButtonText("Settings");
        settingsButton.setTooltip("Open settings menu");
        settingsButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        settingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);

        settingsButton.onClick = [this]()
        {
            juce::PopupMenu menu;

            juce::PopupMenu throttleSub;
                            throttleSub.addItem(1, "Off (send every change)", true, changeThreshold == 0);
                            throttleSub.addItem(2, "1 step (fine)",           true, changeThreshold == 1);
                            throttleSub.addItem(3, "2 steps",                 true, changeThreshold == 2);
                            throttleSub.addItem(4, "4 steps",                 true, changeThreshold == 4);
                            throttleSub.addItem(5, "8 steps (coarse)",        true, changeThreshold == 8);

            juce::PopupMenu limiterSub;
                            limiterSub.addItem(6, "Off (send every change)", true, msFloofThreshold == 0.0);
                            limiterSub.addItem(7, "0.5ms",                   true, msFloofThreshold == 0.5);
                            limiterSub.addItem(8, "1.0ms",                   true, msFloofThreshold == 1.0);
                            limiterSub.addItem(9, "1.5ms",                   true, msFloofThreshold == 1.5);
                            limiterSub.addItem(10, "2.0ms",                  true, msFloofThreshold == 2.0);
                            limiterSub.addItem(11, "3.0ms",                  true, msFloofThreshold == 3.0);
                            limiterSub.addItem(12, "5.0ms",                  true, msFloofThreshold == 5.0);


            menu.addSectionHeader("Performance");
            menu.addSubMenu("MIDI Data throttle", throttleSub);
            menu.addSubMenu("MIDI Rate limiter", limiterSub);

            menu.addSeparator();
            menu.addItem(99, "zaoum");


            menu.showMenuAsync(juce::PopupMenu::Options(),
                [this](int result)
                {
                    switch (result)
                    {
                        case 1: changeThreshold = 0; break;
                        case 2: changeThreshold = 1; break;
                        case 3: changeThreshold = 2; break;
                        case 4: changeThreshold = 4; break;
                        case 5: changeThreshold = 8; break;
                        case 6: msFloofThreshold = 0.0; break;
                        case 7: msFloofThreshold = 0.5; break;
                        case 8: msFloofThreshold = 1.0; break;
                        case 9: msFloofThreshold = 1.5; break;
                        case 10: msFloofThreshold = 2.0; break;
                        case 11: msFloofThreshold = 3.0; break;
                        case 12: msFloofThreshold = 5.0; break;
                        default: break;
                    }
                });
        };

        //MIDI MONITOR
        #if JUCE_DEBUG

        //EG check in ScopeRoute[0]
        addAndMakeVisible(showEGinScopeToggle);
        showEGinScopeToggle.setToggleState(showEGinScope, juce::dontSendNotification);
        showEGinScopeToggle.onClick = [this]()
            {
                showEGinScope = showEGinScopeToggle.getToggleState();
            };

        #endif

        // Timer
        startTimerHz(30); // 100Hz refresh  
    }

    ~MainComponent() override
    {
        stopTimer();

        rateSlider.setLookAndFeel (nullptr);
        depthSlider.setLookAndFeel (nullptr);

        apvts.removeParameterListener ("syncMode", this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (SetupUI::background);
    }

    void resized() override
    {
        // prepare column layout
        constexpr int lfoWidth = 450;
        constexpr int egWidth  = 450;
        constexpr int columnSpacing = 12;

        auto area = getLocalBounds().reduced(12);

        // Horizontal split
        auto lfoColumn = area.removeFromLeft(lfoWidth);
        area.removeFromLeft(columnSpacing);
        auto egColumn  = area.removeFromLeft(egWidth);

        // LFO block (fixed-width column)
        auto lfoArea = lfoColumn;

        lfoGroup.setBounds(lfoArea);

        auto lfoAreaContent = lfoArea.reduced(10, 20); // space for title

        auto rowHeight = 28;
        auto labelWidth = 150;
        auto spacing = 6;

        auto placeRow = [&](juce::Label& label, juce::Component& comp)
        {
            auto row = lfoAreaContent.removeFromTop(rowHeight);
            label.setBounds(row.removeFromLeft(labelWidth));
            row.removeFromLeft(spacing);
            comp.setBounds(row);
            lfoAreaContent.removeFromTop(6);
        };

        placeRow(syncModeLabel, syncModeBox);

        // BPM label pair
        auto bpmRow = lfoAreaContent.removeFromTop(rowHeight);
        bpmLabelTitle.setBounds(bpmRow.removeFromLeft(labelWidth));
        bpmRow.removeFromLeft(spacing);
        bpmLabel.setBounds(bpmRow);
        lfoAreaContent.removeFromTop(6);

        placeRow(divisionLabel, divisionBox);

        // LFO routes checkbox top labels
        constexpr int routeLabelWidth      = 70;
        constexpr int channelBoxWidth      = 90;
        constexpr int parameterBoxWidth    = 200;
        constexpr int checkboxColumnWidth  = 40;
        constexpr int columnGap            = 8;

        auto headerRow = lfoAreaContent.removeFromTop(rowHeight);

        juce::FlexBox headerFlex;
        headerFlex.flexDirection = juce::FlexBox::Direction::row;
        headerFlex.alignItems = juce::FlexBox::AlignItems::flexEnd;

        // spacers for Route / Channel / Parameter columns
        headerFlex.items.add(juce::FlexItem().withWidth(routeLabelWidth + columnGap));
        headerFlex.items.add(juce::FlexItem().withWidth(channelBoxWidth + columnGap));
        headerFlex.items.add(juce::FlexItem().withWidth(parameterBoxWidth + columnGap));

        // checkbox headers
        headerFlex.items.add(
            juce::FlexItem(bipolarLabel)
                .withWidth(checkboxColumnWidth)
                .withHeight(rowHeight)
                .withMargin({ 0, columnGap, 0, 0 })
        );

        headerFlex.items.add(
            juce::FlexItem(invertPhaseLabel)
                .withWidth(checkboxColumnWidth)
                .withHeight(rowHeight)
                .withMargin({ 0, columnGap, 0, 0 })
        );

        headerFlex.items.add(
            juce::FlexItem(oneShotLabel)
                .withWidth(checkboxColumnWidth)
                .withHeight(rowHeight)
                .withMargin({ 0, columnGap, 0, 0 })
        );

        headerFlex.performLayout(headerRow);

        lfoAreaContent.removeFromTop(6);

        // Place route selectors
        for (int i = 0; i < maxRoutes; ++i)
        {
            auto rowArea = lfoAreaContent.removeFromTop(rowHeight);

            juce::FlexBox fb;
            fb.flexDirection = juce::FlexBox::Direction::row;
            fb.alignItems = juce::FlexBox::AlignItems::center;

            fb.items.add(
                juce::FlexItem(routeLabels[i])
                    .withWidth(routeLabelWidth)
                    .withHeight(rowHeight)
                    .withMargin({ 0, columnGap, 0, 0 })
            );

            fb.items.add(
                juce::FlexItem(routeChannelBoxes[i])
                    .withWidth(channelBoxWidth)
                    .withHeight(rowHeight)
                    .withMargin({ 0, columnGap, 0, 0 })
            );

            if (routeParameterBoxes[i].isVisible())
            {
                fb.items.add(
                    juce::FlexItem(routeParameterBoxes[i])
                        .withWidth(parameterBoxWidth)
                        .withHeight(rowHeight)
                        .withMargin({ 0, columnGap, 0, 0 })
                );
            }
            else
            {
                // keep column alignment even if hidden
                fb.items.add(juce::FlexItem().withWidth(parameterBoxWidth + columnGap));
            }

            fb.items.add(
                juce::FlexItem(*routeBipolarToggles[i])
                    .withWidth(checkboxColumnWidth)
                    .withHeight(rowHeight - 4)
                    .withMargin({ 0, columnGap, 0, 0 })
            );

            fb.items.add(
                juce::FlexItem(*routeInvertToggles[i])
                    .withWidth(checkboxColumnWidth)
                    .withHeight(rowHeight - 4)
                    .withMargin({ 0, columnGap, 0, 0 })
            );

            fb.items.add(
                juce::FlexItem(*routeOneShotToggles[i])
                    .withWidth(checkboxColumnWidth)
                    .withHeight(rowHeight - 4)
                    .withMargin({ 0, columnGap, 0, 0 })
            );

            fb.performLayout(rowArea);
            lfoAreaContent.removeFromTop(10);
        }

        placeRow(shapeLabel, shapeBox);

        lfoAreaContent.removeFromTop(6);

        placeRow(rateLabel, rateSlider);
        placeRow(depthLabel, depthSlider);

        lfoAreaContent.removeFromTop(10);
        startButton.setBounds(lfoAreaContent.removeFromTop(40));

        lfoAreaContent.removeFromTop(10);

        auto placeRowToggle = [&](juce::Button& button,
                                  juce::Label& label,
                                  juce::Component& rightAlignComponent)
        {
            // Take one row from the flowing layout
            auto row = lfoAreaContent.removeFromTop (rowHeight);

            // --- Button (fixed size, vertically centered)
            auto buttonArea = row.removeFromLeft (labelWidth);

            const int buttonY = buttonArea.getY()
                              + (buttonArea.getHeight() - SetupUI::toggleSize) / 2;

            button.setBounds (buttonArea.getX(),
                              buttonY,
                              SetupUI::toggleSize,
                              SetupUI::toggleSize);

            // --- Label (takes remaining space before combobox)
            auto labelArea = buttonArea.withX (button.getRight() + (spacing - 6));
            labelArea.setWidth (labelWidth - SetupUI::toggleSize - spacing);

            label.setBounds (labelArea);

            // --- Right aligned control
            row.removeFromLeft (spacing);
            rightAlignComponent.setBounds (row);

            // Vertical spacing after row
            lfoAreaContent.removeFromTop (6);
        };

        placeRowToggle (*noteRestartToggle, noteRestartToggleLabel, noteSourceChannelBox);

        auto placeSingleToggleRow = [&](juce::Button& button,
                                  juce::Label& label)
        {
            // Take one row from the flowing layout
            auto row = lfoAreaContent.removeFromTop (rowHeight);

            // --- Button (fixed size, vertically centered)
            auto buttonArea = row.removeFromLeft (labelWidth);

            const int buttonY = buttonArea.getY()
                              + (buttonArea.getHeight() - SetupUI::toggleSize) / 2;

            button.setBounds (buttonArea.getX(),
                              buttonY,
                              SetupUI::toggleSize,
                              SetupUI::toggleSize);

            // --- Label (takes remaining space before combobox)
            auto labelArea = buttonArea.withX (button.getRight() + (spacing - 6));
            labelArea.setWidth (labelWidth - SetupUI::toggleSize - spacing);

            label.setBounds (labelArea);

            // Vertical spacing after row
            lfoAreaContent.removeFromTop (6);
        };

        // Stop-on-Note-Off toggle directly underneath
        if (noteOffStopToggle->isVisible())
            placeSingleToggleRow(*noteOffStopToggle, noteOffStopToggleLabel);

        constexpr int marginScope = 8;
        constexpr int scopeButtonSize = 40;

        auto lfoBounds = lfoGroup.getBounds();

        scopeButton.setBounds(
            lfoBounds.getX() + marginScope,
            lfoBounds.getBottom() - scopeButtonSize - marginScope + 2,
            scopeButtonSize,
            scopeButtonSize
        );

        scopeButton.setOpaque(false);

        #if JUCE_DEBUG
            showEGinScopeToggle.setBounds(
            lfoBounds.getX() + marginScope + 50,
            lfoBounds.getBottom() - scopeButtonSize - marginScope + 2,
            scopeButtonSize + 20,
            scopeButtonSize
        );
        #endif


        // setting button
        constexpr int size = 24;

        auto bounds = getLocalBounds();

        settingsButton.setBounds(bounds.removeFromBottom(10 + size)
                                        .removeFromRight(10 + size)
                                        .removeFromLeft(size)
                                        .removeFromTop(size));

        settingsButton.setColour(juce::TextButton::buttonOnColourId, juce::Colours::darkgrey.withAlpha(0.3f));
        settingsButton.setClickingTogglesState(false);

        // Envelop generator frame
        if (envelopeComponent != nullptr)
            envelopeComponent->setBounds(egColumn);
    }

    // Oscilloscope pop-up view (not modal)
    void toggleScope()
    {
        if (scopeOverlay)
        {
            closeScope();
            return;
        }

        lfoRoutesToScope[0] = true; // first route active by default

        scopeOverlay.reset(new ScopeModalComponent<maxRoutes>(
            lastLfoRoutesValues,
            lfoRoutesToScope));

        scopeOverlay->onAllRoutesDisabled = [this]()
        {
            toggleScope();   // closes and cleans up
        };

        addAndMakeVisible(scopeOverlay.get());

        constexpr int scopeSize = 136;
        constexpr int bottomOffset = 45;

        // Position relative to LFO area
        auto lfoBounds = getLocalBounds()
                            .withHeight(800).reduced(12)
                            .removeFromLeft(450);  //LFO area width

        scopeOverlay->setBounds(
            lfoBounds.getCentreX() - scopeSize / 2,
            lfoBounds.getBottom() - bottomOffset - scopeSize,
            scopeSize,
            scopeSize
        );

        scopeOverlay->toFront(true);
    }

    void closeScope()
    {
        if (!scopeOverlay)
            return;

        lfoRoutesToScope.fill(false);
        removeChildComponent(scopeOverlay.get());
        scopeOverlay.reset();
    }


private:
    // UI Components
    ModzTaktAudioProcessor& processor;
    ModzTaktAudioProcessor::APVTS& apvts;

    //juce::AudioProcessorValueTreeState& apvts;

    std::atomic<bool> pendingSyncModeChange { false };

    

    juce::GroupComponent lfoGroup;

    juce::Label syncModeLabel;
    juce::Label bpmLabelTitle, bpmLabel, divisionLabel;
    juce::Label parameterLabel, shapeLabel, rateLabel, depthLabel, channelLabel, bipolarLabel, invertPhaseLabel, oneShotLabel;

    juce::ComboBox syncModeBox, divisionBox; //////divbox
    juce::ComboBox shapeBox; ////////////

    // SLiders
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    juce::Slider rateSlider, depthSlider; //////////

    //Note-On retrig on/off and source channel
    std::unique_ptr<LedToggleButton> noteRestartToggle, noteOffStopToggle; ////////////
    juce::Label noteRestartToggleLabel, noteOffStopToggleLabel;

    juce::ComboBox noteSourceChannelBox; // source channel for Note-On listening (lfo)

    juce::TextButton startButton;

    bool syncEnabled = false; // for BPM display

    // Multi-CC Routing
    static constexpr int maxRoutes = 3;
    struct LfoRoute { 
        int midiChannel = 0;
        int parameterIndex = 0;
        bool bipolar = false;
        bool invertPhase = false;
        bool oneShot = false;
        bool passedPeak = false; // used when unipolar + oneshot
        bool hasFinishedOneShot = false; // runtime state
    };

    std::array<LfoRoute, maxRoutes> lfoRoutes;
    std::array<juce::Label, maxRoutes> routeLabels;
    std::array<juce::ComboBox, maxRoutes> routeChannelBoxes;
    std::array<juce::ComboBox, maxRoutes> routeParameterBoxes;

    std::unique_ptr<LedToggleButton> routeBipolarToggles[maxRoutes], routeInvertToggles[maxRoutes], routeOneShotToggles[maxRoutes];

    #if JUCE_DEBUG
    // EG test: to scope Route 0.
    juce::ToggleButton showEGinScopeToggle{ "EG to Scope" };

    bool showEGinScope = false;
    #endif

    // Setting Pop-Up
    juce::TextButton settingsButton;

    //*******************************  APVTS ****************************************//
    //*******************************************************************************//
    using SliderAttachment  = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment  = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ChoiceAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<ButtonAttachment> lfoActiveAttach;
    std::unique_ptr<SliderAttachment> rateAttach, depthAttach;
    std::unique_ptr<ChoiceAttachment> shapeAttach;
    std::unique_ptr<ChoiceAttachment> syncModeAttach;
    std::unique_ptr<ButtonAttachment> noteRestartAttach, noteOffStopAttach;
    std::unique_ptr<ChoiceAttachment> noteSourceChannelAttach;
    std::unique_ptr<ChoiceAttachment> syncDivisionAttach;

    //LFO Routes UI
    std::array<std::unique_ptr<ChoiceAttachment>, 3> routeChannelAttach;
    std::array<std::unique_ptr<ChoiceAttachment>, 3> routeParamAttach;

    std::array<std::unique_ptr<ButtonAttachment>, 3> routeBipolarAttach;
    std::array<std::unique_ptr<ButtonAttachment>, 3> routeInvertAttach;
    std::array<std::unique_ptr<ButtonAttachment>, 3> routeOneShotAttach;

    bool lastWasRandomShape = false;

    // BPM smoothing / throttling
    double displayedBpm = 0.0;
    juce::int64 lastBpmUpdateMs = 0;

    // Oscilloscope
    juce::Image scopeIcon;
    juce::ImageButton scopeButton;
    std::unique_ptr<ScopeModalComponent<maxRoutes>> scopeOverlay;

    std::array<std::atomic<float>, maxRoutes> lastLfoRoutesValues { 0.0f, 0.0f, 0.0f };
    std::array<bool, maxRoutes> lfoRoutesToScope { false, false, false };

    // EG
    std::unique_ptr<EnvelopeComponent> envelopeComponent;

    // settings - Dithering and MIDI throttle
    int changeThreshold = 1; // difference needed before sending

    // settings - Anti flooding
    double msFloofThreshold = 0.0; // delay between Midi datas chunk

    void parameterChanged (const juce::String& paramID, float newValue) override
    {
        if (paramID == "syncMode")
        {
            // runs on audio thread: DO NOT touch UI or start/stop devices here
            pendingSyncModeChange.store (true, std::memory_order_release);
        }
    }

    void timerCallback() override
    {
        // UI update
        const bool lfoRunning = processor.isLfoRunningForUi();

        const juce::String lfoStartStopText = lfoRunning ? "Stop LFO" : "Start LFO";
        if (startButton.getButtonText() != lfoStartStopText)
            startButton.setButtonText(lfoStartStopText);

        if (processor.uiRequestSetLfoActiveOn.exchange(false, std::memory_order_acq_rel))
        {
            if (auto* p = apvts.getParameter("lfoActive"))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost(1.0f);  // turn ON
                p->endChangeGesture();
            }
        }

        const bool on = processor.getAPVTS().getRawParameterValue("lfoActive")->load() > 0.5f;
        startButton.setButtonText(on ? "Stop LFO" : "Start LFO");

        const double rateValue = processor.getAPVTS().getRawParameterValue("lfoRateHz")->load();
        rateSlider.setValue(rateValue, juce::dontSendNotification);

        const int shapeId = shapeBox.getSelectedId();
        const bool isRandom = (shapeId == 5);

        if (isRandom != lastWasRandomShape)
        {
            lastWasRandomShape = isRandom;

            for (int i = 0; i < maxRoutes; ++i)
            {
                auto* bipolar = routeBipolarToggles[i].get();
                auto* invert  = routeInvertToggles[i].get();

                if (isRandom)
                {
                    bipolar->setToggleState(false, juce::sendNotification); // do not update actual flag! so:
                    // TODO : UPDATE routes flags explicitely so Random always bypass bipolar/phase
                    bipolar->setEnabled(false);
                    bipolar->setAlpha(0.8f);

                    invert->setToggleState(false, juce::sendNotification);
                    // TODO : UPDATE routes flags explicitely so Random always bypass bipolar/phase

                    invert->setEnabled(false);
                    invert->setAlpha(0.8f);
                }
                else
                {
                    bipolar->setEnabled(true);
                    bipolar->setAlpha(1.0f);

                    invert->setEnabled(true);
                    invert->setAlpha(1.0f);
                }
            }
        }

        // Display bpm TODO
        // const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        // const double bpm = midiClock.getCurrentBPM();

        // Always update BPM display if sync mode is active
        // if (syncEnabled)
        // {
        //     const double bpm = midiClock.getCurrentBPM();
        //     const auto nowMs = juce::Time::getMillisecondCounterHiRes();

        //     if (bpm > 0.0)
        //     {
        //         // Smooth & rate-limit UI updates
        //         displayedBpm = 0.9 * displayedBpm + 0.1 * bpm;
        //         if (nowMs - lastBpmUpdateMs > 250.0)
        //         {
        //             bpmLabel.setText(juce::String(displayedBpm, 1), juce::dontSendNotification);
        //             lastBpmUpdateMs = nowMs;
        //         }
        //     }
        //     else
        //     {
        //         // No clock yet: show placeholder
        //         if (nowMs - lastBpmUpdateMs > 500.0)
        //         {
        //             bpmLabel.setText("--", juce::dontSendNotification);
        //             lastBpmUpdateMs = nowMs;
        //         }
        //     }
        // }
        // else
        // {
        //     // When not in sync mode, just freeze BPM display
        // }
    }
};
