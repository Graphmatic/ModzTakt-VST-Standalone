A MIDI standalone app and VST3 plugin to interact with Elektron Syntakt (or other synth if you edit MIDI CC routing)


- one LFO with routing to up to 3 MIDI channels (so up to 3 Syntakt tracks can share the same LFO, with independent CC destination for each track).
-- LFO can be synced to MIDI clock
-- Note-On trig/re-trig option / Stop on Note-Off option

- one AHDSR Envelop Generator with linear/exponential/log curves with routing to up to 3 MIDI channels or CCs
-- "Long" mode for Attack and Release (for pads and texture)
-- EG can also modulate LFO depth or/and rate

- the Oscilloscope view is gadget, not accurate.

LFO route triggered by EG always run until end of EG cycle.

"vibe-coded" with AI
