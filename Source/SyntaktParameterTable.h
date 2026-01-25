#pragma once
#include <JuceHeader.h>

struct SyntaktParameter
    {
        const char* name;
        bool isCC;       // true = CC, false = NRPN
        int ccNumber;    // if isCC
        int nrpnLsb;     // if NRPN
        int nrpnMsb;     // if NRPN
        int minValue;    // min mapped value
        int maxValue;    // max mapped value
        bool isBipolar;  // centered params
        bool egDestination; // available in EG dest selector
    };


static const SyntaktParameter syntaktParameters[] = {

    // Track parameters
    {"Pattern Mute", true, 110, 104, 1, 0, 1, false},
    {"Track Mute", true, 94, 101, 1, 0, 1, false, false},
    {"Track Level", true, 95, 100, 1, 0, 127, false, true},

    // SYN parameters
    {"Knob A", true, 17, 1, 1, 0, 127, false, true},
    {"Knob B", true, 18, 2, 1, 0, 127, false, true},
    {"Knob C", true, 19, 3, 1, 0, 127, false, true},
    {"Knob D", true, 20, 4, 1, 0, 127, false, true},
    {"Knob E", true, 21, 5, 1, 0, 127, false, true},
    {"Knob F", true, 22, 6, 1, 0, 127, false, true},
    {"Knob G", true, 23, 7, 1, 0, 127, false, true},
    {"Knob H", true, 24, 8, 1, 0, 127, false, true},

    // Filter parameters (max nrpn: 16256 )
    {"Filter: Frequency", false, 74, 20, 1, 0, 16256, false, true},
    {"Filter: Resonance", false, 75, 21, 1, 0, 16256, false, true},
    {"Filter: Type",      true, 76, 22, 1, 0, 7, false, false},
    {"Filter: Attack Time", true, 70, 16, 1, 0, 127, false, false},
    {"Filter: Decay Time",  true, 71, 17, 1, 0, 127, false, false},
    {"Filter: Sustain Level", true, 72, 18, 1, 0, 127, false, false},
    {"Filter: Release Time",  true, 73, 19, 1, 0, 127, false, false},
    {"Filter: Envelope Depth", false, 77, 23, 1, 0, 16256, true, true},
    {"Filter: Envelope Delay", true, 78, 52, 1, 0, 127, false, false},
    {"Filter: Envelope Reset", true, 111, 56, 1, 0, 1, false, false},
    {"Filter: Base",       true, 26, 50, 1, 0, 127, false, false},
    {"Filter: Width",      true, 27, 51, 1, 0, 127, false, false},

    // Amp parameters
    {"Amp: Attack Time",   true, 79, 48, 1, 0, 127, false, false},
    {"Amp: Hold Time",   true, 80, 25, 1, 0, 127, false, false},
    {"Amp: Decay Time",   true, 81, 26, 1, 0, 127, false, false},
    {"Amp: Sustain Level",   true, 82, 49, 1, 0, 127, false, false},
    {"Amp: Release Time",   true, 83, 24, 1, 0, 127, false, false},
    {"Amp: Delay Send",   false, 84, 28, 1, 0, 16256, false, true},
    {"Amp: Reverb Send",   false, 85, 29, 1, 0, 16256, false, true},
    {"Amp: Pan",   true, 10, 30, 1, 0, 127, true, true},
    {"Amp: Volume",   false, 7, 31, 1, 0, 16256, false, true},

    // LFO 1 parameters
    {"LFO 1: Speed",   false, 102, 32, 1, 0, 16383, true, true}, // to check
    {"LFO 1: Multiplier",   true, 103, 33, 1, 0, 23, false, false},
    {"LFO 1: Fade In/Out",   true, 104, 34, 1, 0, 127, true, false},
    {"LFO 1: Destination",   true, 105, 35, 1, 0, 127, false, false},
    {"LFO 1: Waveform",   true, 106, 36, 1, 0, 127, false, false},
    {"LFO 1: Start Phase",   true, 107, 37, 1, 0, 127, false, false},
    {"LFO 1: Trig Mode",   true, 108, 38, 1, 0, 127, false, false},
    {"LFO 1: Depth",   false, 109, 39, 1, 0, 16383, true, true},  // to check

    // LFO 2 parameters
    {"LFO 2: Speed",   false, 112, 40, 1, 0, 16383, true, true}, // to check
    {"LFO 2: Multiplier",   true, 113, 41, 1, 0, 23, false, false},
    {"LFO 2: Fade In/Out",   true, 114, 42, 1, 0, 127, false, false},
    {"LFO 2: Destination",   true, 115, 43, 1, 0, 127, false, false},
    {"LFO 2: Waveform",   true, 116, 44, 1, 0, 127, false, false},
    {"LFO 2: Start Phase",   true, 117, 45, 1, 0, 127, false, false},
    {"LFO 2: Trig Mode",   true, 118, 46, 1, 0, 127, false, false},
    {"LFO 2: Depth",   false, 119, 47, 1, 0, 16383, true, true}  // to check

    };

constexpr size_t numSyntaktParameters =
    sizeof(syntaktParameters) / sizeof(SyntaktParameter);

