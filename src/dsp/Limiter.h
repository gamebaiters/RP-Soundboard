#pragma once

#include <cmath>
#include <algorithm>

class Limiter {
public:
    enum Mode { LimiterMode = 0, CompressorMode = 1, GateMode = 2 };

    void setSampleRate(double sr);
    void setParams(float ceilingDb, float lookaheadMs, float releaseMs,
                   Mode mode, float ratio, float gateThreshDb);
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
};
