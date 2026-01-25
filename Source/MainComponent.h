#pragma once
#include <JuceHeader.h>
#include "SyntaktParameterTable.h"
#include "MidiInput.h"
#include "MidiMonitorWindow.h"
#include "EnvelopeComponent.h"
#include "ScopeModalComponent.h"
#include "Cosmetic.h"

class MainComponent : public juce::Component,
                      private juce::Timer,
                      public MidiClockListener
{
public:
    MainComponent()
    {

        // Initialize MIDI clock listener
        midiClock.setListener(this);

        // frame
        lfoGroup.setText("LFO");
        lfoGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::white);
        lfoGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::white);

        addAndMakeVisible(lfoGroup);

        // Envelop Generator
        envelopeComponent = std::make_unique<EnvelopeComponent>();
        addAndMakeVisible(*envelopeComponent);

        // MIDI Output
        midiOutputLabel.setText("MIDI Output:", juce::dontSendNotification);
        addAndMakeVisible(midiOutputLabel);
        addAndMakeVisible(midiOutputBox);

        // MIDI Input for Clock
        midiInputLabel.setText("MIDI Input (Clock):", juce::dontSendNotification);
        addAndMakeVisible(midiInputLabel);
        addAndMakeVisible(midiInputBox);

        refreshMidiInputs();
        refreshMidiOutputs();

        // Sync Mode
        syncModeLabel.setText("Sync Source:", juce::dontSendNotification);
        addAndMakeVisible(syncModeLabel);
        addAndMakeVisible(syncModeBox);
        syncModeBox.addItem("Free", 1);
        syncModeBox.addItem("MIDI Clock", 2);

        // SET UP CALLBACKS BEFORE POPULATING OR SETTING VALUES
        midiOutputBox.onChange = [this] { openSelectedMidiOutput(); };
        midiInputBox.onChange = [this]() { updateMidiInput(); };
        syncModeBox.onChange = [this]() { updateMidiClockState(); };

        // NOW populate the combo boxes (might trigger callbacks if devices exist)
        refreshMidiOutputs();
        refreshMidiInputs();

        // Set default selections AFTER everything is wired up
        syncModeBox.setSelectedId(1); // Free mode by default      

        // Select first available MIDI output if any
        if (midiOutputBox.getNumItems() > 0)
            midiOutputBox.setSelectedId(1);
    
        // Select first available MIDI input if any  
        if (midiInputBox.getNumItems() > 0)
            midiInputBox.setSelectedId(1);

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
        divisionBox.onChange = [this]() { updateLfoRateFromBpm(rateSlider.getValue()); };
        addAndMakeVisible(divisionBox);

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

        shapeBox.onChange = [this]()
        {
            for (int i = 0; i < maxRoutes; ++i)
            {
                if (shapeBox.getSelectedId() == 5) // disable bipolar and invert-Phase if shape = Random
                {
                    routeBipolarToggles[i]->setToggleState(false, juce::sendNotification);
                    routeBipolarToggles[i]->setEnabled(false);
                    routeBipolarToggles[i]->setAlpha(0.8f);

                    routeInvertToggles[i]->setToggleState(false, juce::sendNotification);
                    routeInvertToggles[i]->setEnabled(false);
                    routeInvertToggles[i]->setAlpha(0.8f);
                }
                else
                {
                    routeBipolarToggles[i]->setEnabled(true);
                    routeBipolarToggles[i]->setAlpha(1.0f);

                    routeInvertToggles[i]->setEnabled(true);
                    routeInvertToggles[i]->setAlpha(1.0f);
                }
            }
        };

        // Rate
        rateLabel.setText("Rate:", juce::dontSendNotification);
        addAndMakeVisible(rateLabel);
        addAndMakeVisible(rateSlider);
        rateSlider.setRange(0.1, 20.0, 0.01);
        rateSlider.setValue(2.0);
        rateSlider.setTextValueSuffix(" Hz");
        rateSlider.setLookAndFeel(&lookGreen);


        // Depth
        depthLabel.setText("Depth:", juce::dontSendNotification);
        addAndMakeVisible(depthLabel);
        addAndMakeVisible(depthSlider);
        depthSlider.setRange(0.0, 1.0, 0.01);
        depthSlider.setValue(1.0);
        depthSlider.setLookAndFeel(&lookPurple);


          // Start Button
        addAndMakeVisible(startButton);
        startButton.setButtonText("Start LFO");
        startButton.onClick = [this] { toggleLfo(); };

        // Note-On Restart
        noteRestartToggle = std::make_unique<LedToggleButton>
        (
            "Restart on Note-On",
            SetupUI::LedColour::Orange
        );

        addAndMakeVisible (*noteRestartToggle);
        noteRestartToggle->setToggleState (false, juce::dontSendNotification);
        noteRestartToggle->setButtonText ("");

        noteRestartToggleLabel.setText ("Restart on Note-On", juce::dontSendNotification);
        noteRestartToggleLabel.setJustificationType (juce::Justification::centredLeft);
        noteRestartToggleLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        addAndMakeVisible (noteRestartToggleLabel);

        addAndMakeVisible(noteSourceChannelBox);
        noteSourceChannelBox.setTextWhenNothingSelected("Source Channel");

        noteSourceChannelBox.onChange = [this]()
        {
            noteRestartChannel.store(noteSourceChannelBox.getSelectedId(), std::memory_order_release);
        };

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

                    lfoRoutes[i].oneShot = false;
                    lfoRoutes[i].hasFinishedOneShot = false;
                }
            }

            // Stop-on-Note-Off UI logic
            noteOffStopToggle->setVisible(enabled);
            noteOffStopToggle->setEnabled(enabled);
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

        noteOffStopToggle->setVisible(noteRestartToggle->getToggleState());
        noteOffStopToggle->setButtonText ("");
        noteOffStopToggle->setEnabled(false);

        noteOffStopToggleLabel.setText ("Stop on Note-Off", juce::dontSendNotification);
        noteOffStopToggleLabel.setJustificationType (juce::Justification::centredLeft);
        noteOffStopToggleLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);


        // Debug labels
        #if JUCE_DEBUG
        noteDebugTitle.setText("Detected Note-On:", juce::dontSendNotification);
        addAndMakeVisible(noteDebugTitle);
        noteDebugLabel.setText("--", juce::dontSendNotification);
        noteDebugLabel.setColour(juce::Label::textColourId, juce::Colours::aqua);
        addAndMakeVisible(noteDebugLabel);
        #endif

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
            // Label
            routeLabels[i].setText("Route " + juce::String(i + 1),
                                    juce::dontSendNotification);
            addAndMakeVisible(routeLabels[i]);

            // Channel box
            if (i != 0) {
                routeChannelBoxes[i].addItem("Disabled", 1);
            }
            
            for (int ch = 1; ch <= 16; ++ch)
                routeChannelBoxes[i].addItem("Ch " + juce::String(ch), ch + 1);

            addAndMakeVisible(routeChannelBoxes[i]);

            // Parameter box
            for (int p = 0; p < juce::numElementsInArray(syntaktParameters); ++p)
                routeParameterBoxes[i].addItem(syntaktParameters[p].name, p + 1);

            addAndMakeVisible(routeParameterBoxes[i]);

            // Bipolar toggle
            routeBipolarToggles[i] = std::make_unique<LedToggleButton>
            (
                "+/-",
                SetupUI::LedColour::Green
            );
            routeBipolarToggles[i]->setButtonText("+/-");
            addAndMakeVisible(*routeBipolarToggles[i]);

            // Invert Phase toggle
            routeInvertToggles[i] = std::make_unique<LedToggleButton>
            (
                "Inv",
                SetupUI::LedColour::Green
            );
            routeInvertToggles[i]->setButtonText("Inv");
            addAndMakeVisible(*routeInvertToggles[i]);

            // One Shot toggle
            routeOneShotToggles[i] = std::make_unique<LedToggleButton>
            (
                "Inv",
                SetupUI::LedColour::Orange
            );
            routeOneShotToggles[i]->setButtonText("1-Shot");
            addAndMakeVisible(*routeOneShotToggles[i]);

            // Set up callbacks BEFORE setting any values
            routeChannelBoxes[i].onChange = [this, i]()
            {
                //debug
                bool stopLFO = false;
                if (lfoActive)
                {
                    toggleLfo();
                    stopLFO = true;
                }
                const int comboId = routeChannelBoxes[i].getSelectedId();
                lfoRoutes[i].midiChannel = (comboId == 1) ? 0 : (comboId - 1);
                const bool enabled = (comboId != 1);
                routeParameterBoxes[i].setVisible(enabled);
                routeBipolarToggles[i]->setVisible(enabled);
                routeInvertToggles[i]->setVisible(enabled);
                if (!enabled)
                    routeOneShotToggles[i]->setToggleState(false, juce::dontSendNotification);
                routeOneShotToggles[i]->setVisible(enabled && noteRestartToggle->getToggleState());

                updateNoteSourceChannel();

                if (stopLFO)
                {
                    toggleLfo();
                    stopLFO = false;
                }
                
                // Defer resized() to avoid blocking during ComboBox interaction
                juce::MessageManager::callAsync([this]() { resized(); });
            };

            routeParameterBoxes[i].onChange = [this, i]()
            {
                lfoRoutes[i].parameterIndex =
                    routeParameterBoxes[i].getSelectedId() - 1;

                const bool paramIsBipolar =
                    syntaktParameters[lfoRoutes[i].parameterIndex].isBipolar;

                // Initialize UI + route state ONCE
                routeBipolarToggles[i]->setToggleState(paramIsBipolar,
                                                     juce::dontSendNotification);
                lfoRoutes[i].bipolar = paramIsBipolar;
            };


            routeBipolarToggles[i]->onClick = [this, i]()
            {
                lfoRoutes[i].bipolar = routeBipolarToggles[i]->getToggleState();
                #if JUCE_DEBUG
                updateLfoRouteDebugLabel();
                #endif
            };

            routeInvertToggles[i]->onClick = [this, i]()
            {
                lfoRoutes[i].invertPhase = routeInvertToggles[i]->getToggleState();
            };

            routeOneShotToggles[i]->onClick = [this, i]()
            {
                lfoRoutes[i].oneShot = routeOneShotToggles[i]->getToggleState();

                if (!lfoRoutes[i].oneShot)
                    lfoRoutes[i].hasFinishedOneShot = false;
            };

            routeInvertToggles[i]->setToggleState(false, juce::dontSendNotification);
            routeOneShotToggles[i]->setToggleState(false, juce::dontSendNotification);

            // set initial values
            if (i == 0)
                routeChannelBoxes[i].setSelectedId(2, juce::dontSendNotification); // Ch 1
            else
                routeChannelBoxes[i].setSelectedId(1, juce::dontSendNotification); // Disabled
                
            routeParameterBoxes[i].setSelectedId(1, juce::dontSendNotification);
            routeBipolarToggles[i]->setToggleState(false, juce::dontSendNotification);

            // Initialize route state
            lfoRoutes[i].midiChannel = (routeChannelBoxes[i].getSelectedId() == 1)
                                        ? 0
                                        : routeChannelBoxes[i].getSelectedId() - 1;

            lfoRoutes[i].parameterIndex = routeParameterBoxes[i].getSelectedId() - 1;

            lfoRoutes[i].bipolar = routeBipolarToggles[i]->getToggleState();

            lfoRoutes[i].invertPhase = false;
            lfoRoutes[i].oneShot     = false;
            lfoRoutes[i].hasFinishedOneShot = false;

            // Set initial visibility
            const bool enabled = (routeChannelBoxes[i].getSelectedId() != 1);
            routeParameterBoxes[i].setVisible(enabled);
            routeBipolarToggles[i]->setVisible(enabled);
            routeInvertToggles[i]->setVisible(enabled);
            routeOneShotToggles[i]->setVisible(noteRestartToggle->getToggleState());
        }

        // Initialize note source channel list
        updateNoteSourceChannel();

        // Initialize the atomic variable with current selection
        noteRestartChannel.store(noteSourceChannelBox.getSelectedId(), std::memory_order_release);

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
        addAndMakeVisible(midiMonitorButton);

        midiMonitorButton.setToggleable(true);
        midiMonitorButton.setClickingTogglesState(true);

        midiMonitorButton.onClick = [this]()
        {
            if (midiMonitorButton.getToggleState())
            {
                if (midiMonitorWindow == nullptr)
                    midiMonitorWindow = std::make_unique<MidiMonitorWindow>();

                midiMonitorWindow->setVisible(true);
                midiMonitorWindow->toFront(true);
            }
            else
            {
                if (midiMonitorWindow != nullptr)
                    midiMonitorWindow->setVisible(false);
            }
        };

        //bipolar check
        addAndMakeVisible(lfoRouteDebugLabel);
        lfoRouteDebugLabel.setJustificationType(juce::Justification::topLeft);
        lfoRouteDebugLabel.setFont(juce::Font(juce::FontOptions()
                                                    .withHeight(12.0f)
                                            ));

        lfoRouteDebugLabel.setColour(juce::Label::textColourId, juce::Colours::yellow);

        //EG check in ScopeRoute[0]
        addAndMakeVisible(showEGinScopeToggle);
        showEGinScopeToggle.setToggleState(showEGinScope, juce::dontSendNotification);
        showEGinScopeToggle.onClick = [this]()
            {
                showEGinScope = showEGinScopeToggle.getToggleState();
            };

        #endif

        // Timer
        startTimerHz(100); // 100Hz refresh  
    }

    ~MainComponent() override
    {
        stopTimer();
        midiClock.stop();
        midiOut.reset();
        rateSlider.setLookAndFeel (nullptr);
        depthSlider.setLookAndFeel (nullptr);
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

        placeRow(midiOutputLabel, midiOutputBox);
        placeRow(midiInputLabel, midiInputBox);
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

        #if JUCE_DEBUG
        auto placeDebugRow = [&](juce::Label& title, juce::Label& midiValues)
        {
            auto row = lfoAreaContent.removeFromTop(rowHeight);
            title.setBounds(row.removeFromLeft(labelWidth));
            row.removeFromLeft(spacing);
            midiValues.setBounds(row);
            lfoAreaContent.removeFromTop(6);
        };
        placeDebugRow(noteDebugTitle, noteDebugLabel);
        #endif

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

        //MIDI MONITOR
        #if JUCE_DEBUG
        auto areaMon = getLocalBounds();

        constexpr int buttonWidth  = 110;
        constexpr int buttonHeight = 22;
        constexpr int margin       = 8;
        constexpr int marginLeft   = 500;

        midiMonitorButton.setBounds(
            areaMon.getRight() - buttonWidth  - marginLeft,
            areaMon.getBottom() - buttonHeight - margin,
            buttonWidth,
            buttonHeight
        );

        // bipolar check
        if (showRouteDebugLabel)
            lfoRouteDebugLabel.setBounds(10, getHeight() - 120, 300, 100);
        #endif

        // Envelop generator frame
        if (envelopeComponent != nullptr)
            envelopeComponent->setBounds(egColumn);
    }

    void postJuceInit()
    {
        refreshMidiInputs();
        refreshMidiOutputs();
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
    juce::GroupComponent lfoGroup;

    juce::Label midiOutputLabel, midiInputLabel, syncModeLabel;
    juce::Label bpmLabelTitle, bpmLabel, divisionLabel;
    juce::Label parameterLabel, shapeLabel, rateLabel, depthLabel, channelLabel, bipolarLabel, invertPhaseLabel, oneShotLabel;

    juce::ComboBox midiOutputBox, midiInputBox, syncModeBox, divisionBox;
    juce::ComboBox shapeBox;

    // SLiders
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    juce::Slider rateSlider, depthSlider;

    //Note-On retrig on/off and source channel
    std::unique_ptr<LedToggleButton> noteRestartToggle, noteOffStopToggle;
    juce::Label noteRestartToggleLabel, noteOffStopToggleLabel;

    juce::ComboBox noteSourceChannelBox; // source channel for Note-On listening (lfo)

    juce::TextButton startButton;

    // MIDI
    std::unique_ptr<juce::MidiOutput> midiOut;
    MidiClockHandler midiClock;

    std::unique_ptr<juce::MidiInput> globalMidiInput;
    std::unique_ptr<juce::MidiInputCallback> midiCallback;

    std::atomic<bool> pendingNoteOn { false };
    std::atomic<bool> pendingNoteOff { false };
    std::atomic<int>  pendingNoteChannel { 0 };
    std::atomic<int>  pendingNoteNumber { 0 };
    std::atomic<float>  pendingNoteVelocity { 0 };

    std::atomic<int> noteRestartChannel { 0 }; // 1–16, 0 = disabled

    std::atomic<bool> requestLfoRestart { false };
    std::atomic<bool> requestLfoStop { false };

    //DEBUG
    #if JUCE_DEBUG
    // Debug: show last Note-On received
    juce::Label noteDebugTitle { {}, "Last Note-On:" };
    juce::Label noteDebugLabel;
    #endif

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

    std::array<double, maxRoutes> lfoPhase { 0.0, 0.0, 0.0 };

    #if JUCE_DEBUG
    std::unique_ptr<MidiMonitorWindow> midiMonitorWindow;
    juce::TextButton midiMonitorButton { "MIDI Monitor" };

    //bipolar check
    juce::Label lfoRouteDebugLabel;
    bool showRouteDebugLabel = false;

    // EG test: to scope Route 0.
    juce::ToggleButton showEGinScopeToggle{ "EG to Scope" };

    bool showEGinScope = false;
    #endif

    enum class LfoShape
    {
        Sine = 1,
        Triangle,
        Square,
        Saw,
        Random
    };

    // Setting Pop-Up
    juce::TextButton settingsButton;

    // LFO State
    double phase = 0.0;
    double sampleRate = 100.0;
    juce::Random random;
    bool lfoActive = false;

    bool oneShotActive = false;

    double oneShotPhaseAccum = 0.0;

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
    std::unordered_map<int, int> lastSentValuePerParam;  // key: param ID or CC number
    int changeThreshold = 1; // difference needed before sending

    // settings - Anti flooding
    std::unordered_map<int, double> lastSendTimePerParam;
    double msFloofThreshold = 0.0; // delay between Midi datas chunk

    //MIDI MONITOR
    #if JUCE_DEBUG
    void settingsButtonClicked()
    {
        if (midiMonitorWindow == nullptr)
        {
            midiMonitorWindow = std::make_unique<MidiMonitorWindow>();
        }

        midiMonitorWindow->setVisible(true);
        midiMonitorWindow->toFront(true);
    }
    #endif

    void updateNoteSourceChannel()
    {
        // Store current selection before clearing
        const int currentSelection = noteSourceChannelBox.getSelectedId();
        
        // Clear without triggering onChange
        noteSourceChannelBox.clear(juce::dontSendNotification);

        juce::Array<int> activeChannels;

        // Collect unique active MIDI channels from routes
        for (const auto& route : lfoRoutes)
        {
            if (route.midiChannel > 0 && !activeChannels.contains(route.midiChannel))
                activeChannels.add(route.midiChannel);
        }

        // Populate selector
        for (int ch : activeChannels)
            noteSourceChannelBox.addItem("Ch " + juce::String(ch), ch);

        // Restore previous selection if possible, otherwise select first
        int newSelection = 0;
        if (activeChannels.contains(currentSelection))
        {
            newSelection = currentSelection;
            noteSourceChannelBox.setSelectedId(currentSelection, juce::dontSendNotification);
        }
        else if (!activeChannels.isEmpty())
        {
            newSelection = activeChannels[0];
            noteSourceChannelBox.setSelectedId(activeChannels[0], juce::dontSendNotification);
        }
        
        // IMPORTANT: Update the atomic variable since onChange won't fire with dontSendNotification
        noteRestartChannel.store(newSelection, std::memory_order_release);
    }

    void refreshMidiOutputs()
    {
        midiOutputBox.clear();
        auto devices = juce::MidiOutput::getAvailableDevices();
        for (int i = 0; i < devices.size(); ++i)
            midiOutputBox.addItem(devices[i].name, i + 1);
        #if JUCE_DEBUG
            midiOutputBox.setSelectedId(1);
        #endif
    }

    void refreshMidiInputs()
    {
        midiInputBox.clear();
        auto devices = juce::MidiInput::getAvailableDevices();
        for (int i = 0; i < devices.size(); ++i)
            midiInputBox.addItem(devices[i].name, i + 1);
        #if JUCE_DEBUG
            midiInputBox.setSelectedId(3);
        #endif
    }

    // MIDI Sync
    void handleIncomingMessage(const juce::MidiMessage& msg)
    {
        const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        if (!syncEnabled)
        {
            midiClock.stop();
            return;
        }

        // existing clock parsing here  midiInputBox
        int inIndex = midiInputBox.getSelectedId() - 1;
        midiClock.start(inIndex);
        return;
    }

    // Call to start/stop the MidiClockHandler based on UI state
    void updateMidiClockState()
    {
        const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        
        if (syncEnabled)
        {
            int inIndex = midiInputBox.getSelectedId() - 1;
            if (inIndex >= 0)
            {
                midiClock.start(inIndex);
            }
            else
            {
                // No valid input selected, stop the clock
                midiClock.stop();
            }
        }
        else
        {
            // Free mode - ensure the clock is stopped
            midiClock.stop();
        }
    }
    
    struct GlobalMidiCallback : public juce::MidiInputCallback
    {
        MainComponent& owner;
        GlobalMidiCallback(MainComponent& o) : owner(o) {}

        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& msg) override
        {
            // MIDI CLOCK
            if (owner.syncModeBox.getSelectedId() == 2)
            {
                owner.handleIncomingMessage(msg); 
            }

            if (msg.isNoteOn())
            {
                owner.pendingNoteChannel.store(msg.getChannel(), std::memory_order_relaxed);
                owner.pendingNoteNumber.store(msg.getNoteNumber(), std::memory_order_relaxed);
                owner.pendingNoteVelocity.store(msg.getFloatVelocity(), std::memory_order_relaxed);
                owner.pendingNoteOn.store(true, std::memory_order_release);
            }
            else if (msg.isNoteOff())
            {
                owner.pendingNoteChannel.store(msg.getChannel(), std::memory_order_relaxed);
                owner.pendingNoteOff.store(true, std::memory_order_release);

                // Request LFO stop only if UI allows it
                if (owner.noteRestartToggle->getToggleState()
                    && owner.noteOffStopToggle->getToggleState())
                {
                    owner.requestLfoStop.store(true, std::memory_order_release);
                }
            }
        }
    };

    void updateMidiInput()
    {
        if (globalMidiInput)
        {
            globalMidiInput->stop();
            globalMidiInput.reset();
        }

        auto inputs = juce::MidiInput::getAvailableDevices();
        int index = midiInputBox.getSelectedId() - 1;
        if (index < 0 || index >= inputs.size())
        {
            // Stop the clock if no valid input selected
            midiClock.stop();
            return;
        }

        midiCallback = std::make_unique<GlobalMidiCallback>(*this);
        globalMidiInput = juce::MidiInput::openDevice(inputs[index].identifier,
                                                       midiCallback.get());
        if (globalMidiInput)
            globalMidiInput->start();
    }

    void toggleLfo()
    {
        if (lfoActive)
        {
            // reset LFO value
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];
                lfoPhase[i] = getWaveformStartPhase(
                    shapeBox.getSelectedId(),
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
                );

                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
            
            lfoActive = false;
            startButton.setButtonText("Start LFO");
        }
        else
        {
            // Clock may still be needed for sync
            updateMidiClockState();
            // reset LFO value
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];
                lfoPhase[i] = getWaveformStartPhase(
                    shapeBox.getSelectedId(),
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
                );

                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
            
            lfoActive = true;
            startButton.setButtonText("Stop LFO");
        }
    }

    // Timer Callback
    void timerCallback() override
    {
        if (!midiOut)
            return;

        // Note messages
        if (pendingNoteOn.exchange(false))
        {
            const int ch   = pendingNoteChannel.load();
            const int note = pendingNoteNumber.load();
            const float velocity = pendingNoteVelocity.load();

            // --- EG ---
            if (envelopeComponent && envelopeComponent->isEgEnabled())
                envelopeComponent->noteOn(ch, note, velocity);

            // --- LFO Note Restart ---
            const int restartCh = noteRestartChannel.load(std::memory_order_acquire);

            if (noteRestartToggle->getToggleState()
                && restartCh > 0
                && ch == restartCh)
            {
                requestLfoRestart.store(true, std::memory_order_release);
                
                #if JUCE_DEBUG
                noteDebugLabel.setText("NoteOn: Ch " + juce::String(ch) +
                                       " | Note " + juce::String(note),
                                       juce::dontSendNotification);
               #endif
            }
        }

        // EG trig
        if (pendingNoteOff.exchange(false))
        {
            const int ch = pendingNoteChannel.load();
            const int note = pendingNoteOff.load();

            if (envelopeComponent && envelopeComponent->isEgEnabled())
                envelopeComponent->noteOff(ch, note);
        }

        // Stop LFO on Note-Off
        if (requestLfoStop.exchange(false))
        {
            lfoActive = false;
            startButton.setButtonText("Start LFO");

            // reset phases for next start
            for (int i = 0; i < maxRoutes; ++i)
            {
                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }
        }

        // LFO
        const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        const double bpm = midiClock.getCurrentBPM();
        int egValue;

        // Handle pending retrigger from Note-On
        if (requestLfoRestart.exchange(false))
        {
            // reset LFO value
            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];
                lfoPhase[i] = getWaveformStartPhase(
                    shapeBox.getSelectedId(),
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
                );

                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }

            if (!lfoActive)
            {
                lfoActive = true;
                startButton.setButtonText("Stop LFO");
            }

        }

        // Always update BPM display if sync mode is active
        if (syncEnabled)
        {
            const double bpm = midiClock.getCurrentBPM();
            const auto nowMs = juce::Time::getMillisecondCounterHiRes();

            if (bpm > 0.0)
            {
                // Smooth & rate-limit UI updates
                displayedBpm = 0.9 * displayedBpm + 0.1 * bpm;
                if (nowMs - lastBpmUpdateMs > 250.0)
                {
                    bpmLabel.setText(juce::String(displayedBpm, 1), juce::dontSendNotification);
                    lastBpmUpdateMs = nowMs;
                }
            }
            else
            {
                // No clock yet: show placeholder
                if (nowMs - lastBpmUpdateMs > 500.0)
                {
                    bpmLabel.setText("--", juce::dontSendNotification);
                    lastBpmUpdateMs = nowMs;
                }
            }
        }
        else
        {
            // When not in sync mode, just freeze BPM display
        }

        if (lfoActive)
        {
            // Compute current rate
            double rateHz = rateSlider.getValue();

            if (syncEnabled && bpm > 0.0)
            {
                rateHz = updateLfoRateFromBpm(rateHz);
            }                

            // Generate and send LFO values
            const double phaseInc = rateHz / sampleRate;
            const auto shapeId = static_cast<LfoShape>(shapeBox.getSelectedId());

            for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];

                if (route.midiChannel <= 0 || route.parameterIndex < 0)
                    continue;

                if (route.oneShot && route.hasFinishedOneShot)
                    continue;

                const bool wrapped = advancePhase(lfoPhase[i], phaseInc);

                double shape = computeWaveform(shapeId,
                                                lfoPhase[i],
                                                route.bipolar,
                                                route.invertPhase,
                                                random
                                            );

                // One-shot logic
                if (route.oneShot)
                {
                    if (route.bipolar)
                    {
                        if (wrapped)
                            route.hasFinishedOneShot = true;
                    }
                    else
                    {
                        if (!route.passedPeak && shape >= 0.999)
                            route.passedPeak = true;

                        if (route.passedPeak && shape <= -0.999)
                            route.hasFinishedOneShot = true;
                    }
                }

                // Mapping
                const auto& param = syntaktParameters[route.parameterIndex];
                const double depth = depthSlider.getValue();

                int midiVal = 0;

                if (route.bipolar)
                {
                    const int center = (param.minValue + param.maxValue) / 2;
                    const int range  = (param.maxValue - param.minValue) / 2;

                    midiVal = center + int(std::round(shape * depth * range));
                }
                else
                {
                    const double uni = juce::jlimit(0.0, 1.0, (shape + 1.0) * 0.5);
                    midiVal = param.minValue
                            + int(std::round(uni * depth * (param.maxValue - param.minValue)));
                }

                midiVal = juce::jlimit(param.minValue, param.maxValue, midiVal);

                sendThrottledParamValue(i, route.midiChannel, param, midiVal);

                // Oscilloscope
                if (lfoRoutesToScope[i])
                {
                    lastLfoRoutesValues[i].store(shape * depth, std::memory_order_relaxed);
                }
            }
        }
        
        if (envelopeComponent && envelopeComponent->isEgEnabled())
        {
            double egMIDIvalue = 0.0;
            
            if (envelopeComponent->tick(egMIDIvalue))
            {
                #if JUCE_DEBUG
                    float egScopeValue = static_cast<float>(egMIDIvalue * 2.0 - 1.0);
                    // 0.0 → -1.0
                    // 1.0 → +1.0
                    if ( showEGinScope )
                    {
                        // ---- SCOPE DEBUG TAP (temporary) ----
                        lastLfoRoutesValues[0].store(
                            static_cast<float>(egMIDIvalue * 2.0 - 1.0),
                            std::memory_order_relaxed
                        );
                        // -------------------------------------
                    }
                #endif

                const int paramId = envelopeComponent->selectedEgOutParamsId();
                const int egValue = mapEgToMidi(egMIDIvalue, paramId);
                const int egCh    = envelopeComponent->selectedEgOutChannel();

                if (egCh > 0 && paramId >= 0)
                {
                    const auto& param = syntaktParameters[paramId];

                    sendThrottledParamValue(
                        0x7FFF,
                        egCh,
                        param,
                        egValue
                    );
                }
            }
        }

        #if JUCE_DEBUG
            if (showRouteDebugLabel)
                updateLfoRouteDebugLabel();
        #endif
    }

    inline bool advancePhase(double& phase, double phaseInc)
    {
        phase += phaseInc;
        if (phase >= 1.0)
        {
            phase -= 1.0;
            return true;
        }
        return false;
    }

    // waveforms
    inline double lfoSine(double phase)
    {
        return std::sin(juce::MathConstants<double>::twoPi * phase);
    }

    inline double lfoTriangle(double phase)
    {
        // canonical triangle: 0 → +1 → 0 → -1 → 0
        double t = phase - std::floor(phase);
        return 4.0 * std::abs(t - 0.5) - 1.0;
    }

    inline double lfoSquare(double phase)
    {
        return (phase < 0.5) ? 1.0 : -1.0;
    }

    inline double lfoSaw(double phase)
    {
        return 2.0 * phase - 1.0;
    }

    inline double lfoRandom(double phase, juce::Random& rng)
    {
        static double lastPhase = 0.0;
        static double lastValue = 0.0;

        // detect phase wrap
        if (phase < lastPhase) 
        {
            lastValue = rng.nextDouble() * 2.0 - 1.0;
        }

        lastPhase = phase;
        return lastValue;
    }

    double computeWaveform(LfoShape shape,
                           double phase,
                           bool bipolar,
                           bool invertPhase,
                           juce::Random& rng)
    {
        // true phase inversion (180°)
        if (invertPhase && shape != LfoShape::Saw)
        {
            phase += 0.5;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (invertPhase && (shape == LfoShape::Saw))
        {
            phase = -phase;
            if (phase <= 1.0)
                phase += 1.0;
        }

        // phase alignment per shape
        if (shape == LfoShape::Triangle && !bipolar)
        {
            phase += 0.25;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (shape == LfoShape::Triangle && bipolar)
        {
            phase -= 0.25;
            if (phase >= 1.0)
                phase -= 1.0;
        }

        if (shape == LfoShape::Saw && bipolar)
        {
            phase += 0.5;
            if (phase >= 1.0)
                phase -= 1.0;
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

    // ensure that LFO Waveforms start from correct offset (bipolar/unipolar)
    double getWaveformStartPhase(int shapeId, bool bipolar, bool invert) const
    {
        double phase = 0.0;

        if (!bipolar)
        {
            switch (static_cast<LfoShape>(shapeId))
            {
                case LfoShape::Sine:     phase = 0.75; break; // -1
                case LfoShape::Triangle: phase = 0.25; break; // -1 ✅ FIX
                case LfoShape::Square:   phase = 0.5;  break; // -1
                case LfoShape::Saw:      phase = 0.0;  break; // -1
                default: break;
            }
        }

        return phase;
    }

    // shared throttling and MIDI send function
    void sendThrottledParamValue(
                                int routeIndex,              // for unique throttle key
                                int midiChannel,
                                const SyntaktParameter& param,
                                int midiValue)
    {
        // Build per-route + per-parameter key
        const int paramKey =
            (routeIndex << 16) |
            (param.isCC ? 0x1000 : 0x2000) |
            (param.isCC ? param.ccNumber
                        : ((param.nrpnMsb << 7) | param.nrpnLsb));

        // Value change threshold
        const int lastVal = lastSentValuePerParam[paramKey];
        if (std::abs(midiValue - lastVal) < changeThreshold)
            return;

        lastSentValuePerParam[paramKey] = midiValue;

        // Time-based anti-flood
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastSendTimePerParam[paramKey] < msFloofThreshold)
            return;

        lastSendTimePerParam[paramKey] = now;

        // Split value if NRPN
        const int valueMSB = (midiValue >> 7) & 0x7F;
        const int valueLSB = midiValue & 0x7F;

        const bool monitorInput = false;

        if (param.isCC)
        {
            auto msg = juce::MidiMessage::controllerEvent(
                midiChannel, param.ccNumber, midiValue);

            midiOut->sendMessageNow(msg);

            #if JUCE_DEBUG
            if (midiMonitorWindow)
                midiMonitorWindow->pushEvent(msg, monitorInput);
            #endif
        }

        else
        {
            auto send = [&](int cc, int val)
            {
                auto msg = juce::MidiMessage::controllerEvent(midiChannel, cc, val);
                midiOut->sendMessageNow(msg);

                #if JUCE_DEBUG
                if (midiMonitorWindow)
                    midiMonitorWindow->pushEvent(msg, monitorInput);
                #endif
            };

            send(99, param.nrpnMsb);
            send(98, param.nrpnLsb);
            send(6,  valueMSB);
            send(38, valueLSB);
        }
    }

    // MIDI Transport Callbacks
    void handleMidiStart() override
    {
        // Reset LFO phase when sequencer starts
        for (int i = 0; i < maxRoutes; ++i)
        {
            auto& route = lfoRoutes[i];
            lfoPhase[i] = getWaveformStartPhase(
                    shapeBox.getSelectedId(),
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
            );

            lfoRoutes[i].hasFinishedOneShot = false;
            lfoRoutes[i].passedPeak = false;
        }

        lfoActive = false;
        // passing via the UI start/stop function to avoid decoherence between HW and UI
        toggleLfo();
    }

    void handleMidiStop() override
    {
        // Reset LFO to get a clean start even is the LFO is restarted from the UI
        for (int i = 0; i < maxRoutes; ++i)
            {
                auto& route = lfoRoutes[i];
                lfoPhase[i] = getWaveformStartPhase(
                    shapeBox.getSelectedId(),
                    lfoRoutes[i].bipolar,
                    lfoRoutes[i].invertPhase
                );

                lfoRoutes[i].hasFinishedOneShot = false;
                lfoRoutes[i].passedPeak = false;
            }

        lfoActive = true;
        toggleLfo();
    }

    double updateLfoRateFromBpm(double rateHz)
    {
        const double bpm = midiClock.getCurrentBPM();
        const bool syncEnabled = (syncModeBox.getSelectedId() == 2);
        if (syncEnabled && bpm > 0.0)
        {
            rateHz = bpmToHz(bpm);

            rateSlider.setValue(rateHz, juce::dontSendNotification);
        }
            
        return rateHz;
    }

    // BPM → Frequency Conversion
    double bpmToHz(double bpm)
    {
        if (bpm <= 0.0)
            return 0.0;

        // Division multiplier relative to 1 beat = quarter note
        double multiplier = 1.0;

        switch (divisionBox.getSelectedId())
        {
            case 1: multiplier = 0.25; break;  // whole note (4 beats per cycle)
            case 2: multiplier = 0.5;  break;  // half note
            case 3: multiplier = 1.0;  break;  // quarter note
            case 4: multiplier = 2.0;  break;  // eighth
            case 5: multiplier = 4.0;  break;  // sixteenth
            case 6: multiplier = 8.0;  break;  // thirty-second
            case 7: multiplier = 2.0 / 1.5; break;  // dotted ⅛ (triplet-based)
            case 8: multiplier = 4.0 / 1.5; break;  // dotted 1/16
            default: break;
        }

        // base beat frequency = beats per second
        const double beatsPerSecond = bpm / 60.0;

        // final LFO frequency in Hz
        return beatsPerSecond * multiplier;
    }

    // Map EG value to MIDI
    int mapEgToMidi(double egValue, int paramId)
    {
        const auto& param = syntaktParameters[paramId];

        if (param.isBipolar)
        {
            // centered mapping
            const double center = (param.minValue + param.maxValue) * 0.5;
            const double range  = (param.maxValue - param.minValue) * 0.5;
            return (int)(center + (egValue * 2.0 - 1.0) * range);
        }
        else
        {
            return (int)(param.minValue + egValue * (param.maxValue - param.minValue));
        }
    }

    void openSelectedMidiOutput()
    {
        midiOut.reset();

        auto outputs = juce::MidiOutput::getAvailableDevices();
        const int outIndex = midiOutputBox.getSelectedId() - 1;

        if (outIndex >= 0 && outIndex < outputs.size())
        {
            midiOut = juce::MidiOutput::openDevice(outputs[outIndex].identifier);
        }
    }

    #if JUCE_DEBUG
    void updateLfoRouteDebugLabel()
    {
        if (showRouteDebugLabel)
        {
            juce::String text;

        text << "LFO Routes bipolar state:\n";

        for (int i = 0; i < maxRoutes; ++i)
        {
            const auto& r = lfoRoutes[i];

            text << "Route " << i
                 << " | ch=" << r.midiChannel
                 << " | param=" << r.parameterIndex
                 << " | bipolar=" << (r.bipolar ? "TRUE" : "FALSE")
                 << " | invert=" << (r.invertPhase ? "TRUE" : "FALSE")
                 << " | oneShot=" << (r.oneShot ? "TRUE" : "FALSE")
                 << "\n";
        }

        lfoRouteDebugLabel.setText(text, juce::dontSendNotification);
        }
    }
    #endif
};
