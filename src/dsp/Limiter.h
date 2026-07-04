#pragma once

#include <cmath>
#include <algorithm>

class Limiter {
public:
    enum Mode { LimiterMode = 0, CompressorMode = 1, GateMode = 2 };

    void setSampleRate(double sr);
    void setParams(float ceilingDb, float lookaheadMs, float releaseMs,
                   Mode mode, float ratio, float gateThreshDb);
    // True-peak sidechain (Q3): estimates inter-sample peaks via 4x
    // Catmull-Rom interpolation so the limiter catches ISP overshoots
    // a sample-peak detector misses (typically up to ~1 dB on hot
    // band-limited material). Default ON via SandboxState::truePeakMode.
    void setTruePeak(bool on) { m_truePeak = on; }
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxLookahead = 480;

    double m_sampleRate = 48000.0;
    float m_ceiling = 0.95f;
    int m_lookaheadSamples = 48;
    float m_releaseCoeff = 0.0f;
    Mode m_mode = LimiterMode;
    float m_ratio = 100.0f;
    float m_gateThresh = 0.001f;
    float m_gateAttenuationDb = -80.0f;

    float m_delayL[kMaxLookahead] = {};
    float m_delayR[kMaxLookahead] = {};
    int m_delayPos = 0;

    float m_gainDb = 0.0f;

    float m_peakBuf[kMaxLookahead] = {};

    // True-peak estimation state: last 4 raw samples per channel for
    // the Catmull-Rom inter-sample interpolator.
    bool  m_truePeak = true;
    float m_tpHistL[4] = {};
    float m_tpHistR[4] = {};
    int   m_tpIdx = 0;
    float truePeakOf(const float *h) const;
};
