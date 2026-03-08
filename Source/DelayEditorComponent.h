#pragma once
#include <JuceHeader.h>

#include "Cosmetic.h"

class DelayEditorComponent : public juce::Component, private juce::Timer
{
public:
    using APVTS          = juce::AudioProcessorValueTreeState;
    using SliderAttachment = APVTS::SliderAttachment;
    using ButtonAttachment = APVTS::ButtonAttachment;
    using ChoiceAttachment = APVTS::ComboBoxAttachment;

    static constexpr int maxRoutes = 3;

    // ─────────────────────────────────────────────────────────────────────────
    DelayEditorComponent (APVTS& apvtsRef)
        : apvts (apvtsRef)
    {
        setName ("Delay");

        // ── Group frame ───────────────────────────────────────────────────────
        addAndMakeVisible (delayGroup);
        delayGroup.setText ("DELAY");
        delayGroup.setColour (juce::GroupComponent::outlineColourId, juce::Colours::white);
        delayGroup.setColour (juce::GroupComponent::textColourId,    juce::Colours::white);

        // ── Enable toggle ─────────────────────────────────────────────────────
        delayEnable = std::make_unique<LedToggleButton> ("Delay", SetupUI::LedColour::Red);
        delayEnable->onClick = [this]
        {
            const bool on = delayEnable->getToggleState();
            delayEnableLabel.setText (on ? "Enabled" : "Disabled",
                                      juce::dontSendNotification);
        };
        addAndMakeVisible (*delayEnable);
        delayEnableAttach = std::make_unique<ButtonAttachment> (apvts, "delayEnabled",
                                                                *delayEnable);

        delayEnableLabel.setText ("Disabled", juce::dontSendNotification);
        delayEnableLabel.setJustificationType (juce::Justification::centredLeft);
        delayEnableLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (delayEnableLabel);

        // ── Note Source channel ───────────────────────────────────────────────
        noteSourceDelayChannelLabel.setText ("Note Source", juce::dontSendNotification);
        noteSourceDelayChannelLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (noteSourceDelayChannelLabel);

        addAndMakeVisible (noteSourceDelayChannelBox);
        for (int ch = 1; ch <= 16; ++ch)
            noteSourceDelayChannelBox.addItem ("Ch " + juce::String (ch), ch);

        noteSourceDelayChannelAttach = std::make_unique<ChoiceAttachment> (
            apvts, "delayNoteSourceChannel", noteSourceDelayChannelBox);

        // ── Delay Sync combobox ───────────────────────────────────────────────
        // "Free" means use the slider value; any other choice locks the delay
        // time to a BPM-derived interval (requires an active clock source).
        delaySyncLabel.setText ("Sync", juce::dontSendNotification);
        delaySyncLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (delaySyncLabel);

        addAndMakeVisible (delaySyncBox);
        delaySyncBox.addItem ("Free",       1);
        delaySyncBox.addItem ("1/1",        2);
        delaySyncBox.addItem ("1/2",        3);
        delaySyncBox.addItem ("1/4",        4);
        delaySyncBox.addItem ("1/8",        5);
        delaySyncBox.addItem ("1/16",       6);
        delaySyncBox.addItem ("1/32",       7);
        delaySyncBox.addItem ("1/8 dot",    8);
        delaySyncBox.addItem ("1/16 dot",   9);

        delaySyncAttach = std::make_unique<ChoiceAttachment> (
            apvts, "delaySyncDivision", delaySyncBox);

        // ── Delay Rate slider ─────────────────────────────────────────────────
        // Create the APVTS attachment first (it sets the slider range from the param).
        delayRateAttach = std::make_unique<SliderAttachment> (apvts, "delayRate",
                                                              delayRateSlider);
        setupDelayRateSlider();

        // ── Feedback slider ───────────────────────────────────────────────────
        feedbackAttach = std::make_unique<SliderAttachment> (apvts, "feedback",
                                                             feedbackSlider);
        setupFeedbackSlider();

        // ── EG Shaping radio buttons ──────────────────────────────────────────
        // Three mutually exclusive toggle buttons backed by a single APVTS
        // AudioParameterChoice "delayEgShape":
        //   0 = Off
        //   1 = EG Volume     — global EG applied to "Amp: Volume" CC
        //   2 = EG Track Level — global EG applied to "Track Level" CC
        //   3 = Per Note EG   — each echo retriggers its own independent EG

        egVolumeBtn   = std::make_unique<LedToggleButton> ("EG Volume",    SetupUI::LedColour::Blue);
        egTrackLvlBtn = std::make_unique<LedToggleButton> ("EG Track Level", SetupUI::LedColour::Blue);
        egPerNoteBtn  = std::make_unique<LedToggleButton> ("Per Note EG",  SetupUI::LedColour::Green);

        egVolumeBtn->setClickingTogglesState (true);
        egTrackLvlBtn->setClickingTogglesState (true);
        egPerNoteBtn->setClickingTogglesState (true);

        addAndMakeVisible (*egVolumeBtn);
        addAndMakeVisible (*egTrackLvlBtn);
        addAndMakeVisible (*egPerNoteBtn);

        // egVolumeBtn / egTrackLvlBtn: mutually exclusive radio pair.
        // They share "delayEgShape" (0=Off, 1=EG Volume, 2=EG Track Level).
        egVolumeBtn->onClick = [this]
        {
            if (egVolumeBtn->getToggleState())
            {
                egTrackLvlBtn->setToggleState (false, juce::dontSendNotification);
                setDelayEgShapeParam (1);
            }
            else { setDelayEgShapeParam (0); }
            // Immediately purge any delay routes that now conflict.
            enforceDelayRouteConflicts();
        };

        egTrackLvlBtn->onClick = [this]
        {
            if (egTrackLvlBtn->getToggleState())
            {
                egVolumeBtn->setToggleState (false, juce::dontSendNotification);
                setDelayEgShapeParam (2);
            }
            else { setDelayEgShapeParam (0); }
            // Immediately purge any delay routes that now conflict.
            enforceDelayRouteConflicts();
        };

        // egPerNoteBtn: fully independent toggle — does NOT affect delayEgShape.
        // Backed directly by APVTS "delayEgPerNote" via ButtonAttachment.
        egPerNoteAttach = std::make_unique<ButtonAttachment> (
            apvts, "delayEgPerNote", *egPerNoteBtn);

        egVolumeBtnLabel.setText ("EG Volume",    juce::dontSendNotification);
        egVolumeBtnLabel.setJustificationType (juce::Justification::centredLeft);
        egVolumeBtnLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (egVolumeBtnLabel);

        egTrackLvlBtnLabel.setText ("EG Trk Level", juce::dontSendNotification);
        egTrackLvlBtnLabel.setJustificationType (juce::Justification::centredLeft);
        egTrackLvlBtnLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (egTrackLvlBtnLabel);

        egPerNoteBtnLabel.setText ("Per Note EG", juce::dontSendNotification);
        egPerNoteBtnLabel.setJustificationType (juce::Justification::centredLeft);
        egPerNoteBtnLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (egPerNoteBtnLabel);

        // ── Output route channel boxes + transpose sliders ────────────────────
        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs = juce::String (r);

            delayRouteLabel[r].setText ("Route " + juce::String (r + 1),
                                        juce::dontSendNotification);
            delayRouteLabel[r].setColour (juce::Label::textColourId, SetupUI::labelsColor);
            addAndMakeVisible (delayRouteLabel[r]);

            // ComboBox IDs: 1 = Disabled, 2..17 = Ch1..Ch16
            delayRouteChannelBox[r].addItem ("Disabled", 1);
            for (int ch = 1; ch <= 16; ++ch)
                delayRouteChannelBox[r].addItem ("Ch " + juce::String (ch), ch + 1);
            addAndMakeVisible (delayRouteChannelBox[r]);

            delayRouteChannelAttach[r] = std::make_unique<ChoiceAttachment> (
                apvts, "delayRoute" + rs + "_channel", delayRouteChannelBox[r]);

            // Transpose slider  (-24 .. +24 semitones, integer steps)
            // Attachment is created BEFORE setupTransposeSlider so APVTS sets initial value.
            delayRouteTransposeAttach[r] = std::make_unique<SliderAttachment> (
                apvts, "delayRoute" + rs + "_transpose", delayRouteTransposeSlider[r]);
            setupTransposeSlider (delayRouteTransposeSlider[r]);
        }

        // ── Step sequencer ────────────────────────────────────────────────────
        // Sub-frame
        addAndMakeVisible (seqGroup);
        seqGroup.setText ("Echo Seq");
        seqGroup.setColour (juce::GroupComponent::outlineColourId, SetupUI::labelsColor);
        seqGroup.setColour (juce::GroupComponent::textColourId,    SetupUI::labelsColor);

        // Binary / Ternary radio pair — always both visible, one always active.
        // No ButtonAttachment: state is written to "delaySeqTernary" manually,
        // mirroring the pattern used by egVolumeBtn / egTrackLvlBtn.
        seqBinaryBtn  = std::make_unique<LedToggleButton> ("Binary",  SetupUI::LedColour::Red);
        seqTernaryBtn = std::make_unique<LedToggleButton> ("Ternary", SetupUI::LedColour::Red);
        seqBinaryBtn ->setClickingTogglesState (true);
        seqTernaryBtn->setClickingTogglesState (true);
        addAndMakeVisible (*seqBinaryBtn);
        addAndMakeVisible (*seqTernaryBtn);

        seqBinaryBtn->onClick = [this]
        {
            seqTernaryBtn->setToggleState (false, juce::dontSendNotification);
            seqBinaryBtn ->setToggleState (true,  juce::dontSendNotification);
            setSeqTernaryParam (false);
        };
        seqTernaryBtn->onClick = [this]
        {
            seqBinaryBtn ->setToggleState (false, juce::dontSendNotification);
            seqTernaryBtn->setToggleState (true,  juce::dontSendNotification);
            setSeqTernaryParam (true);
        };

        seqBinaryLabel.setText  ("4/4",  juce::dontSendNotification);
        seqTernaryLabel.setText ("3/4", juce::dontSendNotification);
        seqBinaryLabel.setJustificationType  (juce::Justification::centredLeft);
        seqTernaryLabel.setJustificationType (juce::Justification::centredLeft);
        seqBinaryLabel.setColour  (juce::Label::textColourId, SetupUI::labelsColor);
        seqTernaryLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        addAndMakeVisible (seqBinaryLabel);
        addAndMakeVisible (seqTernaryLabel);

        // Step buttons — backed by individual APVTS bools.
        for (int s = 0; s < maxSeqSteps; ++s)
        {
            seqStepBtn[s] = std::make_unique<LedToggleButton> (
                juce::String (s + 1), SetupUI::LedColour::Orange);
            seqStepBtn[s]->setClickingTogglesState (true);
            addAndMakeVisible (*seqStepBtn[s]);
            seqStepAttach[s] = std::make_unique<ButtonAttachment> (
                apvts, "delaySeqStep" + juce::String (s), *seqStepBtn[s]);
        }

        startTimerHz (20);
    }

    // ─────────────────────────────────────────────────────────────────────────
    ~DelayEditorComponent() override
    {
        stopTimer();

        // Reset all APVTS attachments before components are destroyed.
        delayEnableAttach.reset();
        noteSourceDelayChannelAttach.reset();
        delaySyncAttach.reset();
        delayRateAttach.reset();
        feedbackAttach.reset();
        egPerNoteAttach.reset();
        for (int r = 0; r < maxRoutes; ++r)
        {
            delayRouteChannelAttach[r].reset();
            delayRouteTransposeAttach[r].reset();
        }

        for (int s = 0; s < maxSeqSteps; ++s)
            seqStepAttach[s].reset();
    }

    // ─────────────────────────────────────────────────────────────────────────
    void resized() override
    {
        if (getWidth() <= 0 || getHeight() <= 0)
            return;

        auto area = getLocalBounds();
        delayGroup.setBounds (area);

        auto content = area.reduced (10, 24);

        constexpr int rowHeight  = 24;
        constexpr int labelWidth = 90;
        constexpr int spacing    = 6;

        // Helper: place a label + component on one row.
        auto placeRow = [&] (juce::Label& label, juce::Component& comp)
        {
            auto row = content.removeFromTop (rowHeight);
            label.setBounds (row.removeFromLeft (labelWidth));
            row.removeFromLeft (spacing);
            comp.setBounds (row);
            content.removeFromTop (6);
        };

        // ── Enable toggle row ─────────────────────────────────────────────────
        {
            auto row = content.removeFromTop (rowHeight + 4);
            juce::FlexBox fb;
            fb.flexDirection  = juce::FlexBox::Direction::row;
            fb.alignItems     = juce::FlexBox::AlignItems::center;
            fb.justifyContent = juce::FlexBox::JustifyContent::center;
            fb.items.add (juce::FlexItem (*delayEnable)
                              .withWidth (22).withHeight (rowHeight)
                              .withMargin ({ 0, 4, 0, 0 }));
            fb.items.add (juce::FlexItem (delayEnableLabel)
                              .withWidth (100).withHeight (rowHeight)
                              .withMargin ({ 0, 8, 0, 0 }));
            fb.performLayout (row);
        }

        content.removeFromTop (12);

        // ── Note Source channel ───────────────────────────────────────────────
        placeRow (noteSourceDelayChannelLabel, noteSourceDelayChannelBox);

        // ── Sync division ─────────────────────────────────────────────────────
        placeRow (delaySyncLabel, delaySyncBox);

        content.removeFromTop (10);

        // ── Sliders ───────────────────────────────────────────────────────────
        placeRow (delayRateLabel, delayRateSlider);
        placeRow (feedbackLabel,  feedbackSlider);

        content.removeFromTop (14);

        constexpr float btnW  = 20.0f;

        // ── EG Shaping radio rows ─────────────────────────────────────────────
        // Row A:  [●] EG Volume   [●] EG Trk Level
        // Row B:  [●] Per Note EG
        // Two rows keep each label readable without widening the panel.
        {
            
            constexpr float lblW  = 76.0f;
            constexpr float gap   = 8.0f;

            auto rowA = content.removeFromTop (rowHeight);
            {
                juce::FlexBox fb;
                fb.flexDirection  = juce::FlexBox::Direction::row;
                fb.alignItems     = juce::FlexBox::AlignItems::center;
                fb.justifyContent = juce::FlexBox::JustifyContent::center;

                fb.items.add (juce::FlexItem (*egVolumeBtn)
                                  .withWidth (btnW).withHeight ((float)(rowHeight - 4))
                                  .withMargin ({ 0, 4, 0, 8 }));
                fb.items.add (juce::FlexItem (egVolumeBtnLabel)
                                  .withWidth (lblW).withHeight ((float) rowHeight)
                                  .withMargin ({ 0, 2, 0, 0 }));
                fb.items.add (juce::FlexItem (*egTrackLvlBtn)
                                  .withWidth (btnW).withHeight ((float)(rowHeight - 4))
                                  .withMargin ({ 0, gap, 0, 0 }));
                fb.items.add (juce::FlexItem (egTrackLvlBtnLabel)
                                  .withFlex (1.0f).withHeight ((float) rowHeight)
                                  .withMargin ({ 0, 2, 0, 0 }));
                fb.items.add (juce::FlexItem (*egPerNoteBtn)
                                  .withWidth (btnW).withHeight ((float)(rowHeight - 4))
                                  .withMargin ({ 0, 4, 0, 0 }));
                fb.items.add (juce::FlexItem (egPerNoteBtnLabel)
                                  .withFlex (1.0f).withHeight ((float) rowHeight)
                                  .withMargin ({ 0, 2, 0, 0 }));
                fb.performLayout (rowA.toFloat());
            }

            content.removeFromTop (4);

        }

        content.removeFromTop (24);

        // ── Step sequencer sub-frame ──────────────────────────────────────────
        // Two rows inside a GroupComponent titled "Echo Seq":
        //   Row 1: Binary / Ternary radio pair
        //   Row 2: step buttons (8 or 6 depending on mode)
        {
            const bool ternary    = apvts.getRawParameterValue ("delaySeqTernary")->load() > 0.5f;
            const int  activeSteps = ternary ? 6 : maxSeqSteps;

            // Total height: group title area (20) + ternary row + gap + steps row + bottom pad
            const int seqInnerH = rowHeight + 16 + rowHeight + 16;   // two rows + gap
            const int seqGroupH = 20 + seqInnerH + 8;          // title + content + bottom pad
            auto seqGroupArea = content.removeFromTop (seqGroupH);
            seqGroup.setBounds (seqGroupArea);

            // Content area inside the frame
            auto seqContent = seqGroupArea.reduced (8, 4);
            seqContent.removeFromTop (16);  // clear the title text area

            // Row 1: Binary [●]  [●] Ternary
            {
                auto row = seqContent.removeFromTop (rowHeight);
                juce::FlexBox fb;
                fb.flexDirection  = juce::FlexBox::Direction::row;
                fb.alignItems     = juce::FlexBox::AlignItems::center;
                fb.justifyContent = juce::FlexBox::JustifyContent::center;
                fb.items.add (juce::FlexItem (*seqBinaryBtn)
                                  .withWidth (btnW).withHeight ((float)(rowHeight - 4))
                                  .withMargin ({ 0, 4, 0, 4 }));
                fb.items.add (juce::FlexItem (seqBinaryLabel)
                                  .withWidth (48.0f).withHeight ((float) rowHeight)
                                  .withMargin ({ 0, 0, 0, 12 }));
                fb.items.add (juce::FlexItem (*seqTernaryBtn)
                                  .withWidth (btnW).withHeight ((float)(rowHeight - 4))
                                  .withMargin ({ 0, 4, 0, 4 }));
                fb.items.add (juce::FlexItem (seqTernaryLabel)
                                  .withWidth (48.0f).withHeight ((float) rowHeight));
                fb.performLayout (row.toFloat());
            }

            seqContent.removeFromTop (16);

            // Row 2: step buttons, equally spaced
            {
                auto row = seqContent.removeFromTop (rowHeight);
                juce::FlexBox fb;
                fb.flexDirection  = juce::FlexBox::Direction::row;
                fb.alignItems     = juce::FlexBox::AlignItems::center;
                fb.justifyContent = juce::FlexBox::JustifyContent::center;
                for (int s = 0; s < maxSeqSteps; ++s)
                {
                    const bool visible = (s < activeSteps);
                    seqStepBtn[s]->setVisible (visible);
                    if (visible)
                        fb.items.add (juce::FlexItem (*seqStepBtn[s])
                                          .withWidth (btnW * 1.5).withHeight (btnW * 1.5)
                                          .withMargin ({ 3, 3, 3, 3 }));
                }
                fb.performLayout (row.toFloat());
            }
        }

        content.removeFromTop (6);

        // ── Output route rows ─────────────────────────────────────────────────
        // Each row: [Route N label | Channel combobox | Transpose slider]
        auto layoutRouteRow = [&] (juce::Rectangle<int> row, int r)
        {
            juce::FlexBox fb;
            fb.flexDirection  = juce::FlexBox::Direction::row;
            fb.alignItems     = juce::FlexBox::AlignItems::center;
            fb.justifyContent = juce::FlexBox::JustifyContent::flexStart;

            fb.items.add (juce::FlexItem (delayRouteLabel[r])
                              .withWidth (50.0f).withHeight ((float) rowHeight));
            fb.items.add (juce::FlexItem (delayRouteChannelBox[r])
                              .withWidth (80.0f).withHeight ((float) rowHeight)
                              .withMargin ({ 0, 6, 0, 0 }));
            fb.items.add (juce::FlexItem (delayRouteTransposeSlider[r])
                              .withFlex (1.0f).withHeight ((float) rowHeight)
                              .withMargin ({ 0, 8, 0, 0 }));

            fb.performLayout (row.toFloat());
        };

        const int routesHeight = (rowHeight + 8) * maxRoutes - 8;

        if (content.getHeight() > routesHeight + 10)
            content.removeFromBottom (10);

        auto routesArea = content.removeFromBottom (routesHeight);

        for (int r = 0; r < maxRoutes; ++r)
        {
            auto row = routesArea.removeFromTop (rowHeight);
            layoutRouteRow (row, r);
            routesArea.removeFromTop (8);
        }
    }

private:
    // ── Timer: keep UI state in sync with the enabled flag ───────────────────
    void timerCallback() override
    {
        updateDelayUiEnabledState();
    }

    void updateDelayUiEnabledState()
    {
        const bool enabled = apvts.getRawParameterValue ("delayEnabled")->load() > 0.5f;

        // "delaySyncDivision" choice index: 0 = Free, 1..8 = synced divisions.
        const bool synced =
            (int) apvts.getRawParameterValue ("delaySyncDivision")->load() > 0;

        // Master toggle always stays interactive.
        if (delayEnable)
            delayEnable->setEnabled (true);

        // Gate everything else on the master enabled flag.
        noteSourceDelayChannelBox.setEnabled (enabled);
        noteSourceDelayChannelLabel.setEnabled (enabled);
        delaySyncBox.setEnabled (enabled);
        delaySyncLabel.setEnabled (enabled);
        feedbackSlider.setEnabled (enabled);
        feedbackLabel.setEnabled (enabled);

        // The rate slider is only interactive when the module is enabled AND
        // not locked to a sync division.
        const bool rateEditable = enabled && !synced;
        delayRateSlider.setEnabled (rateEditable);
        delayRateLabel.setEnabled (rateEditable);

        for (int r = 0; r < maxRoutes; ++r)
        {
            delayRouteChannelBox[r].setEnabled (enabled);
            delayRouteLabel[r].setEnabled (enabled);
            delayRouteTransposeSlider[r].setEnabled (enabled);
        }

        // Cross-module conflict: when EG shaping is active, grey channels in
        // delayRouteChannelBox that are already claimed by LFO or EG routes for the
        // same destination parameter (Amp: Volume or Track Level).
        // enforceDelayRouteConflicts() has already cleared any conflicting routes
        // by this point (called from onClick and from this function below), so
        // the currently-selected channel for each route is guaranteed to be clean.
        {
            const int egShape = static_cast<int> (apvts.getRawParameterValue ("delayEgShape")->load());

            if (egShape > 0)
            {
                const int targetGlobalIdx = findGlobalParamByName (
                    egShape == 1 ? "Amp: Volume" : "Track Level");

                std::array<bool, 17> blocked {};

                for (int r = 0; r < maxRoutes; ++r)
                {
                    const auto rs   = juce::String (r);
                    const int lfoCh = (int) apvts.getRawParameterValue ("route"    + rs + "_channel")->load();
                    const int lfoP  = (int) apvts.getRawParameterValue ("route"    + rs + "_param"  )->load();
                    if (lfoCh > 0 && lfoP == targetGlobalIdx)
                        blocked[lfoCh] = true;
                }

                for (int r = 0; r < maxRoutes; ++r)
                {
                    const auto rs  = juce::String (r);
                    const int egCh   = (int) apvts.getRawParameterValue ("egRoute" + rs + "_channel")->load();
                    if (egCh <= 0) continue;
                    const int egDest = (int) apvts.getRawParameterValue ("egRoute" + rs + "_dest"   )->load();
                    if (mapEgChoiceToGlobal (egDest) == targetGlobalIdx)
                        blocked[egCh] = true;
                }

                for (int r = 0; r < maxRoutes; ++r)
                    for (int ch = 1; ch <= 16; ++ch)
                        delayRouteChannelBox[r].setItemEnabled (ch + 1, !blocked[ch]);
            }
            else
            {
                for (int r = 0; r < maxRoutes; ++r)
                    for (int ch = 1; ch <= 16; ++ch)
                        delayRouteChannelBox[r].setItemEnabled (ch + 1, true);
            }

            // Also enforce on every timer tick so routes set before egShape was
            // activated are cleaned up even without a button click.
            enforceDelayRouteConflicts();
        }

        // EG shaping buttons: gated by delay enabled AND EG enabled.
        // The APVTS "egEnabled" param is shared across modules so we can read it here.
        const bool egEnabled = apvts.getRawParameterValue ("egEnabled")->load() > 0.5f;
        const bool egShapeEditable = enabled && egEnabled;

        if (egVolumeBtn)   egVolumeBtn->setEnabled (egShapeEditable);
        if (egTrackLvlBtn) egTrackLvlBtn->setEnabled (egShapeEditable);
        egVolumeBtnLabel.setEnabled (egShapeEditable);
        egTrackLvlBtnLabel.setEnabled (egShapeEditable);

        // Sync radio-pair toggle states from APVTS (handles automation / preset recall).
        // "delayEgShape": 0=Off, 1=EG Volume, 2=EG Track Level.
        // egPerNoteBtn is handled automatically by its ButtonAttachment.
        const int egShape = static_cast<int> (apvts.getRawParameterValue ("delayEgShape")->load());
        if (egVolumeBtn)   egVolumeBtn->setToggleState   (egShape == 1, juce::dontSendNotification);
        if (egTrackLvlBtn) egTrackLvlBtn->setToggleState (egShape == 2, juce::dontSendNotification);

        // "Per Note EG" is only meaningful when a destination is selected.
        // Hide it entirely when delayEgShape == 0 so the option isn't
        // confusingly available while no EG shaping destination is active.
        // When hidden, the processor also skips the per-note EG path (see changes doc).
        const bool perNoteVisible = egShapeEditable && (egShape > 0);
        if (egPerNoteBtn)
        {
            egPerNoteBtn->setVisible (perNoteVisible);
            egPerNoteBtn->setEnabled (perNoteVisible);
        }
        egPerNoteBtnLabel.setVisible (perNoteVisible);
        egPerNoteBtnLabel.setEnabled (perNoteVisible);

        // Visual alpha fade.
        const float a      = enabled ? 1.0f : 0.45f;
        const float aRate  = rateEditable ? 1.0f : 0.40f;  // extra dim when overridden by sync

        noteSourceDelayChannelBox.setAlpha (a);
        delaySyncBox.setAlpha (a);
        delaySyncLabel.setAlpha (enabled ? 1.0f : 0.60f);
        delayRateSlider.setAlpha (aRate);
        delayRateLabel.setAlpha (aRate);
        feedbackSlider.setAlpha (a);

        for (int r = 0; r < maxRoutes; ++r)
        {
            delayRouteChannelBox[r].setAlpha (a);
            delayRouteLabel[r].setAlpha (enabled ? 1.0f : 0.60f);
            delayRouteTransposeSlider[r].setAlpha (a);
        }

        // EG shaping buttons alpha (egPerNoteBtn uses visibility instead of alpha)
        const float aEgShape = egShapeEditable ? 1.0f : 0.45f;
        if (egVolumeBtn)   egVolumeBtn->setAlpha (aEgShape);
        if (egTrackLvlBtn) egTrackLvlBtn->setAlpha (aEgShape);
        egVolumeBtnLabel.setAlpha (aEgShape);
        egTrackLvlBtnLabel.setAlpha (aEgShape);

        // Step sequencer: gated by master delay enabled flag.
        const bool ternary    = apvts.getRawParameterValue ("delaySeqTernary")->load() > 0.5f;
        const int  activeSteps = ternary ? 6 : maxSeqSteps;

        // Sync radio pair toggle states from APVTS (handles automation / preset recall).
        if (seqBinaryBtn)
        {
            seqBinaryBtn ->setToggleState (!ternary, juce::dontSendNotification);
            seqBinaryBtn ->setEnabled (enabled);
            seqBinaryBtn ->setAlpha (a);
        }
        if (seqTernaryBtn)
        {
            seqTernaryBtn->setToggleState (ternary,  juce::dontSendNotification);
            seqTernaryBtn->setEnabled (enabled);
            seqTernaryBtn->setAlpha (a);
        }
        seqBinaryLabel.setEnabled (enabled);
        seqTernaryLabel.setEnabled (enabled);
        seqBinaryLabel.setAlpha (a);
        seqTernaryLabel.setAlpha (a);

        for (int s = 0; s < maxSeqSteps; ++s)
        {
            const bool stepVisible = (s < activeSteps);
            seqStepBtn[s]->setVisible (stepVisible);
            if (stepVisible)
            {
                seqStepBtn[s]->setEnabled (enabled);
                seqStepBtn[s]->setAlpha (a);
            }
        }

        // Trigger a layout refresh when ternary mode changes (step buttons show/hide).
        if (ternary != lastSeqTernary)
        {
            lastSeqTernary = ternary;
            resized();
        }
    }

    // ── Slider setup helpers ──────────────────────────────────────────────────

    void setupDelayRateSlider()
    {
        addAndMakeVisible (delayRateSlider);
        addAndMakeVisible (delayRateLabel);
        delayRateLabel.setText ("Delay Rate", juce::dontSendNotification);
        delayRateLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        delayRateSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        delayRateSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        delayRateSlider.setLookAndFeel (&lookGreen);
        delayRateSlider.setNumDecimalPlacesToDisplay (0);

        // Range must match the APVTS parameter definition in PluginProcessor.h.
        delayRateSlider.setNormalisableRange (
            juce::NormalisableRange<double> (50.0, 2000.0, 1.0, 0.5));

        delayRateSlider.textFromValueFunction = [] (double v) -> juce::String
        {
            if (v < 1000.0)
                return juce::String (static_cast<int> (v)) + " ms";
            return juce::String (v / 1000.0, 2) + " s";
        };

        delayRateSlider.updateText();
    }

    void setupFeedbackSlider()
    {
        addAndMakeVisible (feedbackSlider);
        addAndMakeVisible (feedbackLabel);
        feedbackLabel.setText ("Feedback", juce::dontSendNotification);
        feedbackLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);

        feedbackSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        feedbackSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        feedbackSlider.setLookAndFeel (&lookOrange);
        feedbackSlider.setNumDecimalPlacesToDisplay (0);

        // Range must match the APVTS parameter definition in PluginProcessor.h.
        feedbackSlider.setRange (0.0, 0.95, 0.01);

        feedbackSlider.textFromValueFunction = [] (double v) -> juce::String
        {
            return juce::String (static_cast<int> (std::round (v * 100.0))) + " %";
        };

        feedbackSlider.updateText();
    }

    // Shared setup for all three per-route transpose sliders.
    // Called after the APVTS attachment is created so the initial value is set.
    void setupTransposeSlider (juce::Slider& s)
    {
        addAndMakeVisible (s);

        s.setSliderStyle (juce::Slider::LinearHorizontal);
        // TextBox is narrow — just enough for "-24 st"
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        s.setLookAndFeel (&lookPurple);

        // Range mirrors the APVTS AudioParameterInt (-24 .. +24, step 1).
        s.setRange (-24.0, 24.0, 1.0);

        s.textFromValueFunction = [] (double v) -> juce::String
        {
            const int st = static_cast<int> (v);
            if (st == 0)  return "0 st";
            if (st  > 0)  return "+" + juce::String (st) + " st";
            return               juce::String (st) + " st";
        };

        s.updateText();
    }

    // Write the delayEgShape choice index to APVTS.
    // 0 = Off, 1 = EG Volume ("Amp: Volume"), 2 = EG Track Level ("Track Level").
    // Note: "Per Note EG" behavior is governed by the separate "delayEgPerNote"
    // AudioParameterBool and is independent of this choice.
    void setDelayEgShapeParam (int choiceIndex)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (
                apvts.getParameter ("delayEgShape")))
        {
            p->beginChangeGesture();
            *p = choiceIndex;
            p->endChangeGesture();
        }
    }

    void setSeqTernaryParam (bool ternary)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterBool*> (
                apvts.getParameter ("delaySeqTernary")))
        {
            p->beginChangeGesture();
            *p = ternary;
            p->endChangeGesture();
        }
    }

    // ── Cross-module conflict enforcement ─────────────────────────────────────
    //
    // Called from onClick (immediate) and from updateDelayUiEnabledState (20 Hz).
    // When egShape > 0, any delay route whose channel is already owned by an LFO
    // or EG route for the same target parameter is forced to Disabled.
    // This is the only place that writes to delayRoute{r}_channel.
    void enforceDelayRouteConflicts()
    {
        const int egShape = static_cast<int> (
            apvts.getRawParameterValue ("delayEgShape")->load());

        if (egShape == 0)
            return; // nothing to enforce when shaping is off

        const int targetGlobalIdx = findGlobalParamByName (
            egShape == 1 ? "Amp: Volume" : "Track Level");

        // Build the blocked channel set (same logic as the greying block).
        std::array<bool, 17> blocked {};

        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs   = juce::String (r);
            const int lfoCh = (int) apvts.getRawParameterValue ("route"    + rs + "_channel")->load();
            const int lfoP  = (int) apvts.getRawParameterValue ("route"    + rs + "_param"  )->load();
            if (lfoCh > 0 && lfoP == targetGlobalIdx)
                blocked[lfoCh] = true;
        }

        for (int r = 0; r < maxRoutes; ++r)
        {
            const auto rs  = juce::String (r);
            const int egCh   = (int) apvts.getRawParameterValue ("egRoute" + rs + "_channel")->load();
            if (egCh <= 0) continue;
            const int egDest = (int) apvts.getRawParameterValue ("egRoute" + rs + "_dest"   )->load();
            if (mapEgChoiceToGlobal (egDest) == targetGlobalIdx)
                blocked[egCh] = true;
        }

        // For each delay route, if its current channel is blocked, write Disabled (0).
        for (int r = 0; r < maxRoutes; ++r)
        {
            const int currentCh = (int) apvts.getRawParameterValue (
                "delayRoute" + juce::String (r) + "_channel")->load();

            if (currentCh > 0 && blocked[currentCh])
            {
                if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (
                        apvts.getParameter ("delayRoute" + juce::String (r) + "_channel")))
                {
                    p->beginChangeGesture();
                    *p = 0; // 0 = Disabled in the choice list
                    p->endChangeGesture();
                }
            }
        }
    }

    // ── Cross-module conflict helpers ─────────────────────────────────────────
    //
    // Both are static so they can be called without a component instance and
    // reused in LFO / EG editors (same SyntaktParameterTable.h is included there).

    // Find global syntaktParameters[] index by exact name. Returns -1 if not found.
    static int findGlobalParamByName (const char* name) noexcept
    {
        for (int i = 0; i < juce::numElementsInArray (syntaktParameters); ++i)
            if (juce::String (syntaktParameters[i].name) == name)
                return i;
        return -1;
    }

    // Map an EG destination choice index (0-based position in the egDestination=true
    // filtered list) to the global syntaktParameters[] index.
    // Mirrors EnvelopeEditorComponent::mapEgChoiceToGlobalParamIndex().
    static int mapEgChoiceToGlobal (int egChoice) noexcept
    {
        int k = 0;
        for (int i = 0; i < juce::numElementsInArray (syntaktParameters); ++i)
        {
            if (!syntaktParameters[i].egDestination) continue;
            if (k == egChoice) return i;
            ++k;
        }
        return -1;
    }

    // ─────────────────────────────────────────────────────────────────────────
    APVTS& apvts;

    // Group frame
    juce::GroupComponent delayGroup;

    // Enable toggle
    std::unique_ptr<LedToggleButton> delayEnable;
    std::unique_ptr<ButtonAttachment> delayEnableAttach;
    juce::Label delayEnableLabel;

    // Note source channel
    juce::Label    noteSourceDelayChannelLabel;
    juce::ComboBox noteSourceDelayChannelBox;
    std::unique_ptr<ChoiceAttachment> noteSourceDelayChannelAttach;

    // Sync division combobox (Free / 1/1 … 1/16 dotted)
    juce::Label    delaySyncLabel;
    juce::ComboBox delaySyncBox;
    std::unique_ptr<ChoiceAttachment> delaySyncAttach;

    // Sliders
    juce::Slider delayRateSlider, feedbackSlider;
    juce::Label  delayRateLabel,  feedbackLabel;
    std::unique_ptr<SliderAttachment> delayRateAttach, feedbackAttach;

    // Output routes
    std::array<juce::Label,    maxRoutes> delayRouteLabel;
    std::array<juce::ComboBox, maxRoutes> delayRouteChannelBox;
    std::array<std::unique_ptr<ChoiceAttachment>, maxRoutes> delayRouteChannelAttach;

    // Per-route transpose sliders (-24 .. +24 semitones)
    std::array<juce::Slider,   maxRoutes> delayRouteTransposeSlider;
    std::array<std::unique_ptr<SliderAttachment>, maxRoutes> delayRouteTransposeAttach;

    // EG shaping controls
    // "delayEgShape" (0=Off / 1=EG Volume / 2=EG Track Level): mutually exclusive radio pair.
    // "delayEgPerNote": independent bool toggle — can be ON simultaneously with either shape.
    std::unique_ptr<LedToggleButton> egVolumeBtn, egTrackLvlBtn, egPerNoteBtn;
    juce::Label egVolumeBtnLabel, egTrackLvlBtnLabel, egPerNoteBtnLabel;
    std::unique_ptr<ButtonAttachment> egPerNoteAttach; // only egPerNoteBtn uses an attachment

    // Step sequencer sub-frame + controls
    // maxSeqSteps must match Params::maxSteps in DelayEngine.h (both = 8).
    static constexpr int maxSeqSteps = 8;

    juce::GroupComponent              seqGroup;   // "Echo Seq" sub-frame

    // Binary / Ternary radio pair (no APVTS attachment — written manually via setSeqTernaryParam)
    std::unique_ptr<LedToggleButton>  seqBinaryBtn,  seqTernaryBtn;
    juce::Label                       seqBinaryLabel, seqTernaryLabel;

    std::array<std::unique_ptr<LedToggleButton>,  maxSeqSteps> seqStepBtn;
    std::array<std::unique_ptr<ButtonAttachment>, maxSeqSteps> seqStepAttach;

    bool lastSeqTernary = false; // tracks ternary state to trigger resized()

    // Look & Feel instances (one per slider colour)
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookOrange { SetupUI::sliderTrackOrange };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    ModzTaktLookAndFeel lookBlue   { SetupUI::sliderTrackBlue };
};