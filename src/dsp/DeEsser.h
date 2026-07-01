#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>

// De-esser: HF band-limited compressor. A narrow bandpass biquad
// isolates the sibilance region (default 6.5 kHz). An envelope
// follower tracks the band energy. When the envelope exceeds the
// threshold, gain reduction is applied to the FULL signal
// (side-chained compressor), which pulls the whole spectrum down
// briefly - the classical broadcast/vocal de-ess response.
//
// Optional cross-slot sidechain: when a valid Sampler-side envelope
// (0..~1.5) is fed via feedSidechain(v), the internal envelope is
// replaced with that value on the next process(). Enables one slot's
// energy (typically a voiceover) to drive de-essing on another slot.
class DeEsser {
public:
    void setSampleRate(double sr);
    // freqHz: center of the sibilance band (default 6500).
    // qFactor: bandpass Q (default 3.0, narrower for surgical).
    // thresholdDb: envelope threshold above which gain reduction fires.
    // rangeDb: max gain reduction (0..-30 dB).
    // attackMs / releaseMs: envelope smoothing constants.
    void setParams(float freqHz, float qFactor, float thresholdDb,
                   float rangeDb, float attackMs, float releaseMs);
    void processStereo(float &l, float &r);
    void reset();

    // Feed a sidechain envelope value from an external source
    // (e.g. another slot). Zero or negative disables sidechain and
    // reverts to the internal envelope follower.
    void feedSidechain(float envLinear) { m_scEnv = envLinear; }

private:
    double m_sampleRate = 48000.0;

    // Bandpass biquad (RBJ constant-0-dB peak). Direct Form I.
    double m_b0 = 1.0, m_b1 = 0.0, m_b2 = 0.0;
    double m_a1 = 0.0, m_a2 = 0.0;
    double m_xL1 = 0.0, m_xL2 = 0.0, m_yL1 = 0.0, m_yL2 = 0.0;
    double m_xR1 = 0.0, m_xR2 = 0.0, m_yR1 = 0.0, m_yR2 = 0.0;

    float m_thresholdDb = -30.0f;
    float m_rangeDb     = -12.0f;
    float m_attackCoeff  = 0.0f;
    float m_releaseCoeff = 0.0f;
    float m_envDb = -96.0f;

    float m_scEnv = 0.0f;  // sidechain override (linear); 0 = use internal
};
