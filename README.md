A MIDI standalone app and VST3 plugin to interact with Elektron Syntakt (or other synth if you edit MIDI CC/NRPN mapping)

<img width="1427" height="726" alt="Screenshot from 2026-03-08 05-07-50" src="https://github.com/user-attachments/assets/52d71cbc-ca72-41bc-bb23-a93e77b804f4" />

- one LFO with routing to up to 3 MIDI channels (so up to 3 Syntakt tracks can share the same LFO, with independent CC destination for each track).
-- LFO can be synced to MIDI clock
-- Note-On trig/re-trig option / Stop on Note-Off option


- one AHDSR Envelop Generator with linear/exponential/log curves with routing to up to 3 MIDI channels or CCs
-- "Long" mode for Attack and Release (for pads and texture)
-- EG can also modulate LFO depth or/and rate and shape notes from Delay


- a MIDI notes delay with routing to up to 3 MIDI channels.
-- Delay can be synced to MIDI clock
-- Per channel transpose function
-- EG can be applied to delay, either per echoed note or one time

- the Oscilloscope view is gadget, not accurate.

LFO route triggered by EG always run until end of EG cycle.

When launching the app for the first time, use Options -> "Reset to default state" first.

"vibe-coded" with AI
