// src/AudioUtils.h
//----------------------------------
// RP Soundboard Source Code
// Shared audio math helpers. Single source of truth for the dB<->linear
// conversions and the pitch/speed slider mapping that used to be
// duplicated across ~15 call sites (Compressor, Limiter, Positional,
// channel_meter, soundview, samples, fx wiring, ...).
//----------------------------------

#pragma once

#include <cmath>

namespace AudioUtils {

// Amplitude convention: gain = 10^(dB/20). This is the correct mapping
// for sample-domain (voltage-like) signals. Do NOT use 10^(dB/10) here -
// that is the POWER convention and doubles every dB applied to samples.
inline float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}
inline double dbToLinear(double db) {
    return std::pow(10.0, db / 20.0);
}

inline float linearToDb(float gain, float floorDb = -96.0f) {
    return (gain > 1e-6f) ? 20.0f * std::log10(gain) : floorDb;
}
inline double linearToDb(double gain, double floorDb = -96.0) {
    return (gain > 1e-9) ? 20.0 * std::log10(gain) : floorDb;
}

// FxPanel / legacy pitch+speed slider mapping: slider in [-100..100]
// maps exponentially to a factor in [1/3 .. 3].
inline float sliderToPitchFactor(int sliderValue) {
    return static_cast<float>(std::pow(3.0, sliderValue / 100.0));
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace AudioUtils
