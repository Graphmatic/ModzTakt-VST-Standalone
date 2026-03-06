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
        delayGroup.setText ("Note Delay");
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

        for (int r = 0; r < maxRoutes; ++r)
        {
            delayRouteChannelAttach[r].reset();
            delayRouteTransposeAttach[r].reset();
        }
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

        content.removeFromTop (20);

        // ── Note Source channel ───────────────────────────────────────────────
        placeRow (noteSourceDelayChannelLabel, noteSourceDelayChannelBox);

        content.removeFromTop (10);

        // ── Sync division ─────────────────────────────────────────────────────
        placeRow (delaySyncLabel, delaySyncBox);

        content.removeFromTop (18);

        // ── Sliders ───────────────────────────────────────────────────────────
        placeRow (delayRateLabel, delayRateSlider);

        content.removeFromTop (18);

        placeRow (feedbackLabel,  feedbackSlider);

        content.removeFromTop (14);

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

    // Look & Feel instances (one per slider colour)
    ModzTaktLookAndFeel lookGreen  { SetupUI::sliderTrackGreen };
    ModzTaktLookAndFeel lookOrange { SetupUI::sliderTrackOrange };
    ModzTaktLookAndFeel lookPurple { SetupUI::sliderTrackPurple };
    ModzTaktLookAndFeel lookBlue   { SetupUI::sliderTrackBlue };
};