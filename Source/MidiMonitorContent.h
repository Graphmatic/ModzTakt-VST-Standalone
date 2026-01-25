class MidiMonitorContent : public juce::Component
{
public:
    MidiMonitorContent()
    {
        logEditor.setMultiLine(true);
        logEditor.setReadOnly(true);
        logEditor.setScrollbarsShown(true);
        addAndMakeVisible(logEditor);
    }

    void resized() override
    {
        logEditor.setBounds(getLocalBounds());
    }

    juce::TextEditor logEditor;
};