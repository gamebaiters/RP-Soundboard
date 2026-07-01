#pragma once

#include <cmath>
#include <algorithm>

class Compressor {
public:
    void setSampleRate(double sr);
    void setParams(float thresholdDb, float ratio, float attackMs, float releaseMs,
                   float kneeDb, float makeupDb);
    void processStereo(float &l, float &r);
    void reset();
    // Set the external sidechain envelope (0..~1.5 linear). When
    // greater than 1e-6 the internal envelope follower is bypassed in
    // favour of this value for a single processStereo call. Otherwise
    // the compressor tracks the input as usual.
    void feedSidechain(float envLinear) { m_scEnv = envLinear; }

private:
    double m_sampleRate = 48000.0;
    float m_thresholdDb = -20.0f;
    float m_ratio = 4.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;
    float m_kneeDb = 6.0f;
    float m_makeupGain = 1.0f;
    float m_envDb = -96.0f;
    float m_scEnv = 0.0f;
};
