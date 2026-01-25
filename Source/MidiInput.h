#pragma once
#include <JuceHeader.h>

// Listener interface for transport events (unchanged)
class MidiClockListener
{
public:
    virtual ~MidiClockListener() = default;
    virtual void handleMidiStart() {}
    virtual void handleMidiStop() {}
    virtual void handleMidiContinue() {}
};

class MidiClockHandler : public juce::MidiInputCallback
{
public:
    MidiClockHandler() = default;
    ~MidiClockHandler() override { stop(); }

    // keep the existing listener API you already use
    void setListener(MidiClockListener* l) { listener = l; }

    // NEW (non-invasive): optional callback to forward Note-On messages
    // MainComponent can set this to receive Note-On events from the open input.
    std::function<void(const juce::MidiMessage&)> noteOnCallback;

    // Start listening to the given MIDI input device index.
    // Returns true if the device was opened.
    bool start(int deviceIndex)
    {
        stop();

        auto devices = juce::MidiInput::getAvailableDevices();
        if (deviceIndex < 0 || deviceIndex >= devices.size())
            return false;

        midiInput = juce::MidiInput::openDevice(devices[deviceIndex].identifier, this);
        if (midiInput)
        {
            midiInput->start();
            lastClockTimes.clear();
            currentBpm = 0.0;
            return true;
        }
        return false;
    }

    void stop()
    {
        if (midiInput)
        {
            midiInput->stop();
            midiInput.reset();
        }
        lastClockTimes.clear();
        currentBpm = 0.0;
    }

    // The incoming message handler: keeps your BPM computation
    void handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message) override
    {
        // High resolution timestamp in milliseconds
        const double nowMs = juce::Time::getMillisecondCounterHiRes();

        if (message.isMidiClock())
        {
            // Store up to last N clocks for averaging (default 48)
            lastClockTimes.add(nowMs);
            if (lastClockTimes.size() > 48)
                lastClockTimes.remove(0);

            const int n = lastClockTimes.size();
            if (n >= 2)
            {
                const double clockIntervals = (double)(n - 1);
                const double elapsedMs = lastClockTimes.getLast() - lastClockTimes.getFirst();
                if (elapsedMs > 0.0)
                {
                    // BPM = (60000 * clockIntervals) / (elapsedMs * 24)
                    const double computedBpm = (60000.0 * clockIntervals) / (elapsedMs * 24.0);

                    // basic sanity filter and smoothing
                    if (computedBpm > 10.0 && computedBpm < 400.0)
                    {
                        // simple smoothing to reduce jitter (keeps previous behaviour)
                        if (currentBpm <= 0.0)
                            currentBpm = computedBpm;
                        else
                            currentBpm = 0.9 * currentBpm + 0.1 * computedBpm;
                    }
                }
            }
        }
        else if (message.isMidiStart())
        {
            // reset stored clocks so BPM restarts cleanly
            lastClockTimes.clear();
            currentBpm = 0.0;
            if (listener) listener->handleMidiStart();
        }
        else if (message.isMidiStop())
        {
            if (listener) listener->handleMidiStop();
        }
        else if (message.isMidiContinue())
        {
            if (listener) listener->handleMidiContinue();
        }

        // --- NEW: forward Note-On events without opening another input ---
        if (message.isNoteOn() && noteOnCallback)
            noteOnCallback(message);

    }

    double getCurrentBPM() const noexcept { return currentBpm; }

private:
    std::unique_ptr<juce::MidiInput> midiInput;
    MidiClockListener* listener = nullptr;

    // BPM calculation state (unchanged)
    juce::Array<double> lastClockTimes; // timestamps in ms
    double currentBpm { 0.0 };
};
