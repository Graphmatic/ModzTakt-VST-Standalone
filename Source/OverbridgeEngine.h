#pragma once

// ============================================================
//  OverbridgeEngine.h  —  Linux Standalone ONLY
//
//  Syntakt Overbridge 2 USB audio capture via libusb.
//
// ──────────────────────────────────────────────────────────────
//  USB DESCRIPTOR (confirmed from diagnose() dump)
// ──────────────────────────────────────────────────────────────
//
//  Interface 1  class=0xff  (OB audio streaming)
//    altsetting 0: 0 endpoints  (quiescent — device default)
//    altsetting 1: EP 0x83 IN  interrupt  maxPacket=32
//    altsetting 2: EP 0x83 IN  interrupt  maxPacket=88
//    altsetting 3: EP 0x83 IN  interrupt  maxPacket=592  ← WE USE
//
//  Interface 2  class=0xff  (OB audio playback — unused here)
//    altsetting 3: EP 0x03 OUT  interrupt  maxPacket=256
//
//  Interface 3  class=0xff  (MIDI / control)
//    EP 0x82 IN   bulk  maxPacket=64
//    EP 0x02 OUT  bulk  maxPacket=512
//
//  Interface 5  class=0x1  (standard USB Audio — NOT Overbridge audio)
//    EP 0x81 IN   bulk  maxPacket=512
//    EP 0x01 OUT  bulk  maxPacket=512
//
//  *** Interface 5 / EP 0x81 is NOT the Overbridge audio stream. ***
//  *** It returns ~4-byte payloads (control only).               ***
//
// ──────────────────────────────────────────────────────────────
//  OVERBRIDGE BLOCK FORMAT  (from overbridge.h reference + Digitakt)
// ──────────────────────────────────────────────────────────────
//
//  The Syntakt sends 24 blocks per USB interrupt transfer.
//  Each block:
//
//    Bytes [0..1]    fixed header  0x0700
//    Bytes [2..3]    sample counter (uint16 BE, +7 per block)
//    Bytes [4..31]   unknown / padding  (28 bytes)
//    Bytes [32..]    7 frames × 20 channels × 4 bytes (int32 BE)
//
//  Per-block sizes:
//    header          =  32 bytes
//    audio payload   =   7 × 20 × 4 = 560 bytes
//    block total     = 592 bytes   ← matches maxPacket=592 above!
//
//  Full transfer:
//    24 blocks × 592 = 14208 bytes per interrupt transfer
//
// ──────────────────────────────────────────────────────────────
//  SYNTAKT 20-CHANNEL LAYOUT  (Overbridge manual D.6)
// ──────────────────────────────────────────────────────────────
//   0  Main L            10  Audio Track  9
//   1  Main R            11  Audio Track 10
//   2  Audio Track  1    12  Audio Track 11
//   3  Audio Track  2    13  Audio Track 12
//   4  Audio Track  3    14  FX Track L
//   5  Audio Track  4    15  FX Track R
//   6  Audio Track  5    16  Delay/Reverb L
//   7  Audio Track  6    17  Delay/Reverb R
//   8  Audio Track  7    18  External In L
//   9  Audio Track  8    19  External In R
//
// ──────────────────────────────────────────────────────────────
//  THREAD MODEL
// ──────────────────────────────────────────────────────────────
//
//  UsbThread runs libusb_handle_events_timeout_completed in a
//  5ms loop.  On each completed interrupt transfer the callback
//  decodeAndFire() converts int32 BE samples to float32 and
//  calls audioCallback.  The transfer is immediately re-armed.
//
// ──────────────────────────────────────────────────────────────
//  DEPENDENCY / UDEV
// ──────────────────────────────────────────────────────────────
//  CMakeLists.txt:
//    find_package(PkgConfig REQUIRED)
//    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
//    target_link_libraries(ModzTakt_Standalone PRIVATE ${LIBUSB_LIBRARIES})
//    target_include_directories(ModzTakt_Standalone PRIVATE ${LIBUSB_INCLUDE_DIRS})
//
//  /etc/udev/rules.d/99-elektron-overbridge.rules:
//    SUBSYSTEM=="usb", ATTR{idVendor}=="1935", MODE="0664", GROUP="audio"
//  Then: sudo udevadm control --reload && sudo udevadm trigger
//        sudo usermod -aG audio $USER   (re-login after)
// ============================================================

#if JUCE_LINUX && defined (JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone

#include <JuceHeader.h>
#include <libusb-1.0/libusb.h>

#include <atomic>
#include <functional>
#include <vector>
#include <cstring>
#include <algorithm>

// ── Protocol constants ─────────────────────────────────────────
namespace OverbridgeProtocol
{
    // Syntakt channel count (Overbridge manual D.6)
    static constexpr int kNumChannels       = 20;
    static constexpr int kSampleRate        = 48000;

    // Block geometry (confirmed: maxPacket=592 = 32 + 7×20×4)
    static constexpr int kFramesPerBlock    = 7;
    static constexpr int kBlockHeaderBytes  = 32;   // 2 fixed + 2 counter + 28 unknown
    static constexpr int kBytesPerSample    = 4;    // int32 big-endian

    static constexpr int kAudioBytesPerBlock =
        kFramesPerBlock * kNumChannels * kBytesPerSample;   // 560

    static constexpr int kBlockBytes =
        kBlockHeaderBytes + kAudioBytesPerBlock;            // 592

    // Number of blocks per interrupt transfer (same as Digitakt)
    static constexpr int kBlocksPerTransfer = 24;

    // Total bytes per transfer
    static constexpr int kTransferBytes = kBlocksPerTransfer * kBlockBytes;  // 14208

    // USB addressing (confirmed from descriptor dump)
    static constexpr int           kAudioInterface    = 1;
    static constexpr int           kAudioAltSetting   = 3;   // activates 592-byte IN packets
    static constexpr unsigned char kEndpointIn        = 0x83;

    // OUT (playback / handshake) interface — Interface 2 alt 3, EP 0x03
    // The device only starts filling IN blocks with real audio once it
    // receives OUT blocks.  We send silent (zero-audio) blocks purely
    // to satisfy the protocol handshake.
    static constexpr int           kPlaybackInterface  = 2;
    static constexpr int           kPlaybackAltSetting = 3;
    static constexpr unsigned char kEndpointOut        = 0x03;

    // OUT block: 32-byte header + 7 frames × 2 channels × 4 bytes = 88 bytes
    // (The Syntakt only expects stereo playback: Main L/R)
    // 24 blocks × 88 = 2112 bytes per OUT transfer
    static constexpr int kOutBlockBytes    = 88;
    static constexpr int kOutTransferBytes = kBlocksPerTransfer * kOutBlockBytes; // 2112

    // OUT block header magic (TO-device direction, from overbridge.h)
    static constexpr uint16_t kOutHeaderMagic = 0x07FF;

    // Transfer pool size
    static constexpr int kNumTransfers = 8;

    // Normalisation: int32 → float ±1.0
    static constexpr float kInt32Norm = 1.0f / 2147483648.0f;

    // Set true to hex-dump the first received block header to the JUCE
    // logger — lets you verify the 0x0700 magic and header size.
    // Also logs min/max decoded float values to confirm non-zero audio.
    static constexpr bool kLogFirstBlock = true;
}


// ── Channel descriptor ─────────────────────────────────────────
struct OverbridgeChannel
{
    int          index     { -1 };
    juce::String name;
    bool         isMainMix { false };
};


// ============================================================
//  OverbridgeEngine
// ============================================================
class OverbridgeEngine
{
public:
    enum class DeviceState
    {
        NotConnected,
        WrongMode,   ///< Elektron device detected in USB-MIDI mode
        Ready,       ///< OB device open, not yet capturing
        Running,     ///< Interrupt transfer loop active
        Error
    };

    /// Called on UsbThread for every decoded block group.
    /// samples[ch] — numFrames float32, non-interleaved.
    using AudioCallback = std::function<void (const float* const* samples,
                                              int                 numChannels,
                                              int                 numFrames)>;

    /// Reserved — MIDI travels via snd-usb-midi / JUCE, not this engine.
    using MidiCallback  = std::function<void (const juce::MidiMessage&)>;

    // ── USB IDs ───────────────────────────────────────────────
    static constexpr uint16_t kElektronVID  = 0x1935;
    static constexpr int      kSampleRate   = OverbridgeProtocol::kSampleRate;
    static constexpr int      kNumChannels  = OverbridgeProtocol::kNumChannels;

    static constexpr uint16_t kKnownOBPids[] = {
        0x000c, 0x0b2c,   // Digitakt
        0x0014,           // Digitone
        0x0010,           // Analog Rytm MKII
        0x000e,           // Analog Four MKII
        0x0b4a,           // Syntakt
    };
    static constexpr uint16_t kKnownMidiPids[] = {
        0x000d, 0x102c,   // Digitakt
        0x0015,           // Digitone
        0x104a,           // Syntakt MIDI mode
    };

    // ── Fixed 20-channel layout ───────────────────────────────
    static const std::vector<OverbridgeChannel>& syntaktChannels()
    {
        static const std::vector<OverbridgeChannel> ch = []
        {
            std::vector<OverbridgeChannel> v;
            v.reserve (kNumChannels);
            auto add = [&](int i, const char* n, bool m)
            { OverbridgeChannel c; c.index=i; c.name=n; c.isMainMix=m; v.push_back(c); };

            add ( 0, "Main L",            true);
            add ( 1, "Main R",            true);
            add ( 2, "Audio Track 1",     false);
            add ( 3, "Audio Track 2",     false);
            add ( 4, "Audio Track 3",     false);
            add ( 5, "Audio Track 4",     false);
            add ( 6, "Audio Track 5",     false);
            add ( 7, "Audio Track 6",     false);
            add ( 8, "Audio Track 7",     false);
            add ( 9, "Audio Track 8",     false);
            add (10, "Audio Track 9",     false);
            add (11, "Audio Track 10",    false);
            add (12, "Audio Track 11",    false);
            add (13, "Audio Track 12",    false);
            add (14, "FX Track L",        false);
            add (15, "FX Track R",        false);
            add (16, "Delay/Reverb L",    false);
            add (17, "Delay/Reverb R",    false);
            add (18, "External In L",     false);
            add (19, "External In R",     false);

            return v;
        }();
        return ch;
    }

    // ── Construction ─────────────────────────────────────────
    OverbridgeEngine()
    {
        if (libusb_init (&usbContext) != LIBUSB_SUCCESS)
        {
            usbContext = nullptr;
            state      = DeviceState::Error;
            statusText = "Failed to initialise libusb";
        }
    }

    ~OverbridgeEngine()
    {
        stop();
        closeDevice();
        if (usbContext) { libusb_exit (usbContext); usbContext = nullptr; }
    }

    // ── Callback wiring ───────────────────────────────────────
    void setAudioCallback (AudioCallback cb) { audioCallback = std::move (cb); }
    void setMidiCallback  (MidiCallback  cb) { midiCallback  = std::move (cb); }

    // ── Probe ─────────────────────────────────────────────────
    DeviceState probe()
    {
        if (state == DeviceState::Running) return state;

        closeDevice();

        if (! usbContext)
        { state = DeviceState::Error; statusText = "libusb context missing"; return state; }

        libusb_device** devList = nullptr;
        ssize_t n = libusb_get_device_list (usbContext, &devList);
        if (n < 0)
        { state = DeviceState::Error; statusText = "libusb_get_device_list failed"; return state; }

        DeviceState     best    = DeviceState::NotConnected;
        libusb_device*  bestDev = nullptr;
        uint16_t        foundPid = 0;

        for (ssize_t i = 0; i < n; ++i)
        {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor (devList[i], &desc) != LIBUSB_SUCCESS) continue;
            if (desc.idVendor != kElektronVID) continue;

            if (isOBPid (desc.idProduct))
            { best = DeviceState::Ready; bestDev = devList[i]; foundPid = desc.idProduct; break; }

            if (isMidiPid (desc.idProduct) && best == DeviceState::NotConnected)
            { best = DeviceState::WrongMode; foundPid = desc.idProduct; }

            if (best != DeviceState::Ready && probeBySyntaktName (devList[i], desc))
            { best = DeviceState::Ready; bestDev = devList[i]; foundPid = desc.idProduct; break; }
        }

        state = best;

        if (state == DeviceState::Ready && bestDev)
        {
            if (libusb_open (bestDev, &deviceHandle) == LIBUSB_SUCCESS)
            {
                detectedPid = foundPid;
                statusText  = "Syntakt OB mode  (PID 0x"
                              + juce::String::toHexString (foundPid).toUpperCase()
                              + ")  —  " + juce::String (kNumChannels) + " ch ready";
            }
            else
            {
                state = DeviceState::Error;
                deviceHandle = nullptr;
                statusText = "Found OB device but could not open it.\n"
                             "Check /etc/udev/rules.d/99-elektron-overbridge.rules";
            }
        }
        else if (state == DeviceState::WrongMode)
        {
            statusText = "Syntakt found in USB-MIDI mode.\n"
                         "Switch: Settings → System → USB Config → Overbridge";
        }
        else if (state == DeviceState::NotConnected)
        {
            statusText = "No Elektron device found.\n"
                         "Connect Syntakt → USB Config → Overbridge.";
        }

        libusb_free_device_list (devList, 1);
        return state;
    }

    // ── Accessors ─────────────────────────────────────────────
    DeviceState         getState()      const noexcept { return state;      }
    const juce::String& getStatusText() const noexcept { return statusText; }
    bool isReady()   const noexcept { return state == DeviceState::Ready;   }
    bool isRunning() const noexcept { return state == DeviceState::Running; }

    // ── Diagnose ──────────────────────────────────────────────
    void diagnose()
    {
        if (! deviceHandle)
        { juce::Logger::writeToLog ("diagnose — no device open"); return; }

        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor (libusb_get_device (deviceHandle), &cfg))
        { juce::Logger::writeToLog ("diagnose — cannot get config"); return; }

        juce::Logger::writeToLog ("──── Syntakt USB descriptor dump ────");
        juce::Logger::writeToLog ("  NumInterfaces: " + juce::String (cfg->bNumInterfaces));

        for (int i = 0; i < (int) cfg->bNumInterfaces; ++i)
        {
            const libusb_interface& iface = cfg->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a)
            {
                const libusb_interface_descriptor& alt = iface.altsetting[a];
                juce::Logger::writeToLog (
                    "  Interface " + juce::String (alt.bInterfaceNumber)
                    + "  alt=" + juce::String (alt.bAlternateSetting)
                    + "  class=0x" + juce::String::toHexString (alt.bInterfaceClass)
                    + "  eps=" + juce::String (alt.bNumEndpoints));

                for (int e = 0; e < (int) alt.bNumEndpoints; ++e)
                {
                    const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                    const int  xferType = ep.bmAttributes & 0x03;
                    const bool isIn     = (ep.bEndpointAddress & 0x80) != 0;
                    const char* typeName = (xferType == 0) ? "ctrl"
                                         : (xferType == 1) ? "iso"
                                         : (xferType == 2) ? "bulk"
                                         : "intr";
                    juce::Logger::writeToLog (
                        "    EP 0x" + juce::String::toHexString (ep.bEndpointAddress).toUpperCase()
                        + "  " + juce::String (isIn ? "IN" : "OUT")
                        + "  " + typeName
                        + "  maxPkt=" + juce::String (ep.wMaxPacketSize));
                }
            }
        }
        juce::Logger::writeToLog ("─────────────────────────────────────");
        libusb_free_config_descriptor (cfg);
    }

    // ── Start ─────────────────────────────────────────────────
    bool start()
    {
        if (state == DeviceState::Running) return true;

        if (state != DeviceState::Ready || ! deviceHandle)
        {
            statusText = "Cannot start: device not in Ready state.";
            juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
            return false;
        }

        juce::Logger::writeToLog (
            "OverbridgeEngine::start"
            "  interface="   + juce::String (OverbridgeProtocol::kAudioInterface)
            + "  altSetting=" + juce::String (OverbridgeProtocol::kAudioAltSetting)
            + "  ep=0x"       + juce::String::toHexString (OverbridgeProtocol::kEndpointIn).toUpperCase()
            + "  blockBytes=" + juce::String (OverbridgeProtocol::kBlockBytes)
            + "  framesPerBlock=" + juce::String (OverbridgeProtocol::kFramesPerBlock)
            + "  channels="   + juce::String (OverbridgeProtocol::kNumChannels)
            + "  transferBytes=" + juce::String (OverbridgeProtocol::kTransferBytes));

        // ── Detach kernel driver if needed ────────────────────
        const int kdrv = libusb_kernel_driver_active (
                             deviceHandle, OverbridgeProtocol::kAudioInterface);
        juce::Logger::writeToLog ("  kernel_driver_active=" + juce::String (kdrv));

        if (kdrv == 1)
        {
            const int r = libusb_detach_kernel_driver (
                              deviceHandle, OverbridgeProtocol::kAudioInterface);
            if (r != LIBUSB_SUCCESS)
            {
                statusText = juce::String ("Could not detach kernel driver: ")
                             + libusb_error_name (r);
                juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
                return false;
            }
            kernelDriverDetached = true;
            juce::Logger::writeToLog ("  kernel driver detached OK");
        }
        else if (kdrv < 0)
        {
            juce::Logger::writeToLog (
                "  kernel_driver_active error: "
                + juce::String (libusb_error_name (kdrv)) + " (continuing)");
        }

        // ── Claim interface ───────────────────────────────────
        const int cr = libusb_claim_interface (
                           deviceHandle, OverbridgeProtocol::kAudioInterface);
        juce::Logger::writeToLog (
            "  claim_interface=" + juce::String (cr)
            + (cr == LIBUSB_SUCCESS ? " OK"
               : juce::String (" ") + libusb_error_name (cr)));

        if (cr != LIBUSB_SUCCESS)
        {
            statusText = juce::String ("Could not claim interface ")
                         + juce::String (OverbridgeProtocol::kAudioInterface)
                         + ": " + libusb_error_name (cr);
            juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
            reattachKernelDriverIfNeeded();
            return false;
        }
        interfaceClaimed = true;

        // ── Select alternate setting 3 (592-byte packets) ─────
        // altsetting 0 has no endpoints; we must switch to alt 3
        // before the endpoint exists to submit transfers against.
        const int ar = libusb_set_interface_alt_setting (
                           deviceHandle,
                           OverbridgeProtocol::kAudioInterface,
                           OverbridgeProtocol::kAudioAltSetting);
        juce::Logger::writeToLog (
            "  set_interface_alt_setting(" +
            juce::String (OverbridgeProtocol::kAudioInterface) + ", " +
            juce::String (OverbridgeProtocol::kAudioAltSetting) + ")=" +
            juce::String (ar) +
            (ar == LIBUSB_SUCCESS ? " OK"
             : juce::String (" ") + libusb_error_name (ar)));

        if (ar != LIBUSB_SUCCESS)
        {
            statusText = juce::String ("Could not set alt setting ")
                         + juce::String (OverbridgeProtocol::kAudioAltSetting)
                         + " on interface "
                         + juce::String (OverbridgeProtocol::kAudioInterface)
                         + ": " + libusb_error_name (ar);
            juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
            libusb_release_interface (deviceHandle, OverbridgeProtocol::kAudioInterface);
            interfaceClaimed = false;
            reattachKernelDriverIfNeeded();
            return false;
        }

        // ── Clear IN endpoint halt ────────────────────────────
        const int ch_r = libusb_clear_halt (deviceHandle,
                                             OverbridgeProtocol::kEndpointIn);
        juce::Logger::writeToLog (
            "  clear_halt(EP 0x"
            + juce::String::toHexString (OverbridgeProtocol::kEndpointIn).toUpperCase()
            + ")=" + juce::String (ch_r)
            + (ch_r == LIBUSB_SUCCESS ? " OK"
               : juce::String (" ") + libusb_error_name (ch_r)));

        // ── Claim playback interface (Interface 2) ────────────
        // The device requires receiving OUT blocks before it sends
        // real audio on the IN endpoint.  We claim Interface 2 and
        // submit silent OUT transfers purely to satisfy this handshake.
        const int cr2 = libusb_claim_interface (
                            deviceHandle, OverbridgeProtocol::kPlaybackInterface);
        juce::Logger::writeToLog (
            "  claim_interface(" + juce::String (OverbridgeProtocol::kPlaybackInterface)
            + ")=" + juce::String (cr2)
            + (cr2 == LIBUSB_SUCCESS ? " OK"
               : juce::String (" ") + libusb_error_name (cr2)));

        if (cr2 != LIBUSB_SUCCESS)
        {
            statusText = juce::String ("Could not claim playback interface ")
                         + juce::String (OverbridgeProtocol::kPlaybackInterface)
                         + ": " + libusb_error_name (cr2);
            juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
            libusb_release_interface (deviceHandle, OverbridgeProtocol::kAudioInterface);
            interfaceClaimed = false;
            reattachKernelDriverIfNeeded();
            return false;
        }
        playbackInterfaceClaimed = true;

        const int ar2 = libusb_set_interface_alt_setting (
                            deviceHandle,
                            OverbridgeProtocol::kPlaybackInterface,
                            OverbridgeProtocol::kPlaybackAltSetting);
        juce::Logger::writeToLog (
            "  set_interface_alt_setting("
            + juce::String (OverbridgeProtocol::kPlaybackInterface) + ", "
            + juce::String (OverbridgeProtocol::kPlaybackAltSetting) + ")="
            + juce::String (ar2)
            + (ar2 == LIBUSB_SUCCESS ? " OK"
               : juce::String (" ") + libusb_error_name (ar2)));

        if (ar2 != LIBUSB_SUCCESS)
        {
            statusText = juce::String ("Could not set playback alt setting: ")
                         + libusb_error_name (ar2);
            juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
            stop(); return false;
        }

        // Clear OUT endpoint halt too
        const int ch_out = libusb_clear_halt (deviceHandle,
                                               OverbridgeProtocol::kEndpointOut);
        juce::Logger::writeToLog (
            "  clear_halt(EP 0x"
            + juce::String::toHexString (OverbridgeProtocol::kEndpointOut).toUpperCase()
            + ")=" + juce::String (ch_out)
            + (ch_out == LIBUSB_SUCCESS ? " OK"
               : juce::String (" ") + libusb_error_name (ch_out)));

        // ── Build OUT transfer buffers (pre-fill with headers) ─
        // Each OUT transfer contains kBlocksPerTransfer blocks.
        // Each block: bytes 0-1 = 0x07FF magic, bytes 2-3 = sample
        // counter uint16 BE (increments by kFramesPerBlock per block),
        // bytes 4-31 = zeros, bytes 32-87 = zero audio (silent).
        outSampleCounter.store (0, std::memory_order_relaxed);

        outTransfers.resize      ((size_t) OverbridgeProtocol::kNumTransfers, nullptr);
        outTransferBuffers.resize ((size_t) OverbridgeProtocol::kNumTransfers);

        for (size_t i = 0; i < outTransfers.size(); ++i)
        {
            auto& buf = outTransferBuffers[i];
            buf.assign ((size_t) OverbridgeProtocol::kOutTransferBytes, 0);
            buildOutTransferBuffer (buf.data());

            outTransfers[i] = libusb_alloc_transfer (0);
            if (! outTransfers[i])
            {
                statusText = "libusb_alloc_transfer (OUT) returned null";
                juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
                stop(); return false;
            }

            libusb_fill_interrupt_transfer (
                outTransfers[i],
                deviceHandle,
                OverbridgeProtocol::kEndpointOut,
                buf.data(),
                OverbridgeProtocol::kOutTransferBytes,
                &OverbridgeEngine::outTransferCallback,
                this,
                5000);

            const int so = libusb_submit_transfer (outTransfers[i]);
            if (so != LIBUSB_SUCCESS)
            {
                statusText = juce::String ("OUT libusb_submit_transfer failed: ")
                             + libusb_error_name (so);
                juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
                stop(); diagnose(); return false;
            }
        }
        juce::Logger::writeToLog (
            "  OUT transfers submitted: " + juce::String (outTransfers.size()));

        // ── Allocate scratch decode buffer ────────────────────
        const int framesPerTransfer =
            OverbridgeProtocol::kFramesPerBlock * OverbridgeProtocol::kBlocksPerTransfer;

        scratchNonInterleaved.assign (
            (size_t) (kNumChannels * framesPerTransfer), 0.0f);
        scratchPtrs.resize ((size_t) kNumChannels);
        for (int ch = 0; ch < kNumChannels; ++ch)
            scratchPtrs[(size_t) ch] =
                scratchNonInterleaved.data() + ch * framesPerTransfer;

        // ── Allocate + submit IN interrupt transfers ──────────
        const int bufSize = OverbridgeProtocol::kTransferBytes;

        transfers.resize      ((size_t) OverbridgeProtocol::kNumTransfers, nullptr);
        transferBuffers.resize ((size_t) OverbridgeProtocol::kNumTransfers);

        shouldStopCapture.store  (false, std::memory_order_release);
        firstBlockLogged.store   (false, std::memory_order_release);
        transfersCompleted.store (0, std::memory_order_relaxed);
        transferErrors.store     (0, std::memory_order_relaxed);
        totalBytesReceived.store (0, std::memory_order_relaxed);
        activeTransferSlots.store (OverbridgeProtocol::kNumTransfers, std::memory_order_relaxed);

        for (size_t i = 0; i < transfers.size(); ++i)
        {
            transferBuffers[i].assign ((size_t) bufSize, 0);

            transfers[i] = libusb_alloc_transfer (0);
            if (! transfers[i])
            {
                statusText = "libusb_alloc_transfer returned null";
                juce::Logger::writeToLog ("OverbridgeEngine::start — " + statusText);
                stop(); return false;
            }

            libusb_fill_interrupt_transfer (
                transfers[i],
                deviceHandle,
                OverbridgeProtocol::kEndpointIn,
                transferBuffers[i].data(),
                bufSize,
                &OverbridgeEngine::transferCallback,
                this,
                5000);

            const int sr = libusb_submit_transfer (transfers[i]);
            if (sr != LIBUSB_SUCCESS)
            {
                statusText = juce::String ("IN libusb_submit_transfer failed: ")
                             + libusb_error_name (sr);
                juce::Logger::writeToLog ("OverbridgeEngine::start — transfer "
                    + juce::String ((int) i) + ": " + statusText);
                stop(); diagnose(); return false;
            }
        }

        usbThread = std::make_unique<UsbThread> (*this);
        usbThread->startThread (juce::Thread::Priority::highest);

        state      = DeviceState::Running;
        statusText = "Recording  —  "
                     + juce::String (kNumChannels) + " ch @ 48 kHz";
        juce::Logger::writeToLog ("OverbridgeEngine::start — capture running OK");
        return true;
    }

    // ── Stop ──────────────────────────────────────────────────
    void stop()
    {
        if (state != DeviceState::Running && ! usbThread) return;

        shouldStopCapture.store (true, std::memory_order_release);

        for (auto* t : transfers)    if (t) libusb_cancel_transfer (t);
        for (auto* t : outTransfers) if (t) libusb_cancel_transfer (t);

        if (usbThread) { usbThread->stopThread (3000); usbThread.reset(); }

        for (auto* t : transfers)    if (t) libusb_free_transfer (t);
        for (auto* t : outTransfers) if (t) libusb_free_transfer (t);
        transfers.clear();
        transferBuffers.clear();
        outTransfers.clear();
        outTransferBuffers.clear();
        scratchNonInterleaved.clear();
        scratchPtrs.clear();

        if (playbackInterfaceClaimed)
        {
            libusb_set_interface_alt_setting (deviceHandle,
                OverbridgeProtocol::kPlaybackInterface, 0);
            libusb_release_interface (deviceHandle,
                OverbridgeProtocol::kPlaybackInterface);
            playbackInterfaceClaimed = false;
        }

        if (interfaceClaimed)
        {
            libusb_set_interface_alt_setting (deviceHandle,
                OverbridgeProtocol::kAudioInterface, 0);
            libusb_release_interface (deviceHandle,
                OverbridgeProtocol::kAudioInterface);
            interfaceClaimed = false;
        }
        reattachKernelDriverIfNeeded();

        if (state == DeviceState::Running)
        {
            juce::Logger::writeToLog (
                "OverbridgeEngine::stop — stats:"
                "  completed="  + juce::String (transfersCompleted.load()) +
                "  errors="     + juce::String (transferErrors.load()) +
                "  bytes="      + juce::String ((int64_t) totalBytesReceived.load()) +
                "  activeSlots="+ juce::String (activeTransferSlots.load()));
            state = DeviceState::Ready;
        }

        juce::Logger::writeToLog ("OverbridgeEngine::stop — done");
    }

private:

    // ── UsbThread ─────────────────────────────────────────────
    struct UsbThread : public juce::Thread
    {
        explicit UsbThread (OverbridgeEngine& e)
            : juce::Thread ("OB-USB"), engine (e) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                struct timeval tv { 0, 5000 };  // 5ms
                libusb_handle_events_timeout_completed (
                    engine.usbContext, &tv, nullptr);
            }
            // Drain after cancellation
            for (int i = 0; i < 20; ++i)
            {
                struct timeval tv { 0, 5000 };
                libusb_handle_events_timeout_completed (
                    engine.usbContext, &tv, nullptr);
            }
        }
        OverbridgeEngine& engine;
    };

    // ── OUT buffer builder ────────────────────────────────────
    // Fills a complete OUT transfer buffer with kBlocksPerTransfer
    // silent blocks, each with the correct 0x07FF header and an
    // incrementing sample counter.
    void buildOutTransferBuffer (uint8_t* buf)
    {
        using namespace OverbridgeProtocol;
        uint16_t ctr = outSampleCounter.load (std::memory_order_relaxed);

        for (int b = 0; b < kBlocksPerTransfer; ++b)
        {
            uint8_t* block = buf + b * kOutBlockBytes;
            std::memset (block, 0, (size_t) kOutBlockBytes);

            // Bytes 0-1: magic 0x07FF (big-endian)
            block[0] = (uint8_t) (kOutHeaderMagic >> 8);
            block[1] = (uint8_t) (kOutHeaderMagic & 0xFF);

            // Bytes 2-3: sample counter (big-endian)
            block[2] = (uint8_t) (ctr >> 8);
            block[3] = (uint8_t) (ctr & 0xFF);

            ctr = (uint16_t) (ctr + (uint16_t) kFramesPerBlock);
        }

        outSampleCounter.store (ctr, std::memory_order_relaxed);
    }

    // ── Static OUT transfer callback ──────────────────────────
    static void LIBUSB_CALL outTransferCallback (libusb_transfer* t)
    {
        static_cast<OverbridgeEngine*> (t->user_data)->handleOutTransfer (t);
    }

    void handleOutTransfer (libusb_transfer* t)
    {
        if (shouldStopCapture.load (std::memory_order_acquire)) return;

        if (t->status != LIBUSB_TRANSFER_COMPLETED
            && t->status != LIBUSB_TRANSFER_TIMED_OUT
            && t->status != LIBUSB_TRANSFER_CANCELLED)
        {
            juce::Logger::writeToLog (
                "OverbridgeEngine: OUT transfer status="
                + juce::String ((int) t->status)
                + " (" + libusb_error_name ((int) t->status) + ")");
        }

        if (t->status == LIBUSB_TRANSFER_CANCELLED) return;

        // Re-fill buffer with fresh headers and re-arm
        if (! shouldStopCapture.load (std::memory_order_acquire))
        {
            buildOutTransferBuffer (t->buffer);
            t->length = OverbridgeProtocol::kOutTransferBytes;
            libusb_submit_transfer (t);
        }
    }

    // ── Static IN transfer callback ───────────────────────────
    static void LIBUSB_CALL transferCallback (libusb_transfer* t)
    {
        static_cast<OverbridgeEngine*> (t->user_data)->handleTransfer (t);
    }

    void handleTransfer (libusb_transfer* t)
    {
        if (shouldStopCapture.load (std::memory_order_acquire)) return;

        switch (t->status)
        {
            case LIBUSB_TRANSFER_COMPLETED:
            case LIBUSB_TRANSFER_TIMED_OUT:
                if (t->actual_length > 0)
                {
                    ++transfersCompleted;
                    totalBytesReceived.fetch_add ((uint64_t) t->actual_length,
                                                  std::memory_order_relaxed);
                    decodeAndFire (t->buffer, t->actual_length);
                }
                break;

            case LIBUSB_TRANSFER_CANCELLED:
                return;   // expected during stop — do NOT re-arm

            default:
                ++transferErrors;
                juce::Logger::writeToLog (
                    "OverbridgeEngine: transfer status="
                    + juce::String ((int) t->status)
                    + " (" + libusb_error_name ((int) t->status) + ")"
                    + "  errors=" + juce::String (transferErrors.load())
                    + "  completed=" + juce::String (transfersCompleted.load()));
                break;
        }

        // Re-arm
        if (! shouldStopCapture.load (std::memory_order_acquire))
        {
            t->length = OverbridgeProtocol::kTransferBytes;
            const int r = libusb_submit_transfer (t);
            if (r != LIBUSB_SUCCESS)
            {
                ++transferErrors;
                const int remaining = --activeTransferSlots;
                juce::Logger::writeToLog (
                    "OverbridgeEngine: re-arm FAILED: "
                    + juce::String (libusb_error_name (r))
                    + "  active slots remaining=" + juce::String (remaining));
                if (remaining == 0)
                    juce::Logger::writeToLog (
                        "OverbridgeEngine: ALL slots dead — stream stopped."
                        "  total bytes=" + juce::String ((int64_t) totalBytesReceived.load()));
            }
        }
    }

    // ── Decode interrupt buffer → float32 → audioCallback ────
    void decodeAndFire (const uint8_t* buf, int bytesReceived)
    {
        using namespace OverbridgeProtocol;

        // First-block diagnostic — dumps header bytes, verifies magic,
        // and reports the peak decoded float value of the first block.
        // Tells us definitively: are the raw bytes zero, or is the
        // decode formula wrong, or is the header offset wrong?
        if (kLogFirstBlock
            && ! firstBlockLogged.exchange (true, std::memory_order_acq_rel))
        {
            // ── Hex dump of first 64 raw bytes ─────────────────
            juce::String hex;
            const int dumpBytes = juce::jmin (bytesReceived, 64);
            for (int i = 0; i < dumpBytes; ++i)
                hex += juce::String::toHexString (buf[i]).paddedLeft ('0', 2) + " ";
            juce::Logger::writeToLog (
                "OverbridgeEngine: first transfer "
                + juce::String (bytesReceived) + " bytes");
            juce::Logger::writeToLog ("  raw[0..63]: " + hex);

            // ── Verify block header magic 0x0700 ───────────────
            if (bytesReceived >= 2)
            {
                const uint16_t magic = (uint16_t) ((buf[0] << 8) | buf[1]);
                juce::Logger::writeToLog (
                    "  block magic: 0x" + juce::String::toHexString (magic).toUpperCase()
                    + (magic == 0x0700 ? "  ✓ correct"
                       : magic == 0x07FF ? "  ✓ correct (TO-device magic, unexpected on IN)"
                       : "  ✗ UNEXPECTED — header size or offset may be wrong"));
            }

            // ── Sample counter (bytes 2-3, uint16 BE) ──────────
            if (bytesReceived >= 4)
            {
                const uint16_t counter = (uint16_t) ((buf[2] << 8) | buf[3]);
                juce::Logger::writeToLog (
                    "  sample counter: " + juce::String (counter));
            }

            // ── Hex dump of first audio bytes (after header) ───
            if (bytesReceived >= kBlockHeaderBytes + 16)
            {
                juce::String audioHex;
                for (int i = kBlockHeaderBytes; i < kBlockHeaderBytes + 16; ++i)
                    audioHex += juce::String::toHexString (buf[i]).paddedLeft ('0', 2) + " ";
                juce::Logger::writeToLog (
                    "  audio bytes [" + juce::String (kBlockHeaderBytes)
                    + ".." + juce::String (kBlockHeaderBytes + 15) + "]: " + audioHex);

                // Decode first sample of ch0 and ch1 manually
                for (int ch = 0; ch < juce::jmin (2, kNumChannels); ++ch)
                {
                    const uint8_t* b = buf + kBlockHeaderBytes + ch * kBytesPerSample;
                    const int32_t raw = (int32_t) (
                        ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) |
                        ((uint32_t) b[2] <<  8) |  (uint32_t) b[3]);
                    const float f = (float) raw * kInt32Norm;
                    juce::Logger::writeToLog (
                        "  ch" + juce::String (ch) + " frame0 raw=0x"
                        + juce::String::toHexString (raw).toUpperCase()
                        + "  float=" + juce::String (f, 8));
                }
            }

            // ── Peak absolute value across entire first transfer ─
            float peak = 0.0f;
            const int audioStart = kBlockHeaderBytes;
            const int audioEnd   = juce::jmin (bytesReceived,
                                               kBlockBytes);  // first block only
            for (int i = audioStart + 4; i + 3 < audioEnd; i += 4)
            {
                const int32_t raw = (int32_t) (
                    ((uint32_t) buf[i]   << 24) | ((uint32_t) buf[i+1] << 16) |
                    ((uint32_t) buf[i+2] <<  8) |  (uint32_t) buf[i+3]);
                const float f = std::abs ((float) raw * kInt32Norm);
                if (f > peak) peak = f;
            }
            juce::Logger::writeToLog (
                "  peak float in first block = " + juce::String (peak, 8)
                + (peak < 1e-6f
                   ? "  ← NEAR ZERO: raw audio bytes are zero or header offset is wrong"
                   : "  ← signal present, decode is working"));
        }

        const int framesPerTransfer = kFramesPerBlock * kBlocksPerTransfer;
        if ((int) scratchNonInterleaved.size() < kNumChannels * framesPerTransfer)
            return;

        int offset         = 0;
        int totalFrames    = 0;

        while (offset + kBlockBytes <= bytesReceived)
        {
            // Skip block header (32 bytes: 2 fixed + 2 counter + 28 unknown)
            const uint8_t* audio = buf + offset + kBlockHeaderBytes;

            for (int frame = 0; frame < kFramesPerBlock; ++frame)
            {
                const int outFrame = totalFrames + frame;
                if (outFrame >= framesPerTransfer) break;

                for (int ch = 0; ch < kNumChannels; ++ch)
                {
                    const uint8_t* b = audio
                        + (frame * kNumChannels + ch) * kBytesPerSample;

                    const int32_t raw = (int32_t) (
                        ((uint32_t) b[0] << 24) |
                        ((uint32_t) b[1] << 16) |
                        ((uint32_t) b[2] <<  8) |
                         (uint32_t) b[3]);

                    scratchPtrs[(size_t) ch][outFrame] =
                        (float) raw * kInt32Norm;
                }
            }

            totalFrames += kFramesPerBlock;
            offset      += kBlockBytes;
        }

        if (audioCallback && totalFrames > 0)
            audioCallback (scratchPtrs.data(), kNumChannels, totalFrames);
    }

    // ── USB helpers ───────────────────────────────────────────
    static bool isOBPid (uint16_t p) noexcept
    { for (auto x : kKnownOBPids)   if (p==x) return true; return false; }

    static bool isMidiPid (uint16_t p) noexcept
    { for (auto x : kKnownMidiPids) if (p==x) return true; return false; }

    bool probeBySyntaktName (libusb_device* dev,
                             const libusb_device_descriptor& desc) noexcept
    {
        if (! desc.iProduct) return false;
        libusb_device_handle* h = nullptr;
        if (libusb_open (dev, &h) != LIBUSB_SUCCESS) return false;
        unsigned char buf[128]{};
        const int r = libusb_get_string_descriptor_ascii (h, desc.iProduct, buf, 127);
        libusb_close (h);
        return r > 0 && juce::String ((const char*) buf).containsIgnoreCase ("Syntakt");
    }

    void closeDevice()
    {
        if (deviceHandle) { libusb_close (deviceHandle); deviceHandle = nullptr; }
        detectedPid = 0;
        if (state == DeviceState::Running) state = DeviceState::Ready;
    }

    void reattachKernelDriverIfNeeded()
    {
        if (kernelDriverDetached && deviceHandle)
        {
            libusb_attach_kernel_driver (deviceHandle,
                OverbridgeProtocol::kAudioInterface);
            kernelDriverDetached = false;
        }
    }

    // ── Members ───────────────────────────────────────────────
    libusb_context*       usbContext    { nullptr };
    libusb_device_handle* deviceHandle  { nullptr };

    DeviceState  state       { DeviceState::NotConnected };
    juce::String statusText;
    uint16_t     detectedPid { 0 };

    bool interfaceClaimed         { false };
    bool playbackInterfaceClaimed { false };
    bool kernelDriverDetached     { false };

    std::atomic<bool>     shouldStopCapture  { false };
    std::atomic<bool>     firstBlockLogged   { false };
    std::atomic<uint16_t> outSampleCounter   { 0 };    // increments by 7 per OUT block

    std::atomic<int>      transfersCompleted { 0 };
    std::atomic<int>      transferErrors     { 0 };
    std::atomic<int>      activeTransferSlots{ 0 };
    std::atomic<uint64_t> totalBytesReceived { 0 };

    std::vector<libusb_transfer*>     transfers;       // IN
    std::vector<std::vector<uint8_t>> transferBuffers; // IN

    std::vector<libusb_transfer*>     outTransfers;       // OUT (handshake)
    std::vector<std::vector<uint8_t>> outTransferBuffers; // OUT

    std::vector<float>  scratchNonInterleaved;
    std::vector<float*> scratchPtrs;

    std::unique_ptr<UsbThread> usbThread;

    AudioCallback audioCallback;
    MidiCallback  midiCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverbridgeEngine)
};

inline constexpr uint16_t OverbridgeEngine::kKnownOBPids[];
inline constexpr uint16_t OverbridgeEngine::kKnownMidiPids[];

#endif // JUCE_LINUX && JucePlugin_Build_Standalone