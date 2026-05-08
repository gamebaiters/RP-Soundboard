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

private:
    double m_sampleRate = 48000.0;
    float m_thresholdDb = -20.0f;
    float m_ratio = 4.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;
    float m_kneeDb = 6.0f;
    float m_makeupGain = 1.0f;
    float m_envDb = -96.0f;
};
