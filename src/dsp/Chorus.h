#pragma once

#include <cmath>
#include <algorithm>

class Chorus {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depthMs, float baseDelayMs, int voices, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxVoices = 4;
    static constexpr int kMaxBufSize = 48000;

    double m_sampleRate = 48000.0;
    float m_rate = 1.0f;
    float m_depth = 3.0f;
    float m_baseDelay = 10.0f;
    int m_voices = 2;
    float m_mix = 0.5f;

    float m_bufL[kMaxBufSize] = {};
    float m_bufR[kMaxBufSize] = {};
    int m_writePos = 0;
    int m_bufSize = 0;

    double m_phase[kMaxVoices] = {};
};
