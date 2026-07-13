#pragma once

#include "BiquadPeaking.h"
#include <cmath>
#include <algorithm>

// Dynamic EQ: 4-band peaking filter whose gain per band is driven by an
// envelope follower on a filtered COPY of the signal at that band's
// centre frequency. Below the per-band threshold the gain stays at the
// static setting; above threshold, an extra dynamic gain (negative for
// compression, positive for expansion) is added based on how much the
// envelope exceeds the threshold, scaled by ratio.
//
// Same threshold / ratio semantics as a standard downward compressor,
// but applied SPECTRALLY: only the frequency band around the centre
// gets attenuated. Useful for taming a nasal midrange resonance that
// only shows up on loud passages, or leaning into a sub-bass region
// only when it is present.
class DynEq {
public:
    static constexpr int kNumBands = 4;

    struct BandConfig {
        float freq        = 200.0f;
        float q           = 1.4f;
        float staticGainDb = 0.0f;   // baseline peaking EQ gain
        float thresholdDb  = -30.0f;  // envelope threshold
        float ratio        = 2.0f;    // 1..20, compression ratio for dynamic gain
        float dynamicDb    = -6.0f;   // total dynamic range (negative = compress)
        float attackMs     = 15.0f;
        float releaseMs    = 150.0f;
        bool  enabled      = false;
    };

    DynEq();
    void setSampleRate(double sr);
    void setBand(int b, const BandConfig &cfg);
    BandConfig band(int b) const;
    void processStereo(float &l, float &r);
    void reset();

private:
    double m_sampleRate = 48000.0;

    BandConfig      m_cfg[kNumBands];
    // Static + dynamic biquads per channel. The dynamic biquad's gain
    // is updated per-sample as (staticGainDb + dynamicGainDb).
    BiquadPeaking   m_bpL[kNumBands];
    BiquadPeaking   m_bpR[kNumBands];
    // Sidechain analyser: separate peaking biquad tuned to the same
    // centre + Q but with +12 dB gain, used purely to isolate the
    // band's magnitude for the envelope follower.
    BiquadPeaking   m_scL[kNumBands];
    BiquadPeaking   m_scR[kNumBands];
    float m_env[kNumBands]         = {0};
    float m_lastGainDb[kNumBands]  = {0};
    float m_attackCoeff[kNumBands] = {0};
    float m_releaseCoeff[kNumBands]= {0};
    // Control-rate divider for updateBandGain. Per-instance: the old
    // function-local static was shared across every DynEq instance AND
    // every band (and raced across audio threads), so the update cadence
    // per band was erratic under multi-channel load.
    int   m_updateCounter          = 0;

    void recomputeBand(int b);
    void updateBandGain(int b);
};
