A MIDI standalone app and VST3 plugin to interact with Elektron Syntakt (or other synth if you edit MIDI CC routing)


- one LFO routable up to 3 MIDI channels (so up to 3 Syntakt tracks can share the same LFO, with independent CC destination for each track).
-- LFO can be synced to MIDI clock
-- Note-On trig/re-trig option / Stop on Note-Off option

- one Envelop Generator with linear/exponential/log curves
-- "Long" mode for Attack and Release (for pads and texture)

- the Oscilloscope view is a gadget, not accurate at high LFO rates

- LFO Start/Stop UI button may not reflect LFO running state if EG is set to modulate an LFO route ( EG set to "EG to LFO route_x ): LFO route triggered by EG always run until end of EG cycle.

- conflicting modulation pathes may still be possible, I hunt them all unless they sound like a new feature.

Mostly "vibe-coded" with ChatGPT and Claude.ai


