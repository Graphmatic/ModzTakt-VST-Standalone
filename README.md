A MIDI standalone app and VST3 plugin to interact with Elektron Syntakt (or other synth if you edit MIDI CC/NRPN mapping)

Linux standalone is also available with OverBridge mode per track Audio recording.

<img width="1432" height="963" alt="Screenshot from 2026-03-20 17-58-30" src="https://github.com/user-attachments/assets/3c965250-e7e3-48f5-9fe1-d30b5be890ea" />

If you want to use/compile the Linux Standalone version with OB audio recording, install "libusb-1.0" from terminal first and ensure that user have permission on real time audio:
Create or edit /etc/udev/rules.d/99-elektron-overbridge.rules:
SUBSYSTEM=="usb", ATTR{idVendor}=="1935", MODE="0664", GROUP="audio"
Then:
sudo udevadm control --reload && sudo udevadm trigger
sudo usermod -aG audio $USER   (re-login after)

to compile the linux standalone version with OB audio, use the separate Makefile available in Builds/MakefileOverbridge folder.

- one LFO with routing to up to 3 MIDI channels (so up to 3 Syntakt tracks can share the same LFO, with independent CC destination for each track).
-- LFO can be synced to MIDI clock
-- Note-On trig/re-trig option / Stop on Note-Off option
-- LFO Depth and Rate can be shaped by EG


- one AHDSR Envelop Generator with linear/exponential/log curves with routing to up to 3 MIDI channels or CCs
-- "Long" mode for Attack and Release (for pads and texture)
-- EG can also modulate LFO depth or/and rate and shape notes from Delay
-- For Linux standalone version with OB, EG can be driven from an Audio Envelope Follower using OB tracks 1-12. 


- a MIDI notes delay with routing to up to 3 MIDI channels.
-- Delay can be synced to MIDI clock
-- Per channel transpose function
-- EG can be applied to delay, either per echoed note or one time
-- Step sequencer to mute/unmute echoed notes
-- Auto-Pan for echoed notes

- a graphical interface to record Syntakt audio channels for the Linux Standalone version if SYNTAKT is set to OverBridge Mode. Code is ripped off the existing Dtdump project (https://github.com/droelfdroelf/dtdump), so repectfull thanks, I won't be able to do that without this previous work! Syntakt MIDI In/Out seems to be available independantly, so using audio recording should not break MIDI functionnalities of the app. 

- the Oscilloscope view is gadget, not accurate.

LFO route triggered by EG always run until end of EG cycle.

When launching the app for the first time, use Options -> "Reset to default state" first.

Syntakt configuration:
in Settings->MIDI config->Port Config: 
	-Input from / Output to = USB (or MIDI+USB)
	-Output CH = TRK CH
	-Trig Key DST = INT+EXT
	-Receive Notes = YES
	-Receive CC/NRPN = YES
in Settings->MIDI config->Channels:
	-Track 1-12 = Channel 1-12
in Settings->MIDI config->Sync:
	-Clock Send = YES
	-Transport Send = YES

Edit MIDI mapping and parameters names in SyntaktParameterTable.h to use the app with others synths

"vibe-coded" with AI (more some human debugging)
