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
                                     int syncModeSelectedId,
                                     ClockHandlerFn&& handleIncomingClockMsg,
                                     bool noteRestartEnabled,
                                     bool noteOffStopEnabled)
{
    for (const auto meta : midiIn)
    {
        const auto msg = meta.getMessage();

        // Preserve original gating
        if (syncModeSelectedId == 2)
            handleIncomingClockMsg (msg);

        if (msg.isNoteOn())
        {
            pending.pendingNoteChannel.store (msg.getChannel(), std::memory_order_relaxed);
            pending.pendingNoteNumber.store  (msg.getNoteNumber(), std::memory_order_relaxed);
            pending.pendingNoteVelocity.store(msg.getFloatVelocity(), std::memory_order_relaxed);
            pending.pendingNoteOn.store (true, std::memory_order_release);
        }
        else if (msg.isNoteOff())
        {
            pending.pendingNoteChannel.store (msg.getChannel(), std::memory_order_relaxed);
            pending.pendingNoteNumber.store  (msg.getNoteNumber(), std::memory_order_relaxed); // IMPORTANT: store note
            pending.pendingNoteOff.store (true, std::memory_order_release);

            if (noteRestartEnabled && noteOffStopEnabled)
                pending.requestLfoStop.store (true, std::memory_order_release);
        }
    }
}
