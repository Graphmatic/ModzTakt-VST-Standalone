#pragma once

// ============================================================
//  AudioRecorderComponent.h
//  AVAILABILITY: Linux Standalone build ONLY.
// ============================================================
#if JUCE_LINUX \
    && defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone \
    && defined(MODZTAKT_OVERBRIDGE) && MODZTAKT_OVERBRIDGE

#include <JuceHeader.h>
#include "Cosmetic.h"
#include "OverbridgeEngine.h"

#include <atomic>
#include <vector>
#include <memory>

// ============================================================
//  SyntaktAudioTrack
// ============================================================
struct SyntaktAudioTrack
{
    int          deviceChannelIndex { -1 };
    juce::String driverName;
    juce::String displayName;
    bool         isMainMix { false };
    bool         isArmed   { false };
};


// ============================================================
//  WriterThread
//
//  Reads float samples from a lock-free ring buffer (one per
//  channel) and writes them to open AudioFormatWriter instances.
//  Lives entirely off the USB callback thread so disk I/O never
//  blocks real-time capture.
// ============================================================
class WriterThread : public juce::Thread
{
public:
    // Ring buffer capacity: 10 seconds at 48 kHz — large enough that even
    // if the writer thread sleeps briefly, it never overflows.
    static constexpr int kRingCapacity = 48000 * 10;

    explicit WriterThread (int numChannels)
        : juce::Thread ("OB-Writer"),
          numCh (numChannels),
          fifo (kRingCapacity)
    {
        ringBuffer.setSize (numChannels, kRingCapacity);
        ringBuffer.clear();
    }

    // ── Called from the USB callback thread ──────────────────

    /// Push a block of non-interleaved float samples into the ring.
    /// @param channelData  Array of numChannels pointers, each with numFrames samples.
    /// @param numChannels  Must match the value passed to the constructor.
    /// @param numFrames    Number of frames in this block.
    void pushSamples (const float* const* channelData,
                      int                 numChannels,
                      int                 numFrames) noexcept
    {
        juce::ignoreUnused (numChannels);

        int start1, size1, start2, size2;
        fifo.prepareToWrite (numFrames, start1, size1, start2, size2);

        // If size1+size2 < numFrames the FIFO is full — count dropped frames.
        const int dropped = numFrames - (size1 + size2);
        if (dropped > 0)
            framesDropped.fetch_add (dropped, std::memory_order_relaxed);

        if (size1 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                ringBuffer.copyFrom (ch, start1, channelData[ch], size1);

        if (size2 > 0)
            for (int ch = 0; ch < numCh; ++ch)
                ringBuffer.copyFrom (ch, start2, channelData[ch] + size1, size2);

        fifo.finishedWrite (size1 + size2);
        framesReceived.fetch_add (size1 + size2, std::memory_order_relaxed);

        // Wake the writer thread immediately instead of waiting for its 2ms poll.
        notify();
    }

    // ── Called from the JUCE message thread ──────────────────

    /// Open one writer per armed channel index.
    /// @param outputDir      Destination folder.
    /// @param armedChannels  Indices into the 14-channel layout.
    /// @param timestamp      Timestamp string used in filenames.
    bool openWriters (const juce::File&        outputDir,
                      const std::vector<int>&  armedChannels,
                      const juce::String&      timestamp)
    {
        closeWriters();

        juce::WavAudioFormat wav;

        for (int chIdx : armedChannels)
        {
            const auto& layout = OverbridgeEngine::syntaktChannels();
            const juce::String name =
                chIdx < (int) layout.size()
                ? layout[(size_t) chIdx].name.replace (" ", "_").replace ("/", "-")
                : ("ch" + juce::String (chIdx));

            const juce::File file = outputDir.getChildFile (
                "SYNTAKT-" + timestamp + "-" + name + ".wav");

            auto* os = file.createOutputStream().release();
            if (! os)
            {
               #if JUCE_DEBUG
                juce::Logger::writeToLog ("WriterThread: could not create " + file.getFullPathName());
               #endif
                closeWriters();
                return false;
            }

            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (os,
                                     OverbridgeEngine::kSampleRate,
                                     1,       // mono per file
                                     32,      // 32-bit — matches Syntakt Overbridge native depth
                                     {},
                                     0));
            if (! writer)
            {
               #if JUCE_DEBUG
                juce::Logger::writeToLog ("WriterThread: WAV writer creation failed for " + name);
               #endif
                delete os;
                closeWriters();
                return false;
            }

            writers.push_back (std::move (writer));
            writerChannelIndex.push_back (chIdx);
        }
        return ! writers.empty();
    }

    void closeWriters()
    {
        writers.clear();
        writerChannelIndex.clear();
    }

    int numArmedWriters() const noexcept { return (int) writers.size(); }

    // ── juce::Thread ─────────────────────────────────────────
    void run() override
    {
        constexpr int kChunkSize = 512;
        juce::AudioBuffer<float> mono (1, kChunkSize);

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("WriterThread: started, writers=" + juce::String ((int) writers.size()));
       #endif

        while (! threadShouldExit())
        {
            int start1, size1, start2, size2;
            fifo.prepareToRead (kChunkSize, start1, size1, start2, size2);

            const int total = size1 + size2;
            if (total == 0)
            {
                wait (1);   // woken by notify() from pushSamples; 1ms fallback
                continue;
            }

            for (int w = 0; w < (int) writers.size(); ++w)
            {
                const int ch = writerChannelIndex[(size_t) w];
                if (ch < 0 || ch >= numCh) continue;

                if (size1 > 0)
                    mono.copyFrom (0, 0, ringBuffer, ch, start1, size1);
                if (size2 > 0)
                    mono.copyFrom (0, size1, ringBuffer, ch, start2, size2);

                writers[(size_t) w]->writeFromAudioSampleBuffer (mono, 0, total);
            }

            fifo.finishedRead (total);
            framesWritten.fetch_add (total, std::memory_order_relaxed);
        }

        // Drain any remaining samples before exiting.
        flush();

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("WriterThread: finished."
            "  received=" + juce::String (framesReceived.load()) +
            "  written="  + juce::String (framesWritten.load()) +
            "  dropped="  + juce::String (framesDropped.load()));
       #endif
    }

        // Diagnostic counters — readable from the message thread
        std::atomic<int> framesReceived { 0 };
        std::atomic<int> framesWritten  { 0 };
        std::atomic<int> framesDropped  { 0 };

private:
    void flush()
    {
        // Drain in chunks to keep memory usage bounded even with a large ring buffer.
        constexpr int kFlushChunk = 4096;
        juce::AudioBuffer<float> mono (1, kFlushChunk);

        int remaining = fifo.getNumReady();
        int totalFlushed = 0;

        while (remaining > 0)
        {
            const int toRead = juce::jmin (remaining, kFlushChunk);
            int start1, size1, start2, size2;
            fifo.prepareToRead (toRead, start1, size1, start2, size2);
            const int got = size1 + size2;
            if (got == 0) break;

            for (int w = 0; w < (int) writers.size(); ++w)
            {
                const int ch = writerChannelIndex[(size_t) w];
                if (ch < 0 || ch >= numCh) continue;
                if (size1 > 0) mono.copyFrom (0, 0,     ringBuffer, ch, start1, size1);
                if (size2 > 0) mono.copyFrom (0, size1, ringBuffer, ch, start2, size2);
                writers[(size_t) w]->writeFromAudioSampleBuffer (mono, 0, got);
            }
            fifo.finishedRead (got);
            framesWritten.fetch_add (got, std::memory_order_relaxed);
            totalFlushed += got;
            remaining    -= got;
        }

        if (totalFlushed > 0)
        {
           #if JUCE_DEBUG
            juce::Logger::writeToLog ("WriterThread: flush() wrote " + juce::String (totalFlushed) + " frames");
           #endif
        }
    }

    const int               numCh;
    juce::AbstractFifo      fifo;
    juce::AudioBuffer<float> ringBuffer;

    std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers;
    std::vector<int>                                       writerChannelIndex;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WriterThread)
};


// ============================================================
//  AudioRecorderComponent
// ============================================================
class AudioRecorderComponent : public juce::Component,
                                private juce::Timer
{
public:
    explicit AudioRecorderComponent (OverbridgeEngine& engine)
        : engine (engine),
          writerThread (OverbridgeEngine::kNumChannels)
    {
        setName ("AudioRecorder");

        // Persistent settings
        juce::PropertiesFile::Options opts;
        opts.applicationName = "ModzTakt";
        opts.filenameSuffix  = "xml";
        opts.folderName      = "ModzTakt";
        opts.storageFormat   = juce::PropertiesFile::storeAsXML;
        propertiesFile = std::make_unique<juce::PropertiesFile> (opts);
        outputDirectory = loadSavedDirectory();

        // ── Title ─────────────────────────────────────────────
        titleLabel.setText ("Syntakt Track Recorder", juce::dontSendNotification);
        titleLabel.setFont (juce::Font (14.0f, juce::Font::bold));
        titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        titleLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (titleLabel);

        // ── Status ────────────────────────────────────────────
        statusLabel.setFont (juce::Font (11.0f));
        statusLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        statusLabel.setJustificationType (juce::Justification::centred);
        statusLabel.setText ("Scanning…", juce::dontSendNotification);
        addAndMakeVisible (statusLabel);

        // ── Output directory ──────────────────────────────────
        outputDirSectionLabel.setText ("Output Directory", juce::dontSendNotification);
        outputDirSectionLabel.setFont (juce::Font (11.0f, juce::Font::bold));
        outputDirSectionLabel.setColour (juce::Label::textColourId,
                                         juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (outputDirSectionLabel);

        outputDirPathLabel.setFont (juce::Font (11.0f));
        outputDirPathLabel.setColour (juce::Label::backgroundColourId,
                                      juce::Colour (0xff141618));
        outputDirPathLabel.setColour (juce::Label::textColourId,
                                      juce::Colour (0xff88c8ff));
        outputDirPathLabel.setMinimumHorizontalScale (1.0f);
        outputDirPathLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (outputDirPathLabel);
        updateDirPathLabel();

        browseButton.setButtonText ("Browse…");
        browseButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2d30));
        browseButton.setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        browseButton.onClick = [this] { browseForOutputDirectory(); };
        addAndMakeVisible (browseButton);

        // ── Refresh ───────────────────────────────────────────
        refreshButton.setButtonText ("Refresh");
        refreshButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2d30));
        refreshButton.setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        refreshButton.onClick = [this] { refreshTracks(); };
        addAndMakeVisible (refreshButton);

        // ── Record ────────────────────────────────────────────
        recordButton.setButtonText (kIdleText);
        recordButton.setClickingTogglesState (true);
        recordButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff2a2d30));
        recordButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffcc0000));
        recordButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        recordButton.setEnabled (false);
        recordButton.onClick = [this] { onRecordToggled(); };
        addAndMakeVisible (recordButton);

        // ── Track list ────────────────────────────────────────
        trackListContent = std::make_unique<TrackListContent> (
            tracks, [this](int i){ onTrackArmToggled (i); });

        trackViewport.setViewedComponent (trackListContent.get(), false);
        trackViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (trackViewport);

        refreshTracks();
        startTimerHz (1);
    }

    ~AudioRecorderComponent() override
    {
        stopTimer();
        if (writerThread.isThreadRunning())
        {
            engine.stop();
            writerThread.stopThread (5000);
            writerThread.closeWriters();
        }
    }

    // ── Layout ────────────────────────────────────────────────
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xee191b1e));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);

        titleLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (4);
        statusLabel.setBounds (area.removeFromTop (30));
        area.removeFromTop (6);

        outputDirSectionLabel.setBounds (area.removeFromTop (14));
        area.removeFromTop (3);
        {
            auto row = area.removeFromTop (22);
            browseButton.setBounds (row.removeFromRight (64));
            row.removeFromRight (4);
            outputDirPathLabel.setBounds (row);
        }
        area.removeFromTop (6);

        {
            auto row = area.removeFromTop (24);
            recordButton.setBounds (row.removeFromRight (120));
            row.removeFromRight (4);
            refreshButton.setBounds (row.removeFromRight (70));
        }
        area.removeFromTop (6);

        trackViewport.setBounds (area);
        if (trackListContent)
            trackListContent->setSize (
                area.getWidth() - trackViewport.getScrollBarThickness(),
                trackListContent->getPreferredHeight());
    }

    // ── Accessors ─────────────────────────────────────────────
    bool isRecording() const noexcept { return recordButton.getToggleState(); }

    const std::vector<SyntaktAudioTrack>& getTracks() const noexcept { return tracks; }

    std::vector<const SyntaktAudioTrack*> getArmedTracks() const
    {
        std::vector<const SyntaktAudioTrack*> out;
        for (const auto& t : tracks) if (t.isArmed) out.push_back (&t);
        return out;
    }

    const juce::File& getOutputDirectory() const noexcept { return outputDirectory; }

private:
    static constexpr const char* kIdleText   = "● Record";
    static constexpr const char* kActiveText = "■ Stop";

    // ── Timer: re-probe once per second when idle ─────────────
    void timerCallback() override
    {
        if (! isRecording())
            refreshTracks();
    }

    // ── Device scan ───────────────────────────────────────────
    void refreshTracks()
    {
        const auto ds = engine.probe();
        const bool ready = (ds == OverbridgeEngine::DeviceState::Ready
                         || ds == OverbridgeEngine::DeviceState::Running);

        // Preserve arm states across re-probes
        std::vector<bool> prevArm (OverbridgeEngine::kNumChannels, false);
        for (const auto& t : tracks)
            if (t.deviceChannelIndex >= 0 && t.deviceChannelIndex < OverbridgeEngine::kNumChannels)
                prevArm[(size_t) t.deviceChannelIndex] = t.isArmed;

        tracks.clear();

        if (ready)
        {
            for (const auto& ch : OverbridgeEngine::syntaktChannels())
            {
                SyntaktAudioTrack t;
                t.deviceChannelIndex = ch.index;
                t.driverName         = ch.name;
                t.displayName        = ch.name;
                t.isMainMix          = ch.isMainMix;
                t.isArmed            = prevArm[(size_t) ch.index];
                tracks.push_back (t);
            }
        }

        setStatus (engine.getStatusText());
        rebuildTrackListUI();
        updateRecordButton();
    }

    // ── Arm toggle ────────────────────────────────────────────
    void onTrackArmToggled (int idx)
    {
        if (idx < 0 || idx >= (int) tracks.size()) return;
        tracks[(size_t) idx].isArmed = ! tracks[(size_t) idx].isArmed;
        if (trackListContent) trackListContent->repaint();
        updateRecordButton();
    }

    void updateRecordButton()
    {
        const auto ds = engine.getState();
        const bool ready = (ds == OverbridgeEngine::DeviceState::Ready
                         || ds == OverbridgeEngine::DeviceState::Running);
        recordButton.setEnabled (ready && ! getArmedTracks().empty());
        if (! isRecording())
            recordButton.setButtonText (kIdleText);
    }

    // ── Record start / stop ───────────────────────────────────
    void onRecordToggled()
    {
        if (recordButton.getToggleState())
            startRecording();
        else
            stopRecording();
    }

    void startRecording()
    {
        // 1. Collect armed channel indices
        std::vector<int> armedIndices;
        for (const auto& t : tracks)
            if (t.isArmed)
                armedIndices.push_back (t.deviceChannelIndex);

        if (armedIndices.empty())
        {
            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        // 2. Build timestamp string: YYYY_MM-DD_HH_MM_SS
        const juce::Time now = juce::Time::getCurrentTime();
        const juce::String timestamp =
            juce::String::formatted ("%04d_%02d_%02d-%02d_%02d_%02d",
                now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                now.getHours(), now.getMinutes(), now.getSeconds());

        // 3. Open WAV writers (one file per armed channel)
        if (! writerThread.openWriters (outputDirectory, armedIndices, timestamp))
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon,
                "Recording error",
                "Could not create output files in:\n" + outputDirectory.getFullPathName()
                + "\n\nCheck that the folder exists and is writable.");
            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        // 4. Reset diagnostic counters for this session
        writerThread.framesReceived.store (0, std::memory_order_relaxed);
        writerThread.framesWritten.store  (0, std::memory_order_relaxed);
        writerThread.framesDropped.store  (0, std::memory_order_relaxed);

        // 5. Wire the engine's audio callback → ring buffer
        engine.setAudioCallback (
            [this] (const float* const* samples, int numCh, int numFrames)
            {
                // Called on the USB thread — push only, no locks
                writerThread.pushSamples (samples, numCh, numFrames);
            });

        // 6. Start the writer thread first, then the USB capture
        writerThread.startThread (juce::Thread::Priority::normal);

        if (! engine.start())
        {
            writerThread.stopThread (2000);
            writerThread.closeWriters();
            engine.setAudioCallback (nullptr);

            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon,
                "Recording error",
                "Failed to start USB capture:\n" + engine.getStatusText());

            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        recordButton.setButtonText (kActiveText);
        setStatus ("Recording — " + juce::String ((int) armedIndices.size())
                   + " channel(s)  →  " + outputDirectory.getFullPathName());

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("AudioRecorder: started recording "
            + juce::String ((int) armedIndices.size()) + " channels to "
            + outputDirectory.getFullPathName());
       #endif
    }

    void stopRecording()
    {
        // Stop USB capture first (no more audioCallback fires after this returns)
        engine.stop();
        engine.setAudioCallback (nullptr);

        // Stop writer thread (flush() drains remaining ring buffer data to disk)
        writerThread.stopThread (5000);

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("AudioRecorder: writer stats:"
            "  received=" + juce::String (writerThread.framesReceived.load()) +
            "  written="  + juce::String (writerThread.framesWritten.load()) +
            "  dropped="  + juce::String (writerThread.framesDropped.load()) +
            "  duration=" + juce::String (writerThread.framesWritten.load() / 48000.0, 2) + "s");
       #endif

        // Destroy writers — their destructors finalize the WAV RIFF headers
        writerThread.closeWriters();

        recordButton.setButtonText (kIdleText);
        setStatus ("Stopped.  Files saved to:\n" + outputDirectory.getFullPathName());
       #if JUCE_DEBUG
        juce::Logger::writeToLog ("AudioRecorder: recording stopped.");
       #endif

        refreshTracks();
    }

    // ── Directory helpers ─────────────────────────────────────
    juce::File loadSavedDirectory() const
    {
        if (propertiesFile)
        {
            const juce::String s = propertiesFile->getValue ("outputDirectory");
            if (s.isNotEmpty()) { juce::File f (s); if (f.isDirectory()) return f; }
        }
        const juce::File m = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        return m.isDirectory() ? m
             : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    }

    void saveDirectory()
    {
        if (propertiesFile)
        {
            propertiesFile->setValue ("outputDirectory", outputDirectory.getFullPathName());
            propertiesFile->saveIfNeeded();
        }
    }

    void browseForOutputDirectory()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Select output directory for recordings", outputDirectory, "", true);

        fileChooser->launchAsync (
            juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const juce::File chosen = fc.getResult();
                if (chosen == juce::File{} || ! chosen.isDirectory()) return;
                outputDirectory = chosen;
                saveDirectory();
                updateDirPathLabel();
            });
    }

    void updateDirPathLabel()
    {
        outputDirPathLabel.setText (outputDirectory.getFullPathName(),
                                    juce::dontSendNotification);
        outputDirPathLabel.setTooltip (outputDirectory.getFullPathName());
    }

    void setStatus (const juce::String& t)
    { statusLabel.setText (t, juce::dontSendNotification); }

    void rebuildTrackListUI()
    {
        if (trackListContent) trackListContent->updateTracks (tracks);
        resized(); repaint();
    }

    // ==========================================================
    //  TrackListContent — one clickable row per track
    // ==========================================================
    class TrackListContent : public juce::Component
    {
    public:
        using ArmCb = std::function<void(int)>;

        TrackListContent (std::vector<SyntaktAudioTrack>& tr, ArmCb cb)
            : tracks (tr), onArmToggled (std::move (cb))
        { setInterceptsMouseClicks (true, false); }

        void updateTracks (const std::vector<SyntaktAudioTrack>&) { repaint(); }
        int  getPreferredHeight() const noexcept
        { return ((int) tracks.size() > 0 ? (int) tracks.size() : 1) * kRowH; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int r = e.y / kRowH;
            if (r >= 0 && r < (int) tracks.size() && onArmToggled)
                onArmToggled (r);
        }

        void paint (juce::Graphics& g) override
        {
            const int n = (int) tracks.size();
            for (int i = 0; i < n; ++i)
            {
                const auto& t = tracks[(size_t) i];
                const juce::Rectangle<int> row (0, i * kRowH, getWidth(), kRowH - 1);

                g.setColour (t.isMainMix ? juce::Colour (0xff1c2a35) : juce::Colour (0xff1c1e21));
                g.fillRect (row);

                const juce::Rectangle<float> led (
                    5.0f, row.getY() + (kRowH - kLed) * 0.5f, (float) kLed, (float) kLed);

                if (t.isArmed)
                {
                    g.setColour (juce::Colour (0xffcc0000).withAlpha (0.35f));
                    g.fillEllipse (led.expanded (3.0f));
                    g.setColour (juce::Colour (0xffff2211));
                    g.fillEllipse (led);
                }
                else
                {
                    g.setColour (juce::Colour (0xff2e2e2e));
                    g.fillEllipse (led);
                }
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.drawEllipse (led.reduced (0.5f), 1.0f);

                g.setColour (juce::Colour (0xff4a4a4a));
                g.setFont (9.5f);
                g.drawText (juce::String (t.deviceChannelIndex + 1),
                            22, row.getY(), 22, kRowH, juce::Justification::centred, false);

                g.setColour (t.isArmed    ? juce::Colours::white.withAlpha (0.95f)
                             : t.isMainMix ? juce::Colour (0xff7ab8e8)
                                           : SetupUI::labelsColor);
                g.setFont (12.0f);
                g.drawText (t.displayName, 48, row.getY(), getWidth() - 54, kRowH,
                            juce::Justification::centredLeft, true);

                g.setColour (juce::Colours::black.withAlpha (0.35f));
                g.drawHorizontalLine (row.getBottom(), 0.0f, (float) getWidth());
            }

            if (tracks.empty())
            {
                g.setColour (SetupUI::labelsColor.withAlpha (0.4f));
                g.setFont (12.0f);
                g.drawText ("Device not ready — press Refresh",
                            0, 0, getWidth(), getHeight(),
                            juce::Justification::centred, true);
            }
        }

        void resized() override {}

    private:
        std::vector<SyntaktAudioTrack>& tracks;
        ArmCb onArmToggled;
        static constexpr int kRowH = 28;
        static constexpr int kLed  = 12;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackListContent)
    };

    // ── Members ───────────────────────────────────────────────
    OverbridgeEngine& engine;
    WriterThread      writerThread;

    juce::File                            outputDirectory;
    std::unique_ptr<juce::PropertiesFile> propertiesFile;
    std::unique_ptr<juce::FileChooser>    fileChooser;

    std::vector<SyntaktAudioTrack> tracks;

    juce::Label      titleLabel, statusLabel;
    juce::Label      outputDirSectionLabel, outputDirPathLabel;
    juce::TextButton browseButton, refreshButton, recordButton;

    juce::Viewport                    trackViewport;
    std::unique_ptr<TrackListContent> trackListContent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRecorderComponent)
};

#endif // JUCE_LINUX && JucePlugin_Build_Standalone