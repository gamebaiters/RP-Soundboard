#pragma once

#include <cmath>
#include <algorithm>

class Flanger {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depth, float feedback, float baseDelayMs, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxBufSize = 4800;

    double m_sampleRate = 48000.0;
    float m_rate = 0.5f;
    float m_depth = 0.7f;
    float m_feedback = 0.5f;
    float m_baseDelay = 2.0f;
    float m_mix = 0.5f;

    float m_bufL[kMaxBufSize] = {};
    float m_bufR[kMaxBufSize] = {};
    int m_writePos = 0;
    double m_phase = 0.0;

    float m_lastOutL = 0.0f;
    float m_lastOutR = 0.0f;
};
