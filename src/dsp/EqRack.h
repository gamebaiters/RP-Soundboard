#pragma once

#include "BiquadPeaking.h"

// 16-band graphic EQ. Centers track the ISO 2/3-octave grid (anchored
// to 1 kHz) plus a 20 Hz extension at the bottom so the rack covers the
// full 20 Hz - 16 kHz range. Q = 2.145 matches 2/3-octave spacing so
// adjacent +/-12 dB bands sum to ~+/-12 dB at the seam (no earrape).
class EqRack {
public:
    static constexpr int kNumBands = 16;
    static constexpr float kQ = 2.145f;
    static constexpr float kMinDb = -12.0f;
    static constexpr float kMaxDb = +12.0f;

    EqRack();
    void setSampleRate(double sr);
    void setBandGainDb(int band, float gainDb);
    float bandGainDb(int band) const;
    static double bandFrequency(int band);

    void processStereo(float &l, float &r);
    void reset();

    // Sum of clamped positive band gains in dB, used by the master stage
    // to apply automatic make-up so a bunch of upward sliders cannot
    // push the output into hard clipping.
    float positiveSumDb() const;

private:
    void recompute(int band);

    double m_sampleRate = 48000.0;
    float  m_gainDb[kNumBands] = {0};
    BiquadPeaking m_left[kNumBands];
    BiquadPeaking m_right[kNumBands];
};
