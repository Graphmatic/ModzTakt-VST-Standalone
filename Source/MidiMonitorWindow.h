#pragma once

#include "MidiMonitorContent.h"

// MidiMonitorWindow.h
class MidiMonitorWindow : public juce::DialogWindow,
                          private juce::Timer
{
public:
    MidiMonitorWindow()
        : DialogWindow("MIDI Monitor",
                       juce::Colours::darkgrey,
                       true),
          fifo(256)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);

        content = std::make_unique<MidiMonitorContent>();
        setContentOwned(content.get(), false);

        centreWithSize(500, 300);

        startTimerHz(2); // UI refresh rate (low priority)
    }

    // ============================================================
    // REALTIME-SAFE ENTRY POINT (called from MIDI / LFO code)
    // ============================================================
    void pushEvent(const juce::MidiMessage& msg, bool isIncoming)
    {
        // Optional decimation to reduce load further
        // if (++decimationCounter % monitorDecimation != 0)
        //     return;

        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0)
        {
            eventBuffer[(size_t)start1] = { msg, isIncoming };
            fifo.finishedWrite(1);
        }
    }

    // ============================================================
    // INTERNAL EVENT STRUCT
    // ============================================================
    struct MidiLogEvent
    {
        juce::MidiMessage msg;
        bool incoming = false;
    };

private:


    // ============================================================
    // TIMER (UI THREAD)
    // ============================================================
    void timerCallback() override
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead(16, start1, size1, start2, size2);

        if (size1 == 0)
            return;

        auto& editor = content->logEditor;

        for (int i = 0; i < size1; ++i)
            appendToEditor(eventBuffer[(size_t)(start1 + i)], editor);

        fifo.finishedRead(size1);

        trimHistory(editor);
        editor.moveCaretToEnd();
    }

    // ============================================================
    // UI HELPERS (UI THREAD ONLY)
    // ============================================================
    void appendToEditor(const MidiLogEvent& e, juce::TextEditor& editor)
    {
        // const auto colour = e.incoming
        //     ? juce::Colours::lightgrey
        //     : getChannelColour(e.msg.getChannel());

        // editor.setColour(juce::TextEditor::textColourId, colour);
        editor.insertTextAtCaret(e.msg.getDescription() + "\n");
    }

    void trimHistory(juce::TextEditor& editor)
    {
        auto text = editor.getText();
        auto lines = juce::StringArray::fromLines(text);

        if (lines.size() <= maxHistory)
            return;

        lines.removeRange(0, lines.size() - maxHistory);

        editor.clear();
        for (auto& l : lines)
            editor.insertTextAtCaret(l + "\n");
    }

    // ============================================================
    // COLOR PER MIDI CHANNEL
    // ============================================================
    // juce::Colour getChannelColour(int channel) const
    // {
    //     float hue = (channel - 1) / 16.0f;
    //     return juce::Colour::fromHSV(hue, 0.8f, 0.9f, 1.0f);
    // }

private:
    // ============================================================
    // DATA
    // ============================================================
    std::unique_ptr<MidiMonitorContent> content;

    juce::AbstractFifo fifo;
    std::vector<MidiLogEvent> eventBuffer { 256 };

    static constexpr int maxHistory = 24;

    // Decimation (log only 1 out of N events)


};