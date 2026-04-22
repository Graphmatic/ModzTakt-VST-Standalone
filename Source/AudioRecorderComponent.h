#pragma once

// ============================================================
//  AudioRecorderComponent.h
//  AVAILABILITY: Linux Standalone + MODZTAKT_OVERBRIDGE builds ONLY.
//  Requires OverbridgeEngine.h and libusb-1.0.
//  Define MODZTAKT_OVERBRIDGE=1 in the Projucer "Linux Makefile
//  (Overbridge)" exporter to enable this feature.
// ============================================================
#if JUCE_LINUX \
    && defined (JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone \
    && defined (MODZTAKT_OVERBRIDGE) && MODZTAKT_OVERBRIDGE

#include <JuceHeader.h>
#include "Cosmetic.h"
#include "OverbridgeEngine.h"
#include "VuMeterComponent.h"

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
    static constexpr int kRingCapacity = 48000 * 10;

    explicit WriterThread (int numChannels)
        : juce::Thread ("OB-Writer"),
          numCh (numChannels),
          fifo (kRingCapacity)
    {
        ringBuffer.setSize (numChannels, kRingCapacity);
        ringBuffer.clear();
    }

    void pushSamples (const float* const* channelData,
                      int                 numChannels,
                      int                 numFrames) noexcept
    {
        juce::ignoreUnused (numChannels);

        int start1, size1, start2, size2;
        fifo.prepareToWrite (numFrames, start1, size1, start2, size2);

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
        notify();
    }

    // ── Writer management ─────────────────────────────────────
    bool openWriters (const juce::File&       outputDir,
                      const std::vector<int>& armedChannels,
                      const juce::String&     timestamp)
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
                "SYNTAKT_" + timestamp + "_" + name + ".wav");

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
                                     1,
                                     32,   // 32-bit — Syntakt Overbridge native depth
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
                wait (1);
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

        flush();

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("WriterThread: finished."
            "  received=" + juce::String (framesReceived.load()) +
            "  written="  + juce::String (framesWritten.load()) +
            "  dropped="  + juce::String (framesDropped.load()));
       #endif
    }

    // Diagnostic counters
    std::atomic<int> framesReceived { 0 };
    std::atomic<int> framesWritten  { 0 };
    std::atomic<int> framesDropped  { 0 };

private:
    void flush()
    {
        constexpr int kFlushChunk = 4096;
        juce::AudioBuffer<float> mono (1, kFlushChunk);
        int remaining   = fifo.getNumReady();
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

       #if JUCE_DEBUG
        if (totalFlushed > 0)
        {
            juce::Logger::writeToLog ("WriterThread: flush() wrote " + juce::String (totalFlushed) + " frames");
        }
       #endif
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
//
//  Inline panel — sits below the MIDI group panels, full width.
//
//  Layout (fixed height kPanelHeight):
//
//   ┌─────────────────────────────────────────────────────────────┐
//   │ ⬤ TRACK RECORDER  [status text ...]  [path] [Browse] [REC] │  ← control bar
//   ├─────────────────────────────────────────────────────────────┤
//   │  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○  ○ │  ← arm LEDs
//   │  M  M  1  2  3  4  5  6  7  8  9  10 11 12 FX FX DR DR EX EX│  ← vertical labels + VU meters
//   └─────────────────────────────────────────────────────────────┘
// ============================================================
class AudioRecorderComponent : public juce::Component,
                                private juce::Timer
{
public:
    // Total height the panel wants in the parent layout
    static constexpr int kPanelHeight = 180;

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

        // ── Status label ──────────────────────────────────────
        statusLabel.setColour (juce::Label::textColourId, SetupUI::labelsColor);
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setText ("Scanning...", juce::dontSendNotification);
        addAndMakeVisible (statusLabel);

        // ── Output directory path ─────────────────────────────
        outputDirPathLabel.setColour (juce::Label::textColourId, juce::Colour (0xff88c8ff));
        outputDirPathLabel.setJustificationType (juce::Justification::centredRight);
        outputDirPathLabel.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (outputDirPathLabel);
        updateDirPathLabel();

        // ── Browse button ─────────────────────────────────────
        browseButton.setButtonText ("Browse...");
        browseButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2d30));
        browseButton.setColour (juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        browseButton.onClick = [this] { browseForOutputDirectory(); };
        addAndMakeVisible (browseButton);

        // ── Record button ─────────────────────────────────────
        recordButton.setButtonText (kIdleText);
        recordButton.setClickingTogglesState (true);
        recordButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff2a2d30));
        recordButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        recordButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffcc0000));
        recordButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        recordButton.setEnabled (false);
        recordButton.onClick = [this] { onRecordToggled(); };
        addAndMakeVisible (recordButton);

        // ── Channel strip ─────────────────────────────────────
        channelStrip = std::make_unique<ChannelStrip> (
            tracks, [this](int i){ onTrackArmToggled (i); }, vuLevels);
        addAndMakeVisible (*channelStrip);

        engine.setVuAudioCallback (
            [this] (const float* const* samples, int numCh, int numFrames)
            {
                vuLevels.pushAudio (samples, numCh, numFrames);
            });

        refreshTracks();

        startTimerHz (30);
    }

    ~AudioRecorderComponent() override
    {
        stopTimer();

        engine.setVuAudioCallback (nullptr);

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
        const auto b = getLocalBounds().toFloat();

        // Panel background — matches other group panels
        g.setColour (juce::Colour (0xee191b1e));
        g.fillRoundedRectangle (b, 7.0f);
        g.setColour (juce::Colours::white);
        g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.0f);

        // Control bar separator
        const float sepY = (float) kControlBarH;
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawHorizontalLine ((int) sepY, 8.0f, (float) getWidth() - 8.0f);

        // Group title — left-aligned in control bar
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.drawText ("OB AUDIO RECORDER",
                    kTitleX, 0, kTitleW, kControlBarH,
                    juce::Justification::centredLeft, false);

        // Red record indicator dot when recording
        if (recordButton.getToggleState())
        {
            g.setColour (juce::Colour (0xffff2211).withAlpha (0.85f));
            const float dotR = 4.0f;
            g.fillEllipse ((float) kTitleX - 12.0f,
                           (float) kControlBarH * 0.5f - dotR,
                           dotR * 2.0f, dotR * 2.0f);
        }
    }

    void resized() override
    {
        // ── Control bar ───────────────────────────────────────
        auto bar = getLocalBounds()
                       .removeFromTop (kControlBarH)
                       .reduced (8, 0);

        // Reserve title area on the left
        bar.removeFromLeft (kTitleX + kTitleW - 8);

        // Record button on the far right
        recordButton.setBounds (bar.removeFromRight (88).withSizeKeepingCentre (88, 22));
        bar.removeFromRight (4);

        // Browse button
        browseButton.setBounds (bar.removeFromRight (64).withSizeKeepingCentre (64, 22));
        bar.removeFromRight (4);

        // Dir path label fills the gap between title and browse button
        // Status label takes the left portion, dir label the right
        const int half = bar.getWidth() / 2;
        statusLabel.setBounds (bar.removeFromLeft (half));
        outputDirPathLabel.setBounds (bar);

        // ── Channel strip fills remaining height ──────────────
        if (channelStrip)
            channelStrip->setBounds (
                getLocalBounds().withTrimmedTop (kControlBarH + 2));
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
    // ── Geometry constants ────────────────────────────────────
    static constexpr int kControlBarH = 36;
    static constexpr int kTitleX      = 14;   // left margin for title text
    static constexpr int kTitleW      = 220;  // width reserved for title

    static constexpr const char* kIdleText   = "RECORD";
    static constexpr const char* kActiveText = "STOP";

    // ── Timer: VU decay + repaint at 20 Hz; device probe at 1 Hz ─
    void timerCallback() override
    {
        vuLevels.decayAll();

        // Repaint the channel strip every tick for smooth VU animation.
        if (channelStrip)
            channelStrip->repaint();

        // Keep the device-scan at its original 1 Hz rate to avoid hammering
        // libusb_get_device_list() unnecessarily.
        if (++timerTickCount >= 20)
        {
            timerTickCount = 0;
            if (! isRecording())
                refreshTracks();
        }
    }

    // ── Device scan ───────────────────────────────────────────
    void refreshTracks()
    {
        const auto ds = engine.probe();
        const bool ready = (ds == OverbridgeEngine::DeviceState::Ready
                         || ds == OverbridgeEngine::DeviceState::Running);

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

        // Keep USB stream alive whenever device is ready so the
        // EnvelopeFollowerEngine receives audio continuously.
        if (ready && ! engine.isRunning())
            engine.start();
    
        setStatus (engine.getStatusText());
        if (channelStrip) channelStrip->repaint();
        updateRecordButton();
        repaint();
    }

    // ── Arm toggle ────────────────────────────────────────────
    void onTrackArmToggled (int idx)
    {
        if (idx < 0 || idx >= (int) tracks.size()) return;
        tracks[(size_t) idx].isArmed = ! tracks[(size_t) idx].isArmed;
        if (channelStrip) channelStrip->repaint();
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
        if (recordButton.getToggleState()) startRecording();
        else                               stopRecording();
    }

    void startRecording()
    {
        std::vector<int> armedIndices;
        for (const auto& t : tracks)
            if (t.isArmed)
                armedIndices.push_back (t.deviceChannelIndex);

        if (armedIndices.empty())
        {
            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        const juce::Time now = juce::Time::getCurrentTime();
        const juce::String timestamp =
            juce::String::formatted ("%04d_%02d-%02d_%02d_%02d_%02d",
                now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                now.getHours(), now.getMinutes(), now.getSeconds());

        if (! writerThread.openWriters (outputDirectory, armedIndices, timestamp))
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon, "Recording error",
                "Could not create output files in:\n" + outputDirectory.getFullPathName()
                + "\n\nCheck that the folder exists and is writable.");
            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        writerThread.framesReceived.store (0, std::memory_order_relaxed);
        writerThread.framesWritten.store  (0, std::memory_order_relaxed);
        writerThread.framesDropped.store  (0, std::memory_order_relaxed);

        engine.setAudioCallback (
            [this] (const float* const* samples, int numCh, int numFrames)
            { writerThread.pushSamples (samples, numCh, numFrames); });

        writerThread.startThread (juce::Thread::Priority::normal);

        if (! engine.start())
        {
            writerThread.stopThread (2000);
            writerThread.closeWriters();
            engine.setAudioCallback (nullptr);
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon, "Recording error",
                "Failed to start USB capture:\n" + engine.getStatusText());
            recordButton.setToggleState (false, juce::dontSendNotification);
            return;
        }

        recordButton.setButtonText (kActiveText);
        setStatus ("Recording - " + juce::String ((int) armedIndices.size())
                   + " ch  ->  " + outputDirectory.getFullPathName());
        repaint();  // refresh recording dot

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("AudioRecorder: started recording "
            + juce::String ((int) armedIndices.size()) + " channels to "
            + outputDirectory.getFullPathName());
       #endif
    }

    void stopRecording()
    {
        // Do NOT stop the OverbridgeEngine here: EnvelopeFollowerEngine
        // still needs the USB stream to track audio amplitude.
        // The engine stops only on device disconnect or app exit.
        // engine.stop();

        engine.setAudioCallback (nullptr);
        writerThread.stopThread (5000);

       #if JUCE_DEBUG
        juce::Logger::writeToLog ("AudioRecorder: writer stats:"
            "  received=" + juce::String (writerThread.framesReceived.load()) +
            "  written="  + juce::String (writerThread.framesWritten.load()) +
            "  dropped="  + juce::String (writerThread.framesDropped.load()) +
            "  duration=" + juce::String (writerThread.framesWritten.load() / 48000.0, 2) + "s");
       #endif

        writerThread.closeWriters();
        recordButton.setButtonText (kIdleText);
        setStatus ("Saved to  " + outputDirectory.getFullPathName());
        repaint();

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
        // Show only the last two path components to keep it compact
        const juce::String full = outputDirectory.getFullPathName();
        const juce::String parent = outputDirectory.getParentDirectory().getFileName();
        const juce::String name   = outputDirectory.getFileName();
        const juce::String display = (parent.isEmpty() ? full
                                      : ".../" + parent + "/" + name);
        outputDirPathLabel.setText (display, juce::dontSendNotification);
        outputDirPathLabel.setTooltip (full);
    }

    void setStatus (const juce::String& t)
    { statusLabel.setText (t, juce::dontSendNotification); }


    // ==========================================================
    //  ChannelStrip
    //
    //  Horizontal row: one column per Syntakt channel.
    //  Each column contains:
    //    • a round arm LED button (clickable)
    //    • the track name drawn vertically below it
    //    • a 7-LED VU meter centred in the label area
    //
    //  Column width = component width / numChannels.
    //  The track name is drawn rotated -90° so it reads
    //  bottom-to-top, fitting neatly in the label area.
    // ==========================================================
    class ChannelStrip : public juce::Component
    {
    public:
        using ArmCb = std::function<void(int)>;

        ChannelStrip (std::vector<SyntaktAudioTrack>& tr,
                      ArmCb                           cb,
                      VuMeterLevelStore&              store)
            : tracks (tr),
              onArmToggled (std::move (cb)),
              vuLevels (store)
        { setInterceptsMouseClicks (true, false); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (tracks.empty()) return;
            const float colW = (float) getWidth() / (float) tracks.size();
            const int col = (int) ((float) e.x / colW);
            if (col >= 0 && col < (int) tracks.size() && onArmToggled)
                onArmToggled (col);
        }

        void paint (juce::Graphics& g) override
        {
            const int n = (int) tracks.size();
            if (n == 0)
            {
                g.setColour (SetupUI::labelsColor.withAlpha (0.3f));
                g.drawText ("Device not connected - plug in Syntakt (Overbridge mode)",
                            0, 0, getWidth(), getHeight(),
                            juce::Justification::centred, false);
                return;
            }

            const float colW    = (float) getWidth() / (float) n;
            const float btnDiam = juce::jmin (colW - 8.0f, (float) kBtnMaxDiam);
            const float btnY    = (float) kBtnTopPad;

            for (int i = 0; i < n; ++i)
            {
                const auto& t  = tracks[(size_t) i];
                const float cx = colW * (float) i + colW * 0.5f;

                // ── Column background (subtle tint for main mix) ──
                if (t.isMainMix)
                {
                    g.setColour (juce::Colour (0xff1a2530).withAlpha (0.5f));
                    g.fillRect (juce::Rectangle<float> (
                        colW * ((float) i) + 2, 0.0f, colW, (float) getHeight() - 2));
                }

                // ── Arm LED button ────────────────────────────────
                const juce::Rectangle<float> led (
                    cx - btnDiam * 0.5f, btnY, btnDiam, btnDiam);

                if (t.isArmed)
                {
                    // Outer glow
                    g.setColour (juce::Colour (0xffcc0000).withAlpha (0.28f));
                    g.fillEllipse (led.expanded (4.0f));
                    // Bright fill
                    g.setColour (juce::Colour (0xffff2211));
                    g.fillEllipse (led);
                    // Inner highlight
                    g.setColour (juce::Colours::white.withAlpha (0.25f));
                    g.fillEllipse (led.reduced (btnDiam * 0.15f)
                                       .withY (led.getY() + btnDiam * 0.08f)
                                       .withHeight (btnDiam * 0.4f));
                }
                else
                {
                    // Subtle radial gradient effect via layered fills
                    g.setColour (juce::Colour (0xff2a2a2a));
                    g.fillEllipse (led);
                    g.setColour (juce::Colour (0xff383838));
                    g.fillEllipse (led.reduced (btnDiam * 0.15f));
                }

                // LED border
                g.setColour (t.isArmed
                             ? juce::Colour (0xffff4433).withAlpha (0.6f)
                             : juce::Colours::black.withAlpha (0.5f));
                g.drawEllipse (led.reduced (0.5f), 1.0f);

                // Channel number (tiny, above the button)
                g.setColour (juce::Colour (0xff555555));
                g.drawText (juce::String (t.deviceChannelIndex + 1),
                            (int) (cx - colW * 0.5f), 0,
                            (int) colW, kBtnTopPad - 1,
                            juce::Justification::centred, false);

                // ── Label-area geometry ───────────────────────────
                const float labelTop  = btnY + btnDiam + (float) kLabelGap;
                const float labelH    = (float) getHeight() - labelTop - (float) kBottomMargin;
                const float labelW    = colW - 2.0f;

                {
                    const float vuStripW = (float) VuMeterGeom::kVuStripW;
                    const float stripX   = cx - vuStripW * 0.5f;
                    const float level    = (t.deviceChannelIndex >= 0)
                                           ? vuLevels.getLevel (t.deviceChannelIndex)
                                           : 0.0f;
                    drawChannelVuMeter (g, stripX, labelTop, labelH, level);
                }

                // ── Vertical track label ──────────────────────────
                // Draw text rotated -90° (reads bottom to top).
                // We save/restore the Graphics transform around this.
                const juce::Colour textCol =
                    t.isArmed    ? juce::Colours::white.withAlpha (0.95f)
                  : t.isMainMix  ? juce::Colour (0xff7ab8e8)
                                 : SetupUI::labelsColor;

                g.setColour (textCol);

                // Apply rotation: pivot = centre of the label area
                const float pivotX = cx;
                const float pivotY = labelTop + labelH * 0.5f;
 
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.addTransform (juce::AffineTransform::rotation (
                        -juce::MathConstants<float>::halfPi, pivotX, pivotY));
 
                    // After rotation the bounding rectangle maps to:
                    //   width  becomes labelH (the rotated height)
                    //   height becomes labelW (the rotated width)
                    g.drawText (t.displayName,
                                (int) (pivotX - labelH * 0.38f),
                                (int) (pivotY - labelW * 0.70f),
                                (int) labelH,
                                (int) labelW,
                                juce::Justification::centredLeft,
                                true);
                }   // rotation transform released here
 
                // ── Column divider (drawn in normal coords, no rotation) ──
                if (i > 0)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.05f));
                    g.drawVerticalLine ((int) (colW * (float) i),
                                        2.0f, (float) getHeight() - 2.0f);
                }
            }
        }

    private:
        std::vector<SyntaktAudioTrack>& tracks;
        ArmCb onArmToggled;

        VuMeterLevelStore& vuLevels;

        // Geometry
        static constexpr int kBtnMaxDiam   = 26;  // LED diameter cap (px)
        static constexpr int kBtnTopPad    = 14;  // space above button (for ch number)
        static constexpr int kLabelGap     = 5;   // gap between button bottom and label top
        static constexpr int kBottomMargin = 10;  // clearance between label text and frame

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };


    // ── Members ───────────────────────────────────────────────
    OverbridgeEngine& engine;
    WriterThread      writerThread;

    juce::File                            outputDirectory;
    std::unique_ptr<juce::PropertiesFile> propertiesFile;
    std::unique_ptr<juce::FileChooser>    fileChooser;

    std::vector<SyntaktAudioTrack> tracks;

    VuMeterLevelStore vuLevels;

    int timerTickCount = 0;

    juce::Label      statusLabel;
    juce::Label      outputDirPathLabel;
    juce::TextButton browseButton, recordButton;

    std::unique_ptr<ChannelStrip> channelStrip;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioRecorderComponent)
};

#endif // JUCE_LINUX && JucePlugin_Build_Standalone && MODZTAKT_OVERBRIDGE