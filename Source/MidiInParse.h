#pragma once

#include <JuceHeader.h>
#include <atomic>

struct PendingMidiFlags
{
    std::atomic<int>   pendingNoteChannel  { 1 };
    std::atomic<int>   pendingNoteNumber   { 60 };
    std::atomic<float> pendingNoteVelocity { 0.0f };

    std::atomic<bool> pendingNoteOn  { false };
    std::atomic<bool> pendingNoteOff { false };

    std::atomic<bool> requestLfoStop { false };
};

template <typename ClockHandlerFn>
inline void parseIncomingMidiBuffer (const juce::MidiBuffer& midiIn,
                                     PendingMidiFlags& pending,
                                     bool syncEnabled,
                                     ClockHandlerFn&& handleIncomingClockMsg,
                                     bool noteRestartEnabled,
                                     bool noteOffStopEnabled)
{
    for (const auto meta : midiIn)
    {
        const auto msg = meta.getMessage();

        // Only feed realtime transport/clock into the clock handler
        if (syncEnabled)
        {
            if (msg.isMidiClock() || msg.isMidiStart() || msg.isMidiStop() || msg.isMidiContinue())
                handleIncomingClockMsg (msg);
        }

        // Note On
        if (msg.isNoteOn())
        {
            pending.pendingNoteChannel.store (msg.getChannel(), std::memory_order_relaxed);
            pending.pendingNoteNumber.store  (msg.getNoteNumber(), std::memory_order_relaxed);
            pending.pendingNoteVelocity.store(msg.getFloatVelocity(), std::memory_order_relaxed);
            pending.pendingNoteOn.store (true, std::memory_order_release);
        }
        // Note Off
        else if (msg.isNoteOff())
        {
            pending.pendingNoteChannel.store (msg.getChannel(), std::memory_order_relaxed);
            pending.pendingNoteNumber.store  (msg.getNoteNumber(), std::memory_order_relaxed);
            pending.pendingNoteOff.store (true, std::memory_order_release);

            if (noteRestartEnabled && noteOffStopEnabled)
                pending.requestLfoStop.store (true, std::memory_order_release);
        }
    }
}