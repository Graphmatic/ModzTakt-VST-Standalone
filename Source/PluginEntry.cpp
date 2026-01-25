#include "PluginEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ModzTaktAudioProcessor();
}

juce::AudioProcessorEditor* ModzTaktAudioProcessor::createEditor()
{
    return new ModzTaktAudioProcessorEditor (*this);
}
