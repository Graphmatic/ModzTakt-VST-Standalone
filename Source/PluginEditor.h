#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// Reuse your existing UI (fastest path to “it compiles”)
#include "MainComponent.h"

class ModzTaktAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    inline explicit ModzTaktAudioProcessorEditor (ModzTaktAudioProcessor& p)
        : juce::AudioProcessorEditor (&p)
        , processor (p)
    {
        addAndMakeVisible (mainComponent);

        // Choose a sensible initial size. You can match your standalone window later.
        setSize (820, 560);
        setResizable(false, false);      // width only
        setResizeLimits(1000, 800, 1920, 800); // minW, minH, maxW, maxH
    }

    inline ~ModzTaktAudioProcessorEditor() override = default;

    inline void paint (juce::Graphics& g) override
    {
        // Keep transparent/neutral; your MainComponent paints itself.
        g.fillAll (juce::Colours::black);
    }

    inline void resized() override
    {
        mainComponent.setBounds (getLocalBounds());
    }

private:
    ModzTaktAudioProcessor& processor;

    // NOTE: MainComponent currently inherits Timer + manages MIDI devices.
    // For plugin-first, we will later refactor it into UI-only, and move
    // the engine + MIDI I/O into the processor (processBlock).
    // MainComponent mainComponent;

    // MainComponent mainComponent (processor.getAPVTS());
    MainComponent mainComponent { processor };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModzTaktAudioProcessorEditor)
};

// //==============================================================================
// // Implement createEditor() inline (needs full editor definition)
// inline juce::AudioProcessorEditor* ModzTaktAudioProcessor::createEditor()
// {
//     return new ModzTaktAudioProcessorEditor (*this);
// }
